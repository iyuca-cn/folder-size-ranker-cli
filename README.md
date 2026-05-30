<div align="center">

# folder-size-ranker-cli

**快速统计 Windows 文件夹大小的命令行工具**

[![Windows](https://img.shields.io/badge/Platform-Windows-0078D6?style=flat-square&logo=windows)](https://www.microsoft.com/windows)
[![C](https://img.shields.io/badge/Language-C-A8B9CC?style=flat-square&logo=c)](https://en.wikipedia.org/wiki/C_(programming_language))
[![CI](https://img.shields.io/github/actions/workflow/status/user/folder-size-ranker-cli/ci.yml?style=flat-square&label=CI)](https://github.com/user/folder-size-ranker-cli/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/License-MIT-green?style=flat-square)](LICENSE)
[![Release](https://img.shields.io/github/v/tag/user/folder-size-ranker-cli?style=flat-square&label=Release)](https://github.com/user/folder-size-ranker-cli/releases)

[English](README_EN.md) | 中文

</div>

## 简介

`folder-size-ranker-cli` 是一个 Windows 命令行工具，用于快速统计文件夹大小。NTFS 卷直接读取 MFT（主文件表），非 NTFS 卷自动降级为平台 API 扫描，实现类似 [WizTree](https://www.diskanalyzer.com/) 的极速统计。

## 核心特性

- ⚡ **极速扫描** - NTFS 卷直接读取 MFT，无需递归遍历目录
- 📊 **灵活输出** - 支持表格和 JSON 两种输出格式
- 🔍 **智能排序** - 按逻辑大小或分配大小排序
- 🎯 **精准过滤** - 支持表达式计算的最小大小过滤
- 📁 **完整树结构** - `--all` 模式输出完整目录树 JSON
- 💻 **双架构支持** - 提供 x86 和 x64 两个版本

## 快速开始

### 1. 下载

从 [Releases](https://github.com/user/folder-size-ranker-cli/releases) 页面下载最新版本：

- `folder-size-ranker-cli-x64.exe` - 64 位版本（推荐）
- `folder-size-ranker-cli-x86.exe` - 32 位版本

### 2. 运行

```powershell
# 扫描 C 盘，按分配大小排序，显示前 20 个最大文件夹
.\folder-size-ranker-cli-x64.exe --location C: --sort allocated --limit 20

# 扫描指定目录，输出 JSON 格式
.\folder-size-ranker-cli-x64.exe --location C:\Users --sort logical --format json

# 输出完整目录树（紧凑 JSON）
.\folder-size-ranker-cli-x64.exe --location C:\Projects --sort allocated --all
```

## 命令行参数

| 参数 | 必填 | 默认值 | 说明 |
|------|------|--------|------|
| `--location <path>` | ✅ | - | 扫描位置（如 `C:`、`C:\Windows`） |
| `--sort <logical\|allocated>` | ✅ | - | 排序方式 |
| `--all` | ❌ | 关闭 | 输出完整目录树 JSON |
| `--format <table\|json>` | ❌ | `table` | 输出格式（与 `--all` 互斥） |
| `--min-size <expr>` | ❌ | `0` | 最小大小过滤（支持表达式） |
| `--limit <N>` | ❌ | 不限制 | 限制输出数量 |
| `--help` | ❌ | - | 显示帮助 |

## 使用示例

### 基础用法

```powershell
# 查看 C 盘最大的 20 个文件夹（分配大小）
.\folder-size-ranker-cli-x64.exe --location C: --sort allocated --limit 20

# 查看 D 盘逻辑大小超过 1GB 的文件夹
.\folder-size-ranker-cli-x64.exe --location D: --sort logical --min-size 1073741824
```

### 高级用法

```powershell
# 使用表达式计算最小大小（1.5GB）
.\folder-size-ranker-cli-x64.exe --location C: --sort allocated --min-size "1.5*1024*1024*1024"

# 输出 JSON 格式，便于脚本处理
.\folder-size-ranker-cli-x64.exe --location C: --sort allocated --format json --limit 50

# 查看完整目录树，每层最多 10 个子项
.\folder-size-ranker-cli-x64.exe --location C:\Users --sort allocated --all --limit 10
```

## 输出示例

### 表格输出

```text
Bytes                Path
1416950032           C:\Windows\System32\DriverStore\FileRepository
524288000            C:\Program Files\Microsoft Visual Studio
262144000            C:\Users\YourName\AppData\Local\Temp
```

### JSON 输出

```json
{
  "volume": "C:",
  "location": "C:\\Windows",
  "sort_by": "allocated",
  "min_size": 0,
  "total_leaf_dirs": 2,
  "items": [
    {
      "path": "C:\\Windows\\System32",
      "bytes": 123469824
    }
  ]
}
```

## 构建说明

在 **Visual Studio Developer PowerShell** 中执行：

```powershell
# 64 位版本
msbuild .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64

# 32 位版本
msbuild .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32
```

构建产物位置：
- `x64\Release\folder-size-ranker-cli.exe`
- `x86\Release\folder-size-ranker-cli.exe`

## GitHub Actions

项目配置了 GitHub Actions 自动化工作流：

- **CI 构建**：每次 `push` 或 `pull_request` 自动执行 Windows 编译检查
- **自动发版**：推送 `v*` 格式的 annotated tag 自动创建 GitHub Release
- **双架构发布**：Release 自动上传 x86 和 x64 两个版本的可执行文件

### 发版流程

```powershell
# 创建 annotated tag 并推送
git tag -a v1.0.0
git push origin v1.0.0
```

在 Git 打开的编辑器中编写多行版本说明，发布后会自动显示在 GitHub Release 页面。

## 注意事项

- ⚠️ 扫描 NTFS 卷需要**管理员权限**
- 非 NTFS 卷自动使用平台 API，速度较慢
- 程序会跳过目录型 reparse point（junction、符号链接）避免循环扫描
- 仅支持本地磁盘盘符，不支持挂载到文件夹的卷

## 许可证

[MIT License](LICENSE)
