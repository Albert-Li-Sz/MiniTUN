# 最终验收

MiniTun 0.1.0 已于 2026-08-02 完成阶段 16 最终验收。获验收的实现基线为提交
[`14593f0d004e6d59513830570189e1c45b46e344`](https://github.com/LMTINSUZHOU/MiniTUN/commit/14593f0d004e6d59513830570189e1c45b46e344)。
验收收尾提交只更新文档，不改变已经测试的程序或软件包。

## 自动化证据

| 要求 | 证据 | 结果 |
| --- | --- | --- |
| 完整测试套件 | [CI 运行 30691311281](https://github.com/LMTINSUZHOU/MiniTUN/actions/runs/30691311281)：Ubuntu 22.04 GCC Debug、Ubuntu 24.04 GCC Release 和 Ubuntu 24.04 Clang Debug 均通过 206/206 个 CTest 用例及 CLI 冒烟测试 | 通过 |
| ASan 与 UBSan | [Sanitizers 运行 30691311287](https://github.com/LMTINSUZHOU/MiniTUN/actions/runs/30691311287)：Clang ASan+UBSan 在启用泄漏检测和遇错即停的情况下通过 206/206 | 通过 |
| TSan | [Sanitizers 运行 30691311287](https://github.com/LMTINSUZHOU/MiniTUN/actions/runs/30691311287)：原生 Clang TSan 在启用遇错即停的情况下通过 206/206 | 通过 |
| Fuzz 冒烟测试 | [Sanitizers 运行 30691311287](https://github.com/LMTINSUZHOU/MiniTUN/actions/runs/30691311287)：五个 libFuzzer 目标均完成 2,000 次运行 | 通过 |
| DEB | [Packages 运行 30691311270](https://github.com/LMTINSUZHOU/MiniTUN/actions/runs/30691311270)：206/206 测试、元数据/内容校验及干净 Ubuntu 安装冒烟测试 | 通过 |
| RPM | [Packages 运行 30691311270](https://github.com/LMTINSUZHOU/MiniTUN/actions/runs/30691311270)：206/206 测试、元数据/内容校验及干净 Fedora 安装冒烟测试 | 通过 |
| 多服务器 E2E | `integration.multi-server-sessions` 在每个编译器、Sanitizer 和软件包测试任务中均通过；覆盖并发服务器、故障隔离、服务端重启、守护进程重启和稳定客户端身份 | 通过 |

同一组测试还覆盖隧道注册、Worker Pool 分配、原始 TCP 中继、半关闭行为、背压、
连接配额、优雅关闭、安装布局、重连和恢复。Linux 重点验收复测的六项高风险集成
测试也全部通过：多服务器会话、隧道注册、Worker Pool、TCP 中继、稳定性和安装布局。

## 软件包产物校验

验收使用从 CI 下载的正式产物，而不是本地构建结果。清单校验成功，软件包元数据
报告版本 0.1.0 和预期的 x86_64 架构；干净的 Ubuntu 与 Fedora 容器均通过安装、
重复安装/升级、卸载，以及文档约定的状态保留或 purge 策略测试。

```text
84d1d6bcb4bca47a422120773d7435674fef07ca302c79c32ac4da3c8f7d7fa8  minitun-client-0.1.0-linux-amd64.deb
eccf04546fd846b933b5308eb6951214ac983fce1eea7d4b26cc2836323460b4  minitun-server-0.1.0-linux-amd64.deb
eb4f8fddd94c99e96d2576f5e04e99cc53e04cf612c66afb7de4011154afec10  minitun-client-0.1.0-linux-x86_64.rpm
f71c6d808d8174aadcf21c64a71fc8eaf3eb2262aa0b8e83ae71af631414af9f  minitun-server-0.1.0-linux-x86_64.rpm
```

## 文档与发布就绪状态

三个 CLI 帮助界面、安装路径、systemd unit、sysusers 定义、man 手册、安全边界、
软件包生命周期、CI 工作流和发布说明均已与实现交叉核对。GitHub Actions 工作流语法
及其中的 Shell 已通过 actionlint 1.7.12 和 ShellCheck 0.11.0。仓库 Shell 脚本、
本地 Markdown 链接、发布 tag 校验、空白检查和已安装 DEB 的 systemd unit 也通过
各自的专项检查。

发布工作流只接受 `vMAJOR.MINOR.PATCH` 和 `vMAJOR.MINOR.PATCH-rc.NUMBER`，
要求基础版本与 `CMakeLists.txt` 匹配，复用两个已测试的软件包任务，校验四个预期
产物及合并后的 SHA-256 清单，并且只在这些任务成功后发布。

最终验收不会创建公开 tag 或 GitHub Release。选定发布点后，操作者可以用以下命令
发布已经验证的 0.1.0：

```bash
git tag -a v0.1.0 -m "MiniTun v0.1.0"
git push origin v0.1.0
```

## 结论

**通过。** 阶段 16 的全部要求及项目完成标准均已满足。MiniTun 0.1.0 已准备好
创建经操作者批准的版本 tag 并发布。
