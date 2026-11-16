#!/usr/bin/env python3
"""Run the real checks discussed with the user; never generate student answers."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from test_engine import BINARY, Session


def topk_counterexample():
    events = []
    with tempfile.TemporaryDirectory(prefix='learning-topk-') as directory:
        db = Session(directory)
        try:
            def query(sql):
                response = db.request(sql)
                events.append({'sql': sql, 'response': response})
                return response
            query('CREATE TABLE t(id BIGINT PRIMARY KEY,g VARCHAR(8),v VECTOR(2));')
            query("INSERT INTO t VALUES (1,'skip','[0,0]'),(2,'skip','[0.5,0]'),(3,'keep','[1,0]'),(4,'keep','[2,0]');")
            query('CREATE VECTOR INDEX idx ON t(v);')
            all_top2 = query("SELECT id,g,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 2;")['rows']
            # Simulate the incorrect order outside the database, not an engine defect.
            wrong = [row[0] for row in all_top2 if row[1] == 'keep']
            filtered = query("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t WHERE g='keep' ORDER BY d LIMIT 2;")['rows']
            assert wrong == []
            assert filtered == [[3, 1.0], [4, 2.0]]
            return {
                'id': 'filtered_topk', 'status': 'passed', 'events': events,
                'incorrect_order_simulation_ids': wrong,
                'correct_filtered_ids': [row[0] for row in filtered],
                'summary': '全表 Top2 的 ID 是 [1,2]，后置过滤得到 []；先过滤的真实数据库查询返回 [3,4]，距离为 [1,2]。',
            }
        finally:
            db.stop()


def commit_crash():
    events = []
    rounds = []
    with tempfile.TemporaryDirectory(prefix='learning-crash-') as directory:
        db = Session(directory)
        try:
            def query(sql):
                response = db.request(sql)
                events.append({'sql': sql, 'response': response})
                return response
            def kill_and_recover(reason):
                db.stop(kill=True)
                return_code = db.proc.returncode
                assert return_code == -signal.SIGKILL, return_code
                events.append({'action': 'SIGKILL', 'exit_code': return_code, 'reason': reason})
                db.start()
                events.append({'action': 'restart', 'handshake': {'type': 'ready'},
                               'data_directory': 'same temporary data directory'})
            query('CREATE TABLE t(id BIGINT PRIMARY KEY,tag VARCHAR(16),v VECTOR(2));')
            query('CREATE VECTOR INDEX idx ON t(v);')
            for key in range(1, 4):
                query('BEGIN;')
                query(f"INSERT INTO t VALUES ({key},'committed','[{3*key},{4*key}]');")
                # Session.request reads and validates the response before returning.
                ack = query('COMMIT;')
                assert ack['ok'] is True
                kill_and_recover('COMMIT success response has already been received')
                rows = query('SELECT id,tag FROM t ORDER BY id;')['rows']
                distances = query("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 10;")['rows']
                assert rows == [[i, 'committed'] for i in range(1, key+1)]
                assert distances == [[i, float(5*i)] for i in range(1, key+1)]
                rounds.append({'round': key, 'commit_response': ack, 'rows_after_restart': rows,
                               'vector_results_after_restart': distances})
            query('BEGIN;')
            query("INSERT INTO t VALUES (99,'uncommitted','[0,0]');")
            query("UPDATE t SET v='[99,99]' WHERE id=1;")
            query('DELETE FROM t WHERE id=2;')
            # Confirm that these pending changes really were visible before the kill.
            before_kill = query('SELECT id FROM t ORDER BY id;')['rows']
            assert before_kill == [[1], [3], [99]]
            kill_and_recover('no COMMIT request for the pending transaction')
            rows = query('SELECT id FROM t ORDER BY id;')['rows']
            distances = query("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 10;")['rows']
            assert rows == [[1], [2], [3]]
            assert distances == [[1, 5.0], [2, 10.0], [3, 15.0]]
            return {
                'id': 'acknowledged_commit_sigkill', 'status': 'passed',
                'rounds': rounds, 'uncommitted_rows_visible_before_kill': before_kill,
                'final_rows': rows, 'final_vector_results': distances, 'events': events,
                'summary': '3 轮均先收到 COMMIT 成功响应，再 SIGKILL；同目录重启后已提交行及向量距离完整。另一个未提交事务的插入、更新和删除在强杀重启后全部回滚。',
            }
        finally:
            db.stop()


def memory_scope():
    with tempfile.TemporaryDirectory(prefix='learning-memory-') as directory:
        db = Session(directory, memory_mb=96, cache_mb=4)
        try:
            pid = db.proc.pid
            limits = Path(f'/proc/{pid}/limits').read_text()
            status = Path(f'/proc/{pid}/status').read_text()
            address_limit = next(line for line in limits.splitlines() if line.startswith('Max address space'))
            address_fields = address_limit.split()
            assert address_fields[-3:-1] == ['100663296', '100663296'], address_limit
            metrics = [line for line in status.splitlines()
                       if line.startswith(('VmSize:', 'VmRSS:', 'VmHWM:'))]
            return {
                'id': 'memory_measurement_scope', 'status': 'process_scope_verified',
                'limits_line': address_limit, 'process_metrics': metrics,
                'docker_cli_available': shutil.which('docker') is not None,
                'local_cgroup_v2_available': Path('/sys/fs/cgroup/cgroup.controllers').exists(),
                'docker_container_measurement': 'not_executed',
                'summary': '读取 /proc 确认 96 MiB 限制作用于 Max address space；VmSize、VmRSS、VmHWM 是不同指标。此记录没有测量 Docker 容器 memory.peak，不能据此声称容器内存小于 96 MiB。',
            }
        finally:
            db.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('use a new output path to preserve previous evidence')
    report = {
        'session_date': '2026-11-16', 'timezone': 'Asia/Shanghai',
        'source': 'actual_local_tool_execution', 'synthetic_student_dialogue': False,
        'implementation_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'binary_sha256': hashlib.sha256(BINARY.read_bytes()).hexdigest(),
        'checks': [topk_counterexample(), commit_crash(), memory_scope()],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    for check in report['checks']:
        print(check['id']+': '+check['status']+' — '+check['summary'])
    print('Report: '+str(args.output))


if __name__ == '__main__':
    main()
