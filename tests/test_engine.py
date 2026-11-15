#!/usr/bin/env python3
"""Black-box regression/property tests against the actual JSONL process."""
import argparse
import copy
import json
import math
import os
from pathlib import Path
import random
import resource
import select
import signal
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('MINIVECTORDB_BINARY', ROOT / 'build/minivectordb')).resolve()


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def literal(x):
    if isinstance(x, str):
        return "'" + x.replace('\\', '\\\\').replace("'", "''") + "'"
    if isinstance(x, list):
        return literal(json.dumps(x, separators=(',', ':')))
    return str(x)


class Session:
    def __init__(self, directory, memory_mb=None, cache_mb=1):
        self.directory = str(directory)
        self.memory_mb = memory_mb
        self.cache_mb = cache_mb
        self.ident = 0
        self.start()

    def start(self):
        def limit():
            if self.memory_mb:
                resource.setrlimit(resource.RLIMIT_AS, (self.memory_mb*1024**2,)*2)
        self.proc = subprocess.Popen(
            [str(BINARY), '--data-dir', self.directory, '--cache-mb', str(self.cache_mb)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            preexec_fn=limit)
        assert self.read() == {'type': 'ready'}

    def read(self):
        if not select.select([self.proc.stdout], [], [], 60)[0]:
            raise TimeoutError('JSON response timeout')
        line = self.proc.stdout.readline()
        if not line:
            raise AssertionError('process exited: '+self.proc.stderr.read().decode())
        return json.loads(line)

    def request(self, sql, mode='exact', error=None):
        self.ident += 1
        request = dict(id=self.ident, op='execute', sql=sql, search_mode=mode)
        self.proc.stdin.write(json.dumps(request, ensure_ascii=False).encode()+b'\n')
        self.proc.stdin.flush()
        response = self.read()
        assert response.get('id') == self.ident, response
        assert type(response.get('ok')) is bool, response
        if error:
            assert response['ok'] is False, (sql, response)
            assert response['error']['code'] == error, (sql, response)
        else:
            assert response['ok'] is True, (sql, response)
        return response

    def stop(self, kill=False):
        if self.proc.poll() is None:
            if kill:
                self.proc.kill()
            else:
                self.proc.stdin.write(b'{"id":"shutdown","op":"shutdown"}\n')
                self.proc.stdin.flush()
                assert self.read() == {'id': 'shutdown', 'ok': True}
            self.proc.wait(timeout=10)
            if not kill:
                assert self.proc.returncode == 0, self.proc.stderr.read().decode()
                assert not self.proc.stdout.read(), 'extra output after shutdown'
        self.proc.stdin.close()
        self.proc.stdout.close()
        self.proc.stderr.close()

    def restart(self, kill=False):
        self.stop(kill)
        self.start()


class EngineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='minivector-test-')
        self.db = Session(self.tmp.name)

    def tearDown(self):
        self.db.stop()
        self.tmp.cleanup()

    def test_types_sql_and_statement_atomicity(self):
        q = self.db.request
        q('CREATE TABLE t (s VARCHAR(4), id BIGINT, v VECTOR(2), PRIMARY KEY(id));')
        q("INSERT INTO t (id,v,s) VALUES (-9223372036854775808,'[0,0]','你好😀'),(9223372036854775807,'[3,4]','é');")
        rows = q('SELECT * FROM t ORDER BY id;')['rows']
        self.assertEqual(rows[0], ['你好😀', -(2**63), [0,0]])
        q("INSERT INTO t VALUES ('abcde',1,'[0,0]');", error='value_too_long')
        q("INSERT INTO t VALUES ('a',9223372036854775808,'[0,0]');", error='value_out_of_range')
        for vector in ('[NaN,0]', '[Infinity,0]', '[1e39,0]', '[1e999,0]', '[1]', '[1,2,3]', '[true,0]', '[null,0]', '{}'):
            q("INSERT INTO t VALUES ('a',1,"+literal(vector)+");", error='invalid_vector')
        q("BEGIN;")
        q("INSERT INTO t VALUES ('x',1,'[0,0]');")
        q("INSERT INTO t VALUES ('x',2,'[0,0]'),('x',1,'[0,0]');", error='duplicate_key')
        q("UPDATE t SET s='bad!' WHERE id>=1; ALTER TABLE t ADD x BIGINT;", error='unsupported_sql')
        q('UPDATE t SET id=9 WHERE id=1;', error='unsupported_sql')
        q('COMMIT;')
        self.db.restart(kill=True)
        self.assertEqual(q('SELECT id FROM t ORDER BY id;')['rows'], [[-2**63],[1],[2**63-1]])
        q('SELECT missing FROM t WHERE id=19;', error='unknown_column')
        q('SELECT id FROM t WHERE id < -9223372036854775808;')
        self.assertEqual(q('SELECT id FROM t WHERE id > 9223372036854775807;')['rows'], [])
        self.assertEqual(q('SELECT id FROM t LIMIT 0;')['rows'], [])

    def test_lexer_and_unicode_order(self):
        q = self.db.request
        q('/* prefix */ CREATE TABLE `T` (`id` BIGINT PRIMARY KEY, s VARCHAR(40));')
        values = ["a; DROP TABLE t;", "it's", '甲', '😀', 'é', '\\n', 'e', '乙', 'a\nb']
        q('INSERT INTO t VALUES '+','.join(f'({i},{literal(s)})' for i,s in enumerate(values))+'; -- comment')
        rows = q('SELECT s FROM t ORDER BY s;')['rows']
        self.assertEqual(rows, [[s] for s in sorted(values)])
        q("INSERT INTO t VALUES (99,'valid'); SELECT * FROM t;", error='unsupported_sql')
        self.assertEqual(q('SELECT id FROM t WHERE id=99;')['rows'], [])
        q('CREATE TABLE MixedCase(ID BIGINT PRIMARY KEY, Content VARCHAR(8));')
        q("INSERT INTO mixedcase VALUES (1,'中文');")
        self.assertEqual(q('SELECT id,CONTENT FROM MIXEDCASE;')['columns'], ['ID','Content'])
        self.assertEqual(q('SELECT id AS KeyName FROM mixedcase ORDER BY keyname;')['columns'], ['KeyName'])

    def test_large_failed_statement_and_eviction(self):
        q = self.db.request
        q('CREATE TABLE t(id BIGINT PRIMARY KEY, s VARCHAR(64));')
        q('BEGIN;')
        q("INSERT INTO t VALUES (-1,'keep');")
        # More COW pages than the 1 MiB cache; evicted pages must also roll back.
        sql='INSERT INTO t VALUES '+','.join(f"({i},'row')" for i in range(30000))+",(-1,'duplicate');"
        q(sql, error='duplicate_key')
        self.assertEqual(q('SELECT id FROM t ORDER BY id;')['rows'], [[-1]])
        q('COMMIT;')
        self.db.restart(kill=True)
        self.assertEqual(q('SELECT * FROM t;')['rows'], [[-1,'keep']])

    def test_randomized_index_transactions_restarts(self):
        q = self.db.request
        rng = random.Random(20261115)
        q('CREATE TABLE t(id BIGINT PRIMARY KEY, g VARCHAR(8), v VECTOR(7));')
        model = {}
        for start in range(0,5200,200):
            rows=[]
            for i in range(start,start+200):
                vector=[f32(rng.uniform(-8,8)) for _ in range(7)]
                model[i]=[i,'keep' if i%3 else 'skip',vector]
                rows.append('('+','.join(literal(x) for x in model[i])+')')
            q('INSERT INTO t VALUES '+','.join(rows)+';')
        q('CREATE VECTOR INDEX idx ON t(v);')

        def verify():
            target=[f32(rng.uniform(-3,3)) for _ in range(7)]
            eligible=[r for r in model.values() if r[1]=='keep' and r[0]>=100]
            expected=sorted(((math.dist(r[2],target),r[0]) for r in eligible))[:13]
            sql=f"SELECT id,L2_DISTANCE(v,{literal(target)}) AS d FROM t WHERE g='keep' AND id>=100 ORDER BY d LIMIT 13;"
            for mode in ('exact','ann'):
                actual=q(sql,mode)['rows']
                self.assertEqual([x[0] for x in actual], [x[1] for x in expected])
                for got,want in zip(actual,expected):
                    self.assertAlmostEqual(got[1],want[0],places=9)

        verify()
        for iteration in range(12):
            backup=copy.deepcopy(model)
            q('BEGIN;')
            for step in range(8):
                key=rng.randrange(5000)
                if key not in model:
                    continue
                if step%3==0:
                    q(f'DELETE FROM t WHERE id={key};')
                    del model[key]
                else:
                    vector=[f32(rng.uniform(-4,4)) for _ in range(7)]
                    q(f'UPDATE t SET v={literal(vector)},g=\'keep\' WHERE id={key};')
                    model[key]=[key,'keep',vector]
            newkey=6000+iteration
            row=[newkey,'keep',[f32(iteration/7)]*7]
            q('INSERT INTO t VALUES ('+','.join(literal(x) for x in row)+');')
            model[newkey]=row
            verify()  # read-your-writes, including the vector delta
            if iteration%3==0:
                q('ROLLBACK;')
                model=backup
            elif iteration%3==1:
                q('COMMIT;')
                self.db.restart(kill=True)
            else:
                self.db.restart(kill=True)
                model=backup
            verify()
        self.assertEqual(q('SELECT id,g FROM t ORDER BY g DESC,id DESC LIMIT 33;')['rows'],
                         [[r[0],r[1]] for r in sorted(model.values(),key=lambda r:(r[1],r[0]),reverse=True)[:33]])

    def test_index_extreme_floats_and_ties(self):
        q=self.db.request
        q('CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2));')
        q("INSERT INTO t VALUES (9,'[0,0]'),(2,'[0,0]'),(7,'[3.4028234663852886e38,0]'),(8,'[-3.4028234663852886e38,0]');")
        q('CREATE VECTOR INDEX idx ON t(v);')
        self.assertEqual(q("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 4;")['rows'], [[2],[9],[7],[8]])
        q('DELETE FROM t WHERE id=2;')
        q("INSERT INTO t VALUES (2,'[1,0]');")
        self.assertEqual(q("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 2;")['rows'], [[9],[2]])

    def test_transactional_schema_and_recovery_interruption(self):
        q=self.db.request
        q('BEGIN;')
        q('CREATE TABLE discard(id BIGINT PRIMARY KEY);')
        q('ROLLBACK;')
        q('SELECT * FROM discard;',error='unknown_table')
        q('CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2));')
        q("INSERT INTO t VALUES (1,'[0,0]');")
        q('BEGIN;')
        q('CREATE VECTOR INDEX discard_idx ON t(v);')
        q("INSERT INTO t VALUES (2,'[1,0]');")
        self.db.restart(kill=True)
        q('CREATE VECTOR INDEX discard_idx ON t(v);')
        self.assertEqual(q('SELECT id FROM t;')['rows'],[[1]])
        self.db.stop(kill=True)
        for _ in range(10):
            p=subprocess.Popen([str(BINARY),'--data-dir',self.tmp.name],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            time.sleep(0.0005)
            p.kill();p.wait();p.stdin.close()
        self.db.start()
        self.assertEqual(q('SELECT id FROM t;')['rows'],[[1]])

    def test_external_sort_and_equal_keys(self):
        q=self.db.request
        q('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(180));')
        text='x'*160
        for start in range(0,70000,1000):
            q('INSERT INTO t VALUES '+','.join(f"({i},'{text}{(i*31)%97:02d}')" for i in range(start,start+1000))+';')
        rows=q('SELECT id FROM t ORDER BY s DESC;')['rows']
        expected=sorted(range(70000),key=lambda i:(-((i*31)%97),i))
        self.assertEqual(rows,[[x] for x in expected])


def large_memory_check():
    """Database payload exceeds the process address space cap; no all-row load."""
    with tempfile.TemporaryDirectory(prefix='minivector-large-') as directory:
        db=Session(directory,memory_mb=96,cache_mb=4)
        start=time.monotonic()
        db.request('CREATE TABLE t(id BIGINT PRIMARY KEY,g VARCHAR(8),v VECTOR(256));')
        vector=literal([1.0]*256)
        count=150000
        for base in range(0,count,200):
            sql='INSERT INTO t VALUES '+','.join(f"({i},'keep',{vector})" for i in range(base,base+200))+';'
            db.request(sql)
        db.request('CREATE VECTOR INDEX idx ON t(v);')
        db.restart(kill=True)
        result=db.request('SELECT id,L2_DISTANCE(v,'+vector+") AS d FROM t WHERE g='keep' ORDER BY d LIMIT 10;")
        assert result['rows']==[[i,0.0] for i in range(10)],result
        size=sum(p.stat().st_size for p in Path(directory).iterdir())
        assert size>96*1024**2
        db.stop()
        report=dict(rows=count,dimension=256,address_space_limit_mib=96,cache_mib=4,
                    database_bytes=size,elapsed_seconds=round(time.monotonic()-start,3),passed=True)
        print(json.dumps(report),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--large-memory',action='store_true')
    args,rest=parser.parse_known_args()
    if args.large_memory:
        large_memory_check()
    else:
        unittest.main(argv=[__file__]+rest,verbosity=2)
