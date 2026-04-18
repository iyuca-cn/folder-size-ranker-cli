# GitHub Actions 与项目更名设计

## 目标

为当前仓库添加 GitHub CI/CD：

- 普通 `push` 与 `pull_request` 自动执行 Windows 编译检查。
- 推送 annotated tag 时自动创建 GitHub Release。
- Release 上传由 GitHub Actions 构建出的可执行文件。
- Release notes 使用 annotated tag 的完整多行描述。

同时把项目名称统一改为 `folder-size-ranker-cli`，并更新 README 中的构建说明，去掉本机绝对路径。

## 设计

### 工作流拆分

拆成两个工作流：

1. `ci.yml`
   - 触发：`push`、`pull_request`
   - 平台：`windows-latest`
   - 步骤：
     - checkout
     - setup-msbuild
     - 构建 `Release|x64`
   - 目的：做语法和编译检查

2. `release.yml`
   - 触发：`push tags: v*`
   - 平台：`windows-latest`
   - 步骤：
     - checkout（带 tags）
     - setup-msbuild
     - 构建 `Release|x64`
     - 读取当前 annotated tag 的完整 message
     - 创建 GitHub Release
     - 上传 `folder-size-ranker-cli.exe`

### annotated tag 描述

要求使用 annotated tag，例如：

```powershell
git tag -a v1.0.0
```

在 Git 编辑器中编写多行说明。GitHub Actions 中通过 Git 读取 tag contents，作为 Release notes。

### 项目更名

统一改为 `folder-size-ranker-cli`：

- `mftscan.sln` → `folder-size-ranker-cli.sln`
- `mftscan.vcxproj` → `folder-size-ranker-cli.vcxproj`
- `mftscan.vcxproj.filters` → `folder-size-ranker-cli.vcxproj.filters`
- 输出 EXE 名改为 `folder-size-ranker-cli.exe`
- README 示例命令同步更新

### README 构建方式

README 不再出现本机绝对路径，改为：

- 在 Visual Studio Developer PowerShell / Developer Command Prompt 中执行 `msbuild`
- 使用相对路径构建项目文件

例如：

```powershell
msbuild .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64
```

## 验证

- 本地 `Release|x64` 构建成功。
- 工作流 YAML 语法正确。
- `ci.yml` 在 push/PR 下会构建。
- `release.yml` 在 `v*` tag 下会构建并生成 Release。
- README 中的项目名和命令与实际文件名一致。
