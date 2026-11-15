# MiniVectorDB

本实现对应 [OceanBase 训练营「性能评测」](https://open.oceanbase.com/train/TopicDetails?questionId=600081&subQesitonId=800517) 和官方 `2026-competition-round2` 题包。
采用 C++20，自行实现 SQL 执行、磁盘 B+ 树、向量块索引、事务和恢复。唯一第三方代码是已随源码交付的 nlohmann/json 3.11.3（JSON 解析及序列化，MIT 许可）。不依赖外部数据库、存储或向量引擎。

## 构建及运行

Linux x86_64，GCC 13.3+，GNU Make，测试需要 Python 3.9+，无 pip 运行依赖。

```bash
make
./build/minivectordb --data-dir /path/to/empty-data
```

程序主动输出 `{"type":"ready"}`，之后按行接收 JSON 请求。默认数据目录 `/data`；`--cache-mb N` 设置 B+ 树页缓存大小，默认 16 MiB。

```jsonl
{"id":1,"op":"execute","sql":"CREATE TABLE docs(id BIGINT PRIMARY KEY, tag VARCHAR(16), embedding VECTOR(2));"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1,'甲','[3,4]'),(2,'乙','[0,0]');"}
{"id":3,"op":"execute","sql":"CREATE VECTOR INDEX docs_vec ON docs(embedding);"}
{"id":4,"op":"execute","sql":"SELECT id,L2_DISTANCE(embedding,'[0,0]') AS distance FROM docs WHERE tag='甲' ORDER BY distance LIMIT 10;","search_mode":"exact"}
{"id":5,"op":"execute","sql":"BEGIN;"}
{"id":6,"op":"execute","sql":"UPDATE docs SET embedding='[0,1]' WHERE id=1;"}
{"id":7,"op":"execute","sql":"ROLLBACK;"}
{"id":8,"op":"execute","sql":"BEGIN;"}
{"id":9,"op":"execute","sql":"UPDATE docs SET tag='已提交' WHERE id=1;"}
{"id":10,"op":"execute","sql":"COMMIT;"}
{"id":11,"op":"shutdown"}
```

以相同 `--data-dir` 再次启动，执行 `SELECT * FROM docs ORDER BY id;` 可读到已提交记录。stdout 只输出 JSONL；诊断信息写 stderr。支持 EOF 关闭；进程被 SIGTERM 或 SIGKILL 终止时，未提交事务通过启动恢复回滚。

## Docker

Dockerfile 用 `gcc:13.3.0-bookworm` 编译静态二进制，最终镜像基于 `scratch`，运行阶段无需安装依赖、不联网、不监听端口。

```bash
docker build -t minivectordb:local .
docker run --rm -i --network none --read-only \
  --mount type=volume,src=minivectordb-data,dst=/data \
  minivectordb:local
```

正式评测前，应在安装了 Docker 的机器运行实际镜像检查：

```bash
python3 protocol_check/run.py --image minivectordb:local --handshake-only --output protocol-results/docker-handshake
python3 protocol_check/run.py --image minivectordb:local --output protocol-results/docker-public
```

以上报告目录须不存在。需要重复运行时更换目录名。

## SQL 范围

- `CREATE TABLE`：BIGINT、VARCHAR(n)、VECTOR(n)，一个 BIGINT 主键，至多一个向量列；支持列内和表级主键声明。
- `INSERT INTO ... VALUES ...`：多行、显式列顺序；所有列必须提供值。
- `SELECT *`、列投影、`L2_DISTANCE(vector,'[...]') AS alias`；WHERE 比较 `= < <= > >=`、AND；ORDER BY 普通列、距离表达式或投影别名、ASC/DESC、多列排序；LIMIT。
- `UPDATE ... SET ... WHERE ...`、`DELETE FROM ... WHERE ...`。
- `CREATE VECTOR INDEX name ON table(vector_column)`。
- `BEGIN`、`COMMIT`、`ROLLBACK`；同一时刻一个显式事务；DDL 也随事务提交或回滚。

VARCHAR 按 Unicode 字符数量校验，按 Unicode 码点顺序比较。BIGINT 保留完整有符号 64 位精度。向量写入和查询向量均转换为有限 float32，距离使用转换后的值，以 double 累加并返回欧氏距离。距离或普通排序键并列时，最终按主键升序。

不支持的 SQL 返回非空语义错误码，例如 `unsupported_sql`。类型、长度、维度、重复主键等错误会保留语句执行前的状态；显式事务中先前成功的语句仍可继续提交。未支持 NULL、JOIN、聚合、子查询、ALTER、主键修改和嵌套事务，不将其静默解释为其他操作。

## 存储和事务设计

`/data/store.bin` 为追加分配的二进制文件：BIGINT 为 int64，字符串为 UTF-8 长度加字节，向量为连续 float32。固定 4 KiB B+ 树页将主键映射至记录位置。页缓存有界，扫描只保留当前树路径；数据和向量无需全量常驻内存。

每条修改语句记录分配边界。触及较早页面时复制该页和必要的祖先；本语句新页可原位修改。发生 SQL 错误时恢复语句前的表根并截断新增尾部，已经驱逐到磁盘的未提交页同样被丢弃。显式事务的成功语句共享待提交表目录，查询可见自身修改；ROLLBACK 恢复已提交目录。

COMMIT 先刷新所有脏页和新目录并 `fsync(store.bin)`，再写入 `/data/MANIFEST` 的交替提交槽并 fsync，完成后才返回成功。槽包含代数、目录位置、提交尾部和校验值；目录也带校验值。启动选择最新有效提交槽，读取目录并截断未提交尾部。恢复无需全表重放，不会修改已提交页，允许恢复过程再次被终止。

## 向量索引和资源边界

向量索引是自行实现的覆盖块索引：每块最多 128 行或约 1 MiB，存储完整记录；上层是最多 32 路的度量球层次。三角不等式提供保守距离下界，可对升序距离 TopK 做精确剪枝。遍历索引块时先应用过滤条件，再更新大小为 K 的堆。浮点下界使用向外裕量，避免边界误剪。

索引建立后的插入、更新和删除写入另一棵 COW 增量 B+ 树。查询跳过被增量覆盖的旧条目，并合并增量当前记录；增量根和表根一起事务提交或回滚。索引无需在启动时全量重建。`ann` 模式也返回精确结果，因此无需用召回率换取正确性。

无索引时执行有界页缓存扫描；有 LIMIT 的排序用 O(K) 堆；无 LIMIT 的排序使用 8 MiB 排序缓冲和最多 32 路归并。查询结果逐行序列化至同一条 JSONL 响应，避免把完整结果集复制到内存。临时归并文件位于 `/data`，打开后立即 unlink，进程退出即自动回收。

当前没有后台压缩：历史版本、被替换的索引及删除产生的旧记录会占用磁盘；长期高频小事务修改的空间放大仍需改进。查询 K 和单条请求的解析内存随输入大小增长。向量球对高维随机数据的剪枝效果有限；本实现没有 HNSW，也未声称正式性能排名。

## 验证

```bash
make public
make check
python3 tests/test_engine.py --large-memory
```

详细执行证据、环境及未验证项见 [VALIDATION.md](VALIDATION.md)。公开样例通过不代表隐藏用例或正式评分已通过。Agent 材料位于 `agent/conversation.json`，仅记录真实用户输入、Agent 操作和实际工具结果，不虚构用户学习或实验。

## 手动推送和提测

Windows PowerShell，进入本目录。先在 GitHub 创建自己的 `oceanbase/seekdb` Fork；取消“仅复制默认分支”，使比赛分支也保留在 Fork 中。然后将下面地址替换成自己的仓库：

```powershell
Set-Location F:\vs_space\minivectordb
git remote rename origin upstream
git remote add origin https://github.com/YOUR_NAME/seekdb.git
git push -u origin 2026-competition-round2
git rev-parse HEAD
```

如果 origin 已指向自己的仓库，跳过 rename/add。首次推送时按 Git 的登录提示完成 GitHub 认证，不要把令牌写进远程 URL。若自己的 Fork 上已经有此分支且 push 被拒绝，先 fetch 并检查差异，不要直接 force push。

平台填写自己的仓库 HTTPS URL、分支 `2026-competition-round2` 和 `git rev-parse HEAD` 返回的完整 40 位 SHA。先运行平台预检；确认镜像构建、握手及公开测试通过后，再进入正式评测。正式机会每日有限，本次交付未触发平台提测。
