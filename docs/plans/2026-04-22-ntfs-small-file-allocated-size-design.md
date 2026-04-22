# NTFS 小文件分配大小修复设计

## 背景

当前 MFT 解析逻辑把 resident 未命名 `$DATA` 的 `allocated_size` 直接记成 `value_length`。这会把 NTFS 小文件错误地统计为“已分配了数据簇”，从而与资源管理器显示的“占用空间”不一致。

用户确认的目标口径不是按文件大小阈值猜测，而是按**当前未命名 `$DATA` 的驻留形态**判断：

- 当前 `$DATA` 是 resident：`allocated_size = 0`
- 当前 `$DATA` 是 non-resident：`allocated_size = 属性头里的 allocated_size`
- 如果文件曾经变大并转成 non-resident，之后逻辑大小缩小到 `1B`，只要当前仍是 non-resident，就继续保留 `4KB` 之类的分配大小
- `0B` 文件的分配大小仍应为 `0`

## 方案

只改 `src/ntfs_record.c` 中未命名 `$DATA` 的大小候选生成逻辑，不引入新的 Win32 API 调用：

1. resident 未命名 `$DATA`
   - `logical_size = value_length`
   - `allocated_size = 0`
2. non-resident 未命名 `$DATA`
   - `logical_size = data_size`
   - `allocated_size = allocated_size`
3. `$ATTRIBUTE_LIST` 跟到扩展记录时，继续沿用同一规则

## 影响

- 叶子目录的 `allocated_size` 汇总会更接近资源管理器的“占用空间”
- 逻辑大小统计不变
- 命名 ADS 仍然忽略，不计入目录大小
- 不需要修改聚合层和输出层
