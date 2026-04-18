# mftscan

`mftscan` 是一个 Windows 命令行工具，用 C 语言实现，通过直接读取指定 NTFS 卷的 MFT 来统计文件夹大小。

它的目标是做类似 WizTree 的快速统计，但当前第一版只输出“没有子文件夹的文件夹”，也就是叶子目录，并按大小从大到小排序。

## 作用

- 直接读取指定盘符的 NTFS MFT，不递归遍历目录。
- 统计叶子目录的文件大小。
- 支持按逻辑大小或分配大小排序。
- 支持设置最小输出大小。
- 支持表格输出和 JSON 输出。

## 统计口径

本工具当前只输出叶子目录：

- 叶子目录：目录下面没有任何子文件夹。
- 非叶子目录：目录下面还有子文件夹，不会出现在输出结果里。
- 目录大小：该叶子目录直属文件的大小总和。
- 硬链接：按文件 MFT 记录去重，同一个文件实体只统计一次。

输出结果始终只有一个大小字段：

- 表格输出列名固定为 `Bytes`
- JSON 输出字段固定为 `bytes`
- 这个字段的含义由 `--sort` 决定：
  - `--sort logical` 时表示逻辑大小
  - `--sort allocated` 时表示分配大小

## 运行要求

- Windows
- NTFS 卷
- 管理员权限
- x64 构建

程序清单 `app.manifest` 已设置为 `requireAdministrator`，双击或从普通终端启动时 Windows 会请求提权。运行时也会再次检查管理员权限。

## 构建

使用 Visual Studio 2022 或 MSBuild 构建：

```powershell
& "D:\vs\2022\Community\MSBuild\Current\Bin\MSBuild.exe" .\mftscan.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64
```

构建产物：

```text
x64\Release\mftscan.exe
```

如果你的 Visual Studio 安装路径不同，请改成你本机的 `MSBuild.exe` 路径。

## CLI 用法

```text
mftscan.exe --volume C: --sort <logical|allocated> [--min-size bytes] [--format <table|json>] [--limit N]
```

参数说明：

| 参数 | 必填 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `--volume <X:>` | 是 | 无 | 指定要扫描的单个盘符，例如 `C:` |
| `--sort <logical|allocated>` | 是 | 无 | 指定排序字段 |
| `--min-size <bytes>` | 否 | `0` | 只输出大小大于等于该值的目录 |
| `--format <table|json>` | 否 | `table` | 输出表格或 JSON |
| `--limit <N>` | 否 | 不限制 | 只输出前 N 条 |
| `--help` | 否 | 无 | 显示帮助 |

`--min-size` 使用的字段和 `--sort` 一致：

- `--sort logical`：按逻辑大小过滤和排序。
- `--sort allocated`：按分配大小过滤和排序。

## 示例

输出 `C:` 盘最大的 20 个叶子目录，按分配大小排序：

```powershell
.\x64\Release\mftscan.exe --volume C: --sort allocated --format table --limit 20
```

输出 `C:` 盘逻辑大小至少 1 GiB 的叶子目录：

```powershell
.\x64\Release\mftscan.exe --volume C: --sort logical --min-size 1073741824 --format table
```

输出 JSON，方便脚本处理：

```powershell
.\x64\Release\mftscan.exe --volume C: --sort allocated --min-size 104857600 --format json --limit 50
```

## 表格输出

表格输出包含两列：

```text
Bytes                Path
1416950032           C:\Windows\System32\DriverStore\FileRepository\...
```

## JSON 输出

JSON 输出为 UTF-8，结构如下：

```json
{
  "volume": "C:",
  "sort_by": "allocated",
  "min_size": 104857600,
  "total_leaf_dirs": 2,
  "items": [
    {
      "path": "C:\\Example\\LeafFolder",
      "bytes": 123469824
    }
  ]
}
```

字段说明：

- `volume`：扫描的盘符。
- `sort_by`：排序字段。
- `min_size`：最小大小过滤值。
- `total_leaf_dirs`：输出结果条数。
- `items`：已经按指定字段降序排序的叶子目录列表，每项只包含 `bytes` 和 `path`。

## 注意事项

- 该工具读取 NTFS 底层结构，必须管理员权限运行。
- 当前只支持单个盘符扫描。
- 当前只输出叶子目录，不输出父目录汇总。
- 当前第一版优先保证正确性和结构清晰，后续可以继续优化性能和输出格式。
