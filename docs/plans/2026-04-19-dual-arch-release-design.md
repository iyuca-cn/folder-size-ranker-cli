# x86 本地构建与双架构 Release 设计

## 背景

当前仓库的 Visual Studio 工程和解决方案只配置了 `x64`，因此本地无法直接生成 `Win32/x86` 的 Release。GitHub Release 工作流也只构建并上传 `x64` 的 `folder-size-ranker-cli.exe`。

本次需求有两点：

- 确认本地可以生成 `x86` 的 Release。
- 所有 GitHub Actions 发版时，同时发布 `x86` 和 `x64` 两个单文件 exe，并分别命名为 `folder-size-ranker-cli-x86.exe` 与 `folder-size-ranker-cli-x64.exe`。

## 目标

- 为现有 `.sln` 和 `.vcxproj` 补齐 `Win32` 的 `Debug/Release` 配置。
- 保持当前工程结构不变，不新增第二套工程文件。
- Release 构建产物保持为单个 exe 文件，GitHub Release 直接上传两个重命名后的 exe。
- 普通 CI 仍保持现状，只调整 Release 工作流。

## 方案

采用“补齐原工程多平台配置 + 在 Release 工作流中顺序构建双架构”的最小改动方案：

1. 在 `folder-size-ranker-cli.sln` 中加入 `Debug|Win32` 与 `Release|Win32`。
2. 在 `folder-size-ranker-cli.vcxproj` 中加入 `Win32` 的配置块、属性表导入和编译/链接参数。
3. 将输出目录按架构分离为 `x86\Release\` 与 `x64\Release\`，便于本地验证与工作流取件。
4. Release 配置显式使用静态 CRT，使发布产物保持为单文件 exe。
5. 在 `.github/workflows/release.yml` 中先后构建 `x86` 与 `x64`，再复制重命名后上传到同一个 GitHub Release。

## 边界

- 本次不修改普通 `push` / `pull_request` 的 CI 流程。
- 本次不改源码逻辑，仅调整工程配置、发布流程和文档。
- `x86` 本地输出目录命名为 `x86\Release\`，与 GitHub Release 附件命名保持语义一致。

## 受影响文件

- `folder-size-ranker-cli.sln`：补充 `Win32` 解决方案配置映射。
- `folder-size-ranker-cli.vcxproj`：补充 `Win32` 工程配置、输出目录和 Release 静态运行库。
- `.github/workflows/release.yml`：双架构构建并上传两个 exe。
- `README.md`：更新本地构建与发版说明。

## 验证

- 本地执行 `msbuild .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32`。
- 确认生成 `x86\Release\folder-size-ranker-cli.exe`。
- 检查 Release 工作流会上传：
  - `folder-size-ranker-cli-x86.exe`
  - `folder-size-ranker-cli-x64.exe`
