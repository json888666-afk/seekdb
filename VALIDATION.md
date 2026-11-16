# 本地验证记录

日期：2026-11-15（Asia/Shanghai）。基于官方题包提交 `f4d4e26653af823a7601668802f80e374ad6643c`。
运行环境为 Windows 主机上的 WSL Ubuntu 24.04、Linux x86_64，GCC 13.3.0、Python 3.12.3。

## 已执行

| 检查 | 结果 | 可复查材料 |
| --- | --- | --- |
| `-Wall -Wextra -Wpedantic` 优化编译 | 通过，无警告 | `make`；Dockerfile 相同编译选项 |
| Linux 静态二进制 | 编译通过，ELF x86_64 statically linked | `build/minivectordb-static`，可从源码重建 |
| 官方交互驱动、静态程序、全部公开样例 | 20/20，通过 95 条 SQL 及重启/模式切换 | `validation/public.json` |
| 额外黑盒回归 | 7/7 | `validation/regression.txt` |
| AddressSanitizer + UndefinedBehaviorSanitizer 回归 | 7/7 | `validation/sanitizers.txt` |
| 超地址空间预算数据处理 | 通过 | `validation/memory.json` |
| 赛题规模本地性能诊断 | 通过，1000 次查询均核对独立真值 | `validation/benchmark.json` |

额外回归覆盖：BIGINT 两端及溢出、Unicode 长度/排序、大小写列名和别名、字符串中的分号和转义、不支持 SQL 的拒绝、NaN/Infinity/float32 溢出/维度错误、失败多行写入原子性、跨页缓存驱逐的回滚、随机向量 TopK 真值比较、索引增删改、事务内读自身修改、COMMIT/ROLLBACK/强杀交错、事务 DDL、恢复中再次终止、7 万行外部排序及排序并列规则。

大数据测试使用 96 MiB **进程地址空间**限制、4 MiB 页缓存，写入 150,000 条 256 维向量，建立索引，强杀并重启后检索。数据文件总计 333,334,161 字节（约 318 MiB），超过进程限制。该测试证明没有依赖全表常驻内存；它不是对 cgroup memory.peak（包括文件页缓存）的测量。

本地基准使用 10,000 条 128 维随机 float32 向量、每批 100 条持久化写入、1,000 次 TopK=10 查询，其中 500 次有等值过滤。1000 次结果全部与 Python `math.dist` 独立计算的 ID、距离核对。一次实际运行记录：写入 1.888781 秒，建索引 0.016591 秒，查询 1.881566 秒；进程峰值 RSS 6,568 KiB，数据文件 12,093,396 字节。时间包含 Python 请求构造和进程通信，受主机负载影响。

## 未执行与限制

- 当前环境没有 Docker CLI，因此没有声称实际镜像构建或容器检查已通过。静态程序已按 Dockerfile 相同的 GCC 主版本及编译选项构建并通过官方交互驱动；请按 README 对实际镜像补跑检查。
- 未访问正式隐藏输入、未触发平台预检或正式提测、未获得正式性能分或 Agent 分。
- 本地时间、进程 RSS、文件大小不能代替正式 cgroup T/B/M 或比赛排名。
- 存储采用追加分配，没有历史版本压缩；频繁小事务可能造成磁盘空间放大。
- 截至首版，真实用户只提出任务和手动推送要求；2026-11-16 的新增用户技术回答和验证另见下节。两阶段来源在材料中明确区分。

## 复现

```bash
make
make public
make check
python3 tests/test_engine.py --large-memory
python3 tests/benchmark.py

g++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -o build/minivectordb-asan src/main.cpp
MINIVECTORDB_BINARY="$PWD/build/minivectordb-asan" python3 tests/test_engine.py
```

所有测试数据目录由测试程序独立创建并回收；每次官方驱动报告目录必须使用新路径。源码和依赖已交付，无需联网下载编译依赖。

## 2026-11-16：Agent Prompt 材料补充

用户在实验前实际解释了过滤与 TopK 的关系，提出“确认 COMMIT 成功后强杀、同目录重启并重复检查”的测试方案，并指出进程内存与容器统计的边界。原文及提问上下文保存在 `agent/conversation.json`。

按该过程运行 `agent/run_learning_checks.py`，实际结果保存于 `validation/learning-checks-20261116.json`，同时内嵌进材料 JSON：

- 过滤反例：全表 Top2 为 ID `[1,2]`，两条均不满足过滤条件，后置过滤为空；真实过滤查询返回 `[3,4]`，距离 `[1,2]`。
- 3 轮均先收到 COMMIT 成功响应，再 SIGKILL；同目录重启后普通行、向量距离完整保留。额外未提交事务在强杀前可见其插入、更新、删除，重启后这些修改全部回滚。
- `/proc/<pid>/limits` 明确显示 `Max address space` 为 100,663,296 字节，等于 96 MiB；同时记录 VmSize、VmRSS、VmHWM。该检查没有测量 Docker 容器 memory.peak；当前环境未发现 Docker CLI 和本地 cgroup v2 入口。

新增 `agent/check_material.py` 检查材料格式、大小、严格 JSON、证据引用和来源。它不计算正式分数，不判断真人学习效果；检查结果在 `validation/material-check.json`。数据库实现和 Dockerfile 与 `b7be60a484387e27ea79dd29e8ab8f09d3877996` 保持一致，源码指纹随材料交付。

用户随后回复“你自己回答”，将实验后的复盘交给 Agent。材料已记录 Agent 对上述结果的解释和局限，并将下一轮容器级资源预算验证列为未执行计划；这些内容没有记为新的学生技术贡献或新的实测结果。

## 2026-11-16（接力第二轮）：缺陷修复、检索加速与容器约束验证

### 源码改动

| 文件 | 改动 |
| --- | --- |
| `src/storage.hpp` | 新增有序块缓存 `blob_cache`（上限 24 MiB，`rollback()` 时清空）；`read_blob_cached()` 返回共享块，避免重复 `pread` |
| `src/storage.hpp` | MANIFEST 无有效槽且数据目录非空时不再直接抛错，截断到首页并重建空目录，消除首次 COMMIT 前被强杀后无法启动的窗口 |
| `src/engine.hpp` | WHERE 支持括号分组的 AND；OR 仍明确报 `unsupported_sql` |
| `src/sql.hpp` | 新增 `l2(const char*, const std::vector<float>&, size_t)`，按行体字节直接算距离，归约顺序与原重载一致 |
| `src/vector_index.hpp` | 叶节点先比较第 K 名距离，超过阈值直接跳过整行解码 |

### 同机 A/B

同一台机器、同一 seed、同一工作负载，只更换 `src/`：

| 版本 | 写入 | 建索引 | 1000 次查询 | 进程 VmHWM |
| --- | --- | --- | --- | --- |
| `ad5e5a3a` 原始源码 | 1.223023 s | 0.018644 s | 2.298565 s | 5120 kB |
| 本轮源码 | 1.227573 s | 0.018704 s | **1.290202 s** | 10240 kB |

查询耗时降到原来的 0.561 倍，写入与建索引基本不变；代价是新增约 5 MiB 常驻块缓存（上限 24 MiB）。
两种版本都通过了 `benchmark.py` 对 1000 次查询的独立真值核对。

### 回归结果

| 检查 | 结果 |
| --- | --- |
| `protocol_check/run.py` 公开用例 | 20/20 PASS（`passed=true`） |
| `tests/test_engine.py` | 7/7 OK |
| `tests/test_engine.py --large-memory`（150,000×256，96 MiB 地址空间） | passed，库 333,334,161 字节，8.05 s |
| `tests/benchmark.py`（10k×128，1000 次 TopK=10，1000 次真值核对） | passed |

### 容器约束等价验证

验证机是嵌套容器：`unshare` 对 root 与非 root 均返回 `Operation not permitted`（缺少 `CAP_SYS_ADMIN`），
Docker 与 cgroup 层级都不可用于创建容器，因此**没有构建或启动真实镜像**，也没有读取 cgroup `memory.peak`。
改为用原生工具逐项等价判题的 `docker run` 约束：

| `docker run` 参数 | 本机等价做法 |
| --- | --- |
| `--cpus=2` | `taskset -c 0,1` |
| `--memory=2g --memory-swap=2g` | `prlimit --as=2147483648 --rss=2147483648` |
| `--pids-limit=128` | `prlimit --nproc=128` |
| `--cap-drop=ALL` | `setpriv` 切到非 root，无 capability |
| `--security-opt=no-new-privileges` | `setpriv --no-new-privs` |
| `--read-only` | 非 root 运行，仅数据目录可写，其它路径写入必然 EACCES |
| `--network=none` | 程序只使用 stdin/stdout/stderr 与数据目录，不创建 socket |

Dockerfile 的命令行在验证机上原样执行通过：无警告，产物 2,714,352 字节，静态链接，
`ldd` 输出 `not a dynamic executable`。这只证明编译步骤可重现，不等于镜像已验证。

| 约束下的检查 | 结果 |
| --- | --- |
| 公开协议 | 20/20 PASS |
| 冷启动、CRUD、L2 TopK | PASS，VmHWM 1024 kB，shutdown 退出码 0 |
| 事务中途 SIGKILL → 重启 | PASS，已提交 3 行保留，未提交插入回滚 |
| 只读根文件系统模拟 | PASS，数据目录外无写入权限错误 |
| 150,000×256 数据集 | PASS，VmHWM 19,812 kB，库 163.4 MiB |
| benchmark | PASS，写入 1.196487 s，查询 1.333650 s，VmHWM 8192 kB |

### 仍未验证

真实 `docker build` / `docker run`、cgroup v2 `memory.peak`、平台隐藏用例、正式提测、正式性能分与 Agent 分。
上面的内存数字都是进程 `VmHWM`，不能当作容器内存。
