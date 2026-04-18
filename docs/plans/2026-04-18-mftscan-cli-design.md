# MFT 叶子目录统计 CLI 设计

## 目标

实现一个基于 C 语言和 Visual Studio 工程的 Windows CLI 工具，直接读取指定 NTFS 卷的 MFT，统计“没有子文件夹的文件夹”的大小，并支持按逻辑大小或分配大小降序输出结果，输出格式支持表格和 JSON。

## 约束

- 仅支持单次扫描一个卷，盘符通过命令行参数传入。
- 要求管理员权限运行，优先通过应用清单自动提权。
- 仅支持 NTFS 卷。
- 输出对象仅限叶子目录，即没有任何子目录的目录。
- 硬链接按文件记录去重，同一个文件实体只统计一次。
- JSON 输出使用 `yyjson`。
- 交付形式为单 EXE 的 Visual Studio 项目，但源码按多文件拆分。

## 命令行接口

建议命令行形式：

```text
mftscan.exe --volume C: --sort allocated --min-size 1048576 --format table --limit 100
```

参数定义：

- `--volume <X:>`：必填，只扫描一个卷。
- `--sort <logical|allocated>`：必填，决定排序字段，同时也决定 `--min-size` 的过滤字段。
- `--min-size <bytes>`：可选，默认 `0`。
- `--format <table|json>`：可选，默认 `table`。
- `--limit <N>`：可选，默认不限。
- `--help`：打印帮助。

## 核心行为

- 打开 `\\.\X:` 设备句柄并确认卷为 NTFS。
- 通过 `FSCTL_GET_NTFS_VOLUME_DATA` 获取 MFT 记录大小等元数据。
- 通过 `FSCTL_GET_NTFS_FILE_RECORD` 顺序遍历在用的 MFT 文件记录。
- 从文件记录中解析：
  - 文件记录号（FRN）
  - 父目录 FRN
  - 是否为目录
  - 名称
  - 逻辑大小
  - 分配大小
- 扫描阶段建立目录索引和“目录是否有子目录”标记。
- 普通文件只向其直接父目录累计大小；由于最终只输出叶子目录，叶子目录的结果天然等于该目录直属文件总和。
- 目录结果只保留 `has_child_dir == false` 的目录。
- 排序与最小值过滤根据 `--sort` 决定使用 `logical_size` 或 `allocated_size`。

## 内部结构

建议按职责拆分源码：

- `src/main.c`：参数解析、流程控制。
- `src/admin.c`：管理员检查。
- `src/ntfs_volume.c`：卷打开、卷信息读取、NTFS 校验。
- `src/ntfs_mft.c`：MFT 记录枚举。
- `src/ntfs_record.c`：单条 MFT 记录解析与 fixup 处理。
- `src/model.h`：核心结构体和枚举。
- `src/aggregate.c`：文件大小累计、叶子目录筛选、排序。
- `src/path_resolver.c`：为最终结果恢复完整路径。
- `src/output_table.c`：表格输出。
- `src/output_json.c`：基于 `yyjson` 的 JSON 输出。
- `src/util.c`：动态数组、字符串、转换工具。
- `include/mftscan.h`：公共声明。
- `third_party/yyjson/*`：第三方源码。
- `app.manifest`：自动提权清单。

## 路径与命名策略

- 一个目录可能存在多个 `FILE_NAME` 属性。
- 选择名称时优先使用 Win32 / Win32&DOS 名称；如果没有，再退回其他命名空间。
- 对硬链接文件，只统计第一次选中的目录项。
- 输出完整路径时，从叶子目录沿 `parent_frn` 回溯到根目录，再拼成 `C:\...`。

## 错误处理

- 所有内部函数使用统一错误码。
- 错误信息输出到 `stderr`。
- 典型错误包括：
  - 参数非法
  - 权限不足
  - 非 NTFS 卷
  - 打开卷失败
  - MFT 记录读取失败
  - 记录解析失败
  - 内存不足
  - JSON 生成失败

## 输出格式

### 表格输出

至少包含以下列：

- `LogicalBytes`
- `AllocatedBytes`
- `Path`

### JSON 输出

顶层结构：

```json
{
  "volume": "C:",
  "sort_by": "allocated",
  "min_size": 1048576,
  "total_leaf_dirs": 123,
  "items": [
    {
      "path": "C:\\Users\\foo\\Downloads",
      "logical_size": 123456789,
      "allocated_size": 130023424
    }
  ]
}
```

JSON 使用 UTF-8 编码。

## 性能策略

- 只解析必要属性，不做全量属性展开。
- 扫描阶段不拼路径，路径恢复延迟到最终结果阶段。
- 结果只构造叶子目录条目，减少字符串与 JSON 节点分配。
- 第一版先单线程，以正确性和稳定性优先。

## 验证范围

- 正常扫描 NTFS 卷并输出表格。
- 正常扫描 NTFS 卷并输出 JSON。
- `--sort logical` 和 `--sort allocated` 两种模式。
- `--min-size` 和 `--limit` 生效。
- 非管理员运行失败。
- 非 NTFS 卷失败。
- 非法盘符失败。
- 空结果时输出合法表格/JSON。
- 硬链接文件不重复计数。
- 非叶子目录不进入结果。

## 备注

当前工作区不是 git 仓库，因此设计文档可直接落盘，但无法执行“提交设计文档”这一步，除非后续初始化仓库。
