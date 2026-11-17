# Round 5：零拷贝 token 与解析成功路径优化

日期：2026-11-17（Asia/Shanghai）。本轮严格基于 `8736a8034c9c9fe33021f2806993383cda369ba3`，新提交追加在它之后；提交完成后的完整 SHA 请用 `git rev-parse HEAD` 获取。

## 1. 版本、平台基线与工作目录

- 仓库：`https://github.com/json888666-afk/seekdb.git`，最终远端分支仍为 `2026-competition-round2`。
- 用户报告的 Round 4 平台结果：MySQL 正式用例通过；性能原始指标 `E = 951.645912`；Agent Prompt 原始分 `3.000000`。这是用户提供的结果，本轮未独立读取平台回执；两项均不是已获得的 40 分制排名分。
- **本轮尚未正式提测，E 和 Agent 原始分未知。不能把本地计时直接换算成 E，也不能沿用旧提交的成绩作为本提交成绩。**
- 本地工作目录：`F:\vs_space\minivectordb-round5`，WSL 路径 `/mnt/f/vs_space/minivectordb-round5`，本地分支 `round5/perf`。
- 原目录 `F:\vs_space\minivectordb` 的工作区和本地旧分支保留。它包含旧交接文档提交，与远端 Round 4 的历史有分叉，因此本轮在新 worktree 中追加，不覆盖原有工作。
- 本机直连 GitHub 的 Git 拉取失败。本轮通过 GitHub 连接器读取 Git 数据，按 Git 对象哈希验证 blob/tree/commit 后导入，实际父提交就是原始 `8736a803...`，没有把源码快照伪造成另一个基线提交。

用户仍自行 push。本地新分支推送到比赛分支的命令是：

```powershell
cd F:\vs_space\minivectordb-round5
git push origin HEAD:2026-competition-round2
git rev-parse HEAD
```

如果远端又被其他人更新，普通 push 应拒绝非快进；先检查新提交并整合，不要 force push。

## 2. 本轮改动

### `src/sql.hpp`

- `Token::text` 改为 `std::string_view`。数字、标识符、标点和不含转义的引号内容直接引用本条 SQL；不再为每个 token 构造拥有字符串。
- 只有 SQL 引号重复或反斜杠转义需要解码时，才将解码内容放入 `std::deque<std::string>`；deque 中的元素位置稳定，token vector 扩容不会使短字符串 SSO 或长字符串内容失效。
- `Parser` 禁止复制；SQL 字符串必须比 Parser 活得久。现有 `Engine::execute` 的 SQL 参数满足此条件。解析器输出的表名、列名、VARCHAR 值等仍转成拥有字符串，不能让这些 view 逃逸到持久化目录或行中。
- 关键字比较逐字节做 ASCII 大小写比较，避免每次 `is/take` 构造两份小写字符串。
- BIGINT 改为在 view 上使用 `from_chars`，显式处理正负号与 `INT64_MIN`，保留溢出/类型错误行为。
- `require` 的 code/message 改为 `string_view`，只有条件失败才构造拥有错误字符串；`expect` 也只在失败时拼接消息。这消除了向量分量校验、查询过滤等成功路径中的重复分配。
- 引号向量直接将 view 传给现有 `vector_value`；Round 4 的浮点解析和 SIMD 距离算法不变。

### 其他

- `src/engine.hpp` 只将比较运算符的字符串构造改为显式从 view 构造，执行逻辑不变。
- 没有修改 `storage.hpp`、`vector_index.hpp`、Dockerfile、行格式、索引格式、fsync 顺序、SIMD 派发或剪枝裕量；没有增加 `-march=native`。
- 新增 `tests/test_parser_views.py`，并纳入 `make check`。
- 新增 `tests/benchmark_ab.py`：同机交替 A/B，提前构造请求与独立真值，分别观察请求墙钟耗时、进程 CPU、进程 I/O 计数与 VmHWM。它是本地诊断，不是平台计分器。

## 3. 本机 A/B 结果

本机为 WSL Ubuntu 24.04、AMD Ryzen 5 9600X、GCC 13.3、Python 3.12，与 Round 4 交接中的 i7 主机不同。表内比较只在本机的 Round 4 基线和本轮候选之间进行，不跨机器比较绝对耗时。

负载均为 seed 600081、10k×128、1000 次 TopK=10、500 次过滤；每版每组 3 轮，交替顺序。全部查询对齐独立 Python 真值，距离容差 `1e-10`。

| 口径与存储 | 基线写入中位数 | 候选写入中位数 | 基线查询中位数 | 候选查询中位数 |
| --- | ---: | ---: | ---: | ---: |
| 原 `tests/benchmark.py`，ext4，包含 Python 请求构造 | 2.520916 s | 2.404170 s | 0.371543 s | 0.344071 s |
| 预构造请求，引号向量，ext4 | 1.998017 s | 1.852907 s | 0.311238 s | 0.280644 s |
| 预构造请求，无引号向量，ext4 | 0.848633 s | 1.913265 s | 0.303798 s | 0.275277 s |
| 预构造请求，引号向量，tmpfs | 0.253732 s | 0.163649 s | 0.306013 s | 0.289253 s |
| 预构造请求，无引号向量，tmpfs | 0.384181 s | 0.183099 s | 0.294021 s | 0.274844 s |

必须保留的限制：ext4 写入波动很大，无引号组的候选墙钟中位数更慢。该现象尚未归因，不声称所有磁盘场景都稳定加速。与此同时，该组写入进程 CPU 中位数由 0.41 s 降到 0.20 s；引号 ext4 组由 0.27 s 降到 0.18 s。tmpfs 排除了大部分持久化等待，呈现更稳定的解析 CPU 收益。所有组的磁盘文件大小相同，写入系统调用数量没有因本轮而减少。

预构造请求各组中，候选进程 VmHWM 约 10,584–10,768 KiB，基线约 10,928–11,148 KiB；这些数值不是整个容器内存。详细每轮结果见 `validation/round5-ab-*.json` 与 `validation/round5-standard-ab.json`，汇总见 `validation/round5-ab-summary.json`。

## 4. 正确性与资源验证

| 检查 | 本轮结果 | 记录 |
| --- | --- | --- |
| 与 Dockerfile 相同主要编译选项的静态 Linux 构建 | 通过，无编译警告 | `build/compile-round5.txt`（本地构建产物） |
| 原有回归 | 7/7 | `validation/round5-regression.txt` |
| 新增 view 生命周期、转义、整数边界、失败语句回滚 | 4/4 | `validation/round5-parser-regression.txt` |
| 官方公开协议用例，静态程序 | 20/20 | `validation/round5-public.json` |
| ASan + UBSan | 原有 7 项及新增 4 项均通过 | `validation/round5-sanitizers.txt` |
| 150k×256、96 MiB 地址空间、4 MiB 页缓存 | 通过，文件 333,334,161 B | `validation/round5-memory.json` |
| 同机 A/B | 原版 benchmark 共 6 次、预构造请求共 24 次，每次 1000 条真值核对 | 对应 A/B JSON |
| 学习材料 | 源码指纹已更新，引用检查通过 | `validation/material-check.json` |

新增解析测试明确覆盖：1000 条同时包含短/长解码字符串的 INSERT、token 扩容、Unicode、NUL、反斜杠与重复引号、后续请求和重启后的字符串内容、带注释的整数符号、BIGINT 两端、重复键失败后的事务原子性。

尚未执行：真实 Docker 构建与运行、cgroup `memory.peak`、本轮平台正式用例和新 E。96 MiB 测试仍只限制进程地址空间。

## 5. 复现与材料维护

```bash
make
make check               # 7 项原有 + 4 项新增
make public              # 新建报告目录
python3 tests/test_engine.py --large-memory
make material
```

同机 A/B 需要两个分别从基线和候选源码构建的二进制，编译选项保持一致：

```bash
python3 tests/benchmark_ab.py --baseline /path/to/baseline --candidate /path/to/candidate \
  --output protocol-results/new-ab-ext4.json
TMPDIR=/dev/shm python3 tests/benchmark_ab.py --baseline /path/to/baseline --candidate /path/to/candidate \
  --unquoted --output protocol-results/new-ab-tmpfs-unquoted.json
```

`agent/conversation.json` 的历史结果保留原版本关联。新增本轮结果标为 Agent 执行，Round 4 平台成绩明确标为用户报告；不增加虚构的学生理解或新证据实例。`code_state.reference_commit` 记录本轮父提交 `8736a803...`，文件指纹对应本轮源码，不是说父提交源码与当前完全相同。评测必须使用新 HEAD 完整 SHA。

## 6. 下一步

1. 用户用新 SHA 在平台以相同口径提测，确认 E 是否超过 `951.645912`。本地结果无法替代这一结论。
2. 如果继续优化写入，优先量化逐行 `pwrite` 与 fsync 的占比，再考虑有界的语句级批量 append；本轮没有改这个路径。
3. 若尝试 fdatasync、批量缓冲或其他存储改动，保留“确认提交后强杀”和“未提交事务整体回滚”语义，先做故障窗口验证。
4. 继续保留运行时 SIMD 派发和标量兜底，避免为本机 CPU 生成不可移植镜像。
