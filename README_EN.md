<div align="center">

# folder-size-ranker-cli

**A fast command-line tool for calculating folder sizes on Windows**

[![Windows](https://img.shields.io/badge/Platform-Windows-0078D6?style=flat-square&logo=windows)](https://www.microsoft.com/windows)
[![C](https://img.shields.io/badge/Language-C-A8B9CC?style=flat-square&logo=c)](https://en.wikipedia.org/wiki/C_(programming_language))
[![CI](https://img.shields.io/github/actions/workflow/status/user/folder-size-ranker-cli/ci.yml?style=flat-square&label=CI)](https://github.com/user/folder-size-ranker-cli/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/License-MIT-green?style=flat-square)](LICENSE)
[![Release](https://img.shields.io/github/v/tag/user/folder-size-ranker-cli?style=flat-square&label=Release)](https://github.com/user/folder-size-ranker-cli/releases)

English | [中文](README.md)

</div>

## Introduction

`folder-size-ranker-cli` is a Windows command-line tool for quickly calculating folder sizes. It directly reads the MFT (Master File Table) on NTFS volumes and falls back to platform API scanning on non-NTFS volumes, achieving blazing-fast performance similar to [WizTree](https://www.diskanalyzer.com/).

## Key Features

- ⚡ **Lightning Fast** - Directly reads NTFS MFT without recursive directory traversal
- 📊 **Flexible Output** - Supports both table and JSON output formats
- 🔍 **Smart Sorting** - Sort by logical size or allocated size
- 🎯 **Precise Filtering** - Expression-based minimum size filtering
- 📁 **Full Tree Structure** - `--all` mode outputs complete directory tree as JSON
- 💻 **Dual Architecture** - Available in x86 and x64 versions

## Quick Start

### 1. Download

Download the latest version from the [Releases](https://github.com/user/folder-size-ranker-cli/releases) page:

- `folder-size-ranker-cli-x64.exe` - 64-bit version (recommended)
- `folder-size-ranker-cli-x86.exe` - 32-bit version

### 2. Run

```powershell
# Scan C: drive, sort by allocated size, show top 20 largest folders
.\folder-size-ranker-cli-x64.exe --location C: --sort allocated --limit 20

# Scan specific directory with JSON output
.\folder-size-ranker-cli-x64.exe --location C:\Users --sort logical --format json

# Output complete directory tree (compact JSON)
.\folder-size-ranker-cli-x64.exe --location C:\Projects --sort allocated --all
```

## Command Line Arguments

| Argument | Required | Default | Description |
|----------|----------|---------|-------------|
| `--location <path>` | ✅ | - | Scan location (e.g., `C:`, `C:\Windows`) |
| `--sort <logical\|allocated>` | ✅ | - | Sort method |
| `--all` | ❌ | Off | Output complete directory tree JSON |
| `--format <table\|json>` | ❌ | `table` | Output format (mutually exclusive with `--all`) |
| `--min-size <expr>` | ❌ | `0` | Minimum size filter (supports expressions) |
| `--limit <N>` | ❌ | Unlimited | Limit output count |
| `--help` | ❌ | - | Show help |

## Usage Examples

### Basic Usage

```powershell
# View top 20 largest folders on C: drive (allocated size)
.\folder-size-ranker-cli-x64.exe --location C: --sort allocated --limit 20

# View folders larger than 1GB on D: drive (logical size)
.\folder-size-ranker-cli-x64.exe --location D: --sort logical --min-size 1073741824
```

### Advanced Usage

```powershell
# Use expression to calculate minimum size (1.5GB)
.\folder-size-ranker-cli-x64.exe --location C: --sort allocated --min-size "1.5*1024*1024*1024"

# Output JSON format for script processing
.\folder-size-ranker-cli-x64.exe --location C: --sort allocated --format json --limit 50

# View complete directory tree, max 10 items per level
.\folder-size-ranker-cli-x64.exe --location C:\Users --sort allocated --all --limit 10
```

## Output Examples

### Table Output

```text
Bytes                Path
1416950032           C:\Windows\System32\DriverStore\FileRepository
524288000            C:\Program Files\Microsoft Visual Studio
262144000            C:\Users\YourName\AppData\Local\Temp
```

### JSON Output

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

## Build Instructions

Execute in **Visual Studio Developer PowerShell**:

```powershell
# 64-bit version
msbuild .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64

# 32-bit version
msbuild .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32
```

Build output locations:
- `x64\Release\folder-size-ranker-cli.exe`
- `x86\Release\folder-size-ranker-cli.exe`

## GitHub Actions

The project has GitHub Actions automation configured:

- **CI Build**: Automatically runs Windows build checks on every `push` or `pull_request`
- **Auto Release**: Automatically creates GitHub Release when pushing `v*` annotated tags
- **Dual Architecture Release**: Automatically uploads both x86 and x64 executables in the Release

### Release Process

```powershell
# Create annotated tag and push
git tag -a v1.0.0
git push origin v1.0.0
```

Write multi-line release notes in the Git editor that opens. After publishing, they will automatically appear on the GitHub Release page.

## Important Notes

- ⚠️ Scanning NTFS volumes requires **administrator privileges**
- Non-NTFS volumes automatically use platform API (slower)
- The program skips directory reparse points (junctions, symlinks) to avoid infinite loops
- Only supports local disk drives, not volumes mounted to folders

## License

[MIT License](LICENSE)
