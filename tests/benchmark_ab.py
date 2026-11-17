#!/usr/bin/env python3
"""Alternating same-host A/B with requests prepared outside the timed region."""
import argparse
import hashlib
import heapq
import json
import math
import os
from pathlib import Path
import random
import statistics
import tempfile
import time

import test_engine


def process_cpu(pid):
    fields=Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()
    return (int(fields[11])+int(fields[12]))/os.sysconf('SC_CLK_TCK')


def io_counters(pid):
    return {key:int(value) for key,value in (line.split(':') for line in Path(f'/proc/{pid}/io').read_text().splitlines())}


def prepare(unquoted):
    rng=random.Random(600081)
    vectors=[[test_engine.f32(rng.uniform(-1,1)) for _ in range(128)] for _ in range(10000)]
    queries=[[test_engine.f32(rng.uniform(-1,1)) for _ in range(128)] for _ in range(1000)]
    def request(ident,sql):
        return json.dumps(dict(id=ident,op='execute',sql=sql,search_mode='exact')).encode()+b'\n'
    inserts=[]
    for base in range(0,10000,100):
        values=[]
        for i in range(base,base+100):
            vector=json.dumps(vectors[i],separators=(',',':'))
            if not unquoted:
                vector="'"+vector+"'"
            values.append(f"({i},'g{i%10}',{vector})")
        inserts.append(request(base+10,'INSERT INTO t VALUES '+','.join(values)+';'))
    selects=[]
    truth=[]
    for i,query in enumerate(queries):
        where='' if i%2==0 else f" WHERE g='g{i%10}'"
        selects.append(request(i+20000,f'SELECT id,L2_DISTANCE(v,{test_engine.literal(query)}) AS d FROM t{where} ORDER BY d LIMIT 10;'))
        pool=range(10000) if i%2==0 else range(i%10,10000,10)
        truth.append(heapq.nsmallest(10,((math.dist(vectors[k],query),k) for k in pool)))
    return inserts,selects,truth


def measure(binary,inserts,selects,truth):
    test_engine.BINARY=binary
    with tempfile.TemporaryDirectory(prefix='mvdb-ab-') as directory:
        db=test_engine.Session(directory,cache_mb=4)
        try:
            db.request('CREATE TABLE t(id BIGINT PRIMARY KEY,g VARCHAR(8),v VECTOR(128));')
            pid=db.proc.pid
            def timed(payloads):
                responses=[]
                before_io=io_counters(pid)
                cpu=process_cpu(pid)
                start=time.perf_counter()
                for payload in payloads:
                    db.proc.stdin.write(payload)
                    db.proc.stdin.flush()
                    responses.append(db.read())
                wall=time.perf_counter()-start
                cpu=process_cpu(pid)-cpu
                after_io=io_counters(pid)
                return responses,dict(wall_seconds=wall,process_cpu_seconds=cpu,
                    process_io_delta={key:after_io[key]-value for key,value in before_io.items()})
            responses,write=timed(inserts)
            assert all(x.get('ok') is True and x.get('affected_rows')==100 for x in responses)
            start=time.perf_counter()
            db.request('CREATE VECTOR INDEX idx ON t(v);')
            index=time.perf_counter()-start
            responses,query=timed(selects)
            for i,(response,expected) in enumerate(zip(responses,truth)):
                assert response.get('ok') is True and response.get('id')==i+20000,response
                actual=response['rows']
                assert [x[0] for x in actual]==[x[1] for x in expected],(i,actual,expected)
                assert all(math.isclose(got[1],want[0],rel_tol=1e-10,abs_tol=1e-10) for got,want in zip(actual,expected))
            peak=next(line for line in Path(f'/proc/{pid}/status').read_text().splitlines() if line.startswith('VmHWM:'))
            size=sum(p.stat().st_size for p in Path(directory).iterdir())
            db.restart(kill=True)
            assert len(db.request('SELECT id FROM t;')['rows'])==10000
            return dict(write=write,index_seconds=index,query=query,peak_rss=peak,database_bytes=size,passed=True)
        finally:
            db.stop()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline',type=Path,required=True)
    parser.add_argument('--candidate',type=Path,required=True)
    parser.add_argument('--runs',type=int,default=3)
    parser.add_argument('--unquoted',action='store_true')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.output.exists():
        parser.error('use a new evidence output path')
    inserts,selects,truth=prepare(args.unquoted)
    observations=[]
    for run in range(args.runs):
        for variant in (('baseline','candidate') if run%2==0 else ('candidate','baseline')):
            result=measure(getattr(args,variant).resolve(),inserts,selects,truth)
            result.update(run=run+1,variant=variant)
            observations.append(result)
            print(f'{variant} round {run+1}: write={result["write"]["wall_seconds"]:.6f}s query={result["query"]["wall_seconds"]:.6f}s',flush=True)
    summary={variant:{key:statistics.median(x[key]['wall_seconds'] for x in observations if x['variant']==variant)
                     for key in ('write','query')} for variant in ('baseline','candidate')}
    report=dict(scope='local_prepared_request_ab_only',host_model=next(line.split(':',1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name')),
        rows=10000,dimension=128,queries=1000,filtered_queries=500,seed=600081,
        insert_vector_syntax='unquoted' if args.unquoted else 'quoted',runs=args.runs,
        binary_sha256={key:hashlib.sha256(getattr(args,key).read_bytes()).hexdigest() for key in ('baseline','candidate')},
        observations=observations,median_wall_seconds=summary,oracle_checked_queries_per_run=1000,passed=True,
        note='Requests and independent truth prepared outside timing. Process CPU/RSS/io counters are not cgroup metrics or official E. Data directories use tempfile default filesystem; record TMPDIR externally.')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(summary),flush=True)


if __name__=='__main__':
    main()
