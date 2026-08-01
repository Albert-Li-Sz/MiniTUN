# CI 与发布自动化

MiniTun 使用相互独立的 GitHub Actions 工作流处理构建、安全、打包和发布。每个
工作流都采用最小 Token 权限，并取消同一 ref 上已过时的运行。

| 工作流 | 触发条件 | 结果 |
| --- | --- | --- |
| `ci.yml` | 推送到 `main`/`develop`、Pull Request、手动触发 | GCC Debug、GCC Release 和 Clang Debug 构建，并运行全部 CTest 与 CLI 冒烟测试 |
| `sanitizers.yml` | 推送到 `main`、Pull Request、手动触发 | Clang ASan+UBSan、独立 TSan 和有界 libFuzzer 冒烟任务 |
| `package.yml` | 与打包有关的 `main` 推送、手动触发、可复用工作流调用 | 已测试的 amd64 DEB、x86_64 RPM 产物及 SHA-256 清单 |
| `release.yml` | 已校验的 `v*.*.*` tag | 已测试的软件包、合并后的 `SHA256SUMS` 和 GitHub Release |

Actions 本身固定到稳定的主版本。Dependabot 每周检查 `github-actions` 生态，并将
更新分组。

编译器矩阵的所有任务都使用发行版软件包，包括 Ubuntu 22.04 支持的 Asio 1.18
基线版本。

## 软件包产物

DEB 任务在 Ubuntu 上构建，并将结果安装到干净的 Ubuntu 22.04 amd64 容器中。
RPM 任务在 Fedora 内构建，再将结果安装到另一个干净的 Fedora x86_64 容器中。
两个任务都在 CPack 前运行完整测试套件，并检查软件包元数据、文件列表、安装、重复
安装和卸载。

上传的软件包文件使用面向发布的名称：

```text
minitun-client-0.1.0-linux-amd64.deb
minitun-server-0.1.0-linux-amd64.deb
minitun-client-0.1.0-linux-x86_64.rpm
minitun-server-0.1.0-linux-x86_64.rpm
SHA256SUMS
```

首个发布架构为 x86_64。软件包任务按架构隔离，因此将来可以添加原生 ARM64 runner
或交叉编译任务，而无需改变 x86_64 发布约定。

## 创建发布

将 CMake 项目版本设置为计划发布的稳定基础版本，并确保所有本地检查通过。创建并
推送稳定 tag 或候选发布 tag：

```bash
git tag -a v0.1.0 -m "MiniTun v0.1.0"
git push origin v0.1.0

git tag -a v0.2.0-rc.1 -m "MiniTun v0.2.0-rc.1"
git push origin v0.2.0-rc.1
```

可接受的 tag 为 `vMAJOR.MINOR.PATCH` 和 `vMAJOR.MINOR.PATCH-rc.NUMBER`。
基础版本必须与 `CMakeLists.txt` 中的 `project(VERSION ...)` 一致。稳定 tag 会创建
普通的 latest release；RC tag 会创建 prerelease。只有两个软件包任务及其干净容器
冒烟测试全部成功后，发布才会执行。

发布任务会下载两组软件包产物，校验全部四个预期文件，重新生成并验证合并后的
`SHA256SUMS`，生成发布说明，然后使用仓库范围的 GitHub Token 上传软件包与清单。
