# `--all` 扫描性能优化设计

## 背景

`--all` 模式会输出指定目录下的完整 JSON 树。当前 NTFS 快路径虽然通过 MFT 避免递归遍历目录，但仍存在两个明显成本：

1. 修复 `WindowsApps` 压缩文件分配大小后，新增的 `GetCompressedFileSizeW` 回填会扫描所有可疑文件，即使本次输出是逻辑大小或目标只是某个子目录。
2. `--all + 子目录` 仍会先构建全卷目录树，再通过路径匹配定位输出根目录，目标子树外的数据也会参与部分后处理。

基线测试中，`C:\Program Files\WindowsApps --sort allocated --all` 约 7.3 秒，`--sort logical --all` 约 8.3 秒。逻辑大小模式仍执行了不需要的分配大小回填，是最直接的优化点。

## 目标

- 优先优化 `--all` 模式，尤其是 `--location` 指向子目录的场景。
- `--sort logical --all` 不再执行分配大小回填。
- `--sort allocated --all` 只回填目标输出子树内的可疑文件。
- 保持现有 JSON 结构、排序规则、`bytes` 语义不变。

## 非目标

- 不改变硬链接去重口径。
- 不引入多线程扫描。
- 不重写 MFT 枚举方式。
- 不改变非 NTFS 平台 API 降级路径。

## 方案

### 1. 按排序模式跳过回填

把 `mftscan_backfill_zero_allocated_files` 改为接收 `MftscanOptions`：

- `--sort logical`：直接返回，不调用 `GetCompressedFileSizeW`。
- `--sort allocated`：保持现有可疑文件回填逻辑。

这能立刻降低 `--sort logical --all` 的运行时间，并避免输出不需要的数据计算。

### 2. 限定 `--all` 子树回填范围

新增基于目录 FRN 的子树判断：

- 对 `--location` 指向盘符根目录的情况，仍允许全卷回填。
- 对 `--location` 指向子目录的情况，先定位该目录对应的 `root_index` / FRN。
- 回填文件前沿着父目录链检查该文件是否属于目标子树；不属于则跳过。

这样 `C:\Program Files\WindowsApps --sort allocated --all` 不再为 `C:` 盘其他目录的可疑文件调用 Win32 API。

### 3. 保持输出构建兼容

现有 `mftscan_output_all_json` 的树构建和排序不变。此次只减少回填阶段的无关工作，避免扩大改动面。

## 风险与处理

- **找不到目标子目录**：保持现有错误路径，让输出阶段返回内部错误。
- **目录父链不完整**：子树判断失败时跳过该文件，避免错误计入目标子树。
- **allocated 根目录扫描**：盘符根目录仍全卷回填，保证现有结果不退化。

## 验证

- 编译 x64 和 Win32 Release。
- 对比 `C:\Program Files\WindowsApps --sort allocated --all` 根节点 `bytes`，确认不回退。
- 对比 `C:\Program Files\WindowsApps --sort logical --all` 根节点 `bytes`，确认逻辑大小不变且耗时降低。
- 跑 `C: --sort allocated --format table --limit 1`，确认默认输出路径仍可用。
