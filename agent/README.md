# Agent Prompt 学习材料

本题与性能评测共用同一次提测。默认评价入口读取本目录的单个 `conversation.json`，无需另交 ZIP 或对话目录。
要求依据：[Agent Prompt 题面](https://open.oceanbase.com/train/TopicDetails?questionId=600081&subQesitonId=800518)及[赛题说明第 3.4、4.4 节](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lg8enxlmwg69unt6)。

## 当前材料

- 保留原实现阶段的用户请求、Agent 决策和实际工具结果。
- 新增 2026-11-16 由 Agent 提问后，用户实际提供的三项技术回答，按原文记录来源和上下文。
- 按用户提出的顺序执行 COMMIT 确认后强杀、同目录恢复，共重复 3 轮，同时核对普通行与向量距离；另补充未提交修改的回滚检查。
- 使用四条记录实际演示过滤与 TopK 顺序的差异；错误执行顺序仅在测试脚本中模拟，没有声称数据库曾存在这一缺陷。
- 从 `/proc` 读取地址空间限制，区分 RLIMIT_AS、进程 RSS 和 Docker 容器总内存；尚未运行的容器测量明确标记为未执行。
- 在 JSON 内嵌入实际 SQL、响应、信号、重启和结果，不依靠评审自动打开仓库其他文件。

`evidence_units` 仅组织三条不同问题链的学生原文与实际结果，附可定位的 RFC6901 引用。不预填分数，不将同一证据拆分重复计分，也不把 Agent 的工作归为学生贡献。正式评审决定实例是否有效、适用维度及档位。

## 本地检查

Windows PowerShell 或 Linux 均可执行格式检查：

```bash
python agent/check_material.py
```

检查 UTF-8、顶层对象、128 KiB 大小、重复键、非有限数值、明显令牌/私钥模式、记录编号、引用定位、原文匹配、来源和源码指纹。源码指纹统一按 LF 换行计算。检查通过不等于材料获得任何正式分数，也不能自动证明对话真实性。

Linux 已构建程序后可重复运行本次技术验证，使用新的输出文件名：

```bash
make
python3 agent/run_learning_checks.py --output protocol-results/learning-new.json
```

该脚本只生成实际实验记录，不生成用户回答或评分。运行结果不会自动改写 `conversation.json`；需要将新实验实际结果连同对应的真实用户理解一并整理后再提交。

## 后续维护

真实新增的提问、判断、预测、复盘可以继续写入 JSON。保留必要上下文，说明用户发言是在实验前还是实验后。仅计划的实验标记为计划，实际运行才记录为执行；对方案的局限和未完成项也应保留。

材料须与源码、Dockerfile 位于同一个 commit。创建新 commit 后，性能与 Agent 评价都应使用新的完整 SHA，不能把不同提交的两项结果拼接。用户手动推送后，在平台先预检再正式提测。
