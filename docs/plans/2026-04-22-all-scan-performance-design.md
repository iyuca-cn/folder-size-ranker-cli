# `--all` 扫描性能优化设计

## 背景

`--all` 模式会输出指定目录下的完整 JSON 树。当前 NTFS 快路径虽然通过 MFT 避免递归遍历目录，但仍存在两个明显成本：

1. `WindowsApps` 这类 AppX/WOF 文件的真实占用不在未命名 `$DATA`，而在命名流 `:WofCompressedData`。此前若用 Win32 API 兜底，虽然能补数，但会引入口径和性能问题。
2. `--all + 子目录` 仍会先构建全卷目录树，但真正的性能热点已经从 JSON 组装转移到“如何准确拿到 allocated_size”。

基线测试中，`C:\Program Files\WindowsApps --sort allocated --all` 约 7.3 秒，`--sort logical --all` 约 8.3 秒。逻辑大小模式仍执行了不需要的分配大小回填，是最直接的优化点。

## 目标

- 优先优化 `--all` 模式，尤其是 `--location` 指向子目录的场景。
- `--all` 模式不再依赖 Win32 API 回填分配大小。
- WOF/AppX 文件的 `allocated_size` 直接来自 MFT 中的 `:WofCompressedData` 命名流。
- 保持现有 JSON 结构、排序规则、`bytes` 语义不变。

## 非目标

- 不改变硬链接去重口径。
- 不引入多线程扫描。
- 不重写 MFT 枚举方式。
- 不改变非 NTFS 平台 API 降级路径。

## 方案

### 1. 在 NTFS 解析阶段识别 WOF backing stream

在 `src/ntfs_record.c` 中识别命名 `$DATA` 流 `WofCompressedData`：

- 未命名 `$DATA` 继续负责 `logical_size`
- `:WofCompressedData` 只提供 `allocated_size`
- 普通命名 ADS 仍然忽略

这样 `WindowsApps` 的占用直接来自 MFT，不再依赖 Win32 API。

### 2. 移除 NTFS 快路径中的 Win32 回填

删除聚合层里对 `GetCompressedFileSizeW` 的可疑文件回填逻辑，让 `--all`、表格、JSON 等路径统一依赖同一份 MFT 解析结果。

这同时解决两件事：

- 避免 Win32 API 带来的口径偏差
- 避免为大量文件额外做路径拼接和文件句柄访问

### 3. 保持输出构建兼容

现有 `mftscan_output_all_json` 的树构建和排序不变。此次只修正 `allocated_size` 的来源，不改 JSON 结构和排序行为。

## 风险与处理

- **误把普通 ADS 计入占用**：只对白名单流名 `WofCompressedData` 启用 allocated 兜底。
- **attribute list 扩展记录遗漏**：base record 与 extension record 都要识别 `WofCompressedData`。
- **性能退化**：去掉 Win32 回填后，`--all` 应明显更快，不应比旧实现更慢。

## 验证

- 编译 x64 和 Win32 Release。
- 对比 `C:\Program Files\WindowsApps --sort allocated --all` 根节点 `bytes`，确认恢复到正确口径。
- 对比 `C:\Program Files\WindowsApps --sort logical --all` 根节点 `bytes`，确认逻辑大小不变。
- 跑 `C: --sort allocated --format table --limit 1`，确认默认输出路径仍可用。
