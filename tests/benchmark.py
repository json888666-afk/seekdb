#!/usr/bin/env python3
"""Local end-to-end diagnostic; never reports a formal score or cgroup metrics."""
import heapq
import json
from pathlib import Path
import random
import tempfile
import time

from test_engine import Session, f32, literal


def main():
    rng=random.Random(600081)
    vectors=[[f32(rng.uniform(-1,1)) for _ in range(128)] for _ in range(10000)]
    queries=[[f32(rng.uniform(-1,1)) for _ in range(128)] for _ in range(1000)]
    with tempfile.TemporaryDirectory(prefix='minivector-bench-') as directory:
        db=Session(directory,cache_mb=4)
        db.request('CREATE TABLE t(id BIGINT PRIMARY KEY,g VARCHAR(8),v VECTOR(128));')
        start=time.monotonic()
        for base in range(0,10000,100):
            db.request('INSERT INTO t VALUES '+','.join(
                f"({i},'g{i%10}',{literal(vectors[i])})" for i in range(base,base+100))+';')
        write_seconds=time.monotonic()-start
        start=time.monotonic()
        db.request('CREATE VECTOR INDEX idx ON t(v);')
        index_seconds=time.monotonic()-start
        # Independent Python oracle, including float32 query conversion and filtering.
        import math
        expected={}
        for i in range(1000):
            pool=range(10000) if i%2==0 else range(i%10,10000,10)
            expected[i]=heapq.nsmallest(10,((math.dist(vectors[k],queries[i]),k) for k in pool))
        responses=[]
        start=time.monotonic()
        for i,query in enumerate(queries):
            where='' if i%2==0 else f" WHERE g='g{i%10}'"
            sql=f'SELECT id,L2_DISTANCE(v,{literal(query)}) AS d FROM t{where} ORDER BY d LIMIT 10;'
            responses.append(db.request(sql)['rows'])
        query_seconds=time.monotonic()-start
        for i,truth in expected.items():
            assert [r[0] for r in responses[i]]==[r[1] for r in truth]
            assert all(math.isclose(got[1],want[0],rel_tol=1e-10,abs_tol=1e-10)
                       for got,want in zip(responses[i],truth))
        with open(f'/proc/{db.proc.pid}/status') as f:
            status=f.read()
        peak=next(line for line in status.splitlines() if line.startswith('VmHWM:'))
        size=sum(p.stat().st_size for p in Path(directory).iterdir())
        db.restart(kill=True)
        assert len(db.request('SELECT id FROM t;')['rows'])==10000
        db.stop()
        print(json.dumps(dict(scope='local-diagnostic-only',seed=600081,rows=10000,dimension=128,
             batch_size=100,queries=1000,k=10,filtered_queries=500,oracle_checked_queries=len(expected),
             write_seconds=round(write_seconds,6),index_seconds=round(index_seconds,6),
             query_seconds=round(query_seconds,6),process_peak_rss=peak,database_bytes=size,
             passed=True,note='WSL local wall time; includes Python request construction. Not official T/B/M or score.')))


if __name__=='__main__':
    main()
