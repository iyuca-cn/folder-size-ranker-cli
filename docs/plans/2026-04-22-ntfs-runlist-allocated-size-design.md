# NTFS runlist 分配大小修复设计

## 背景

此前实现对 non-resident 属性直接使用属性头中的 `allocated_size`。这对普通文件多数情况下可用，但对 `$UsnJrnl:$J` 这类稀疏（sparse）系统流会把虚拟跨度误当成真实占用，导致分配大小被明显放大。

同时，`$Extend` 下还有一些系统记录没有未命名 `$DATA`，真实占用来自命名系统流或目录元数据属性。

## 根因

NTFS non-resident 属性的真实占用应按 runlist 中实际映射到磁盘的 runs 计算：

- 非 sparse run：计入 `allocated_size`
- sparse hole：不计入 `allocated_size`

只看属性头里的聚合值，无法可靠反映这类系统流的真实占用。

## 方案

1. 在 `src/ntfs_record.c` 中新增 runlist-based allocated 计算 helper
2. 未命名 non-resident `$DATA` 改为按 runlist 中真实已分配 runs 求和
3. `$ATTRIBUTE_LIST` 中未命名 `$DATA` 的分配大小改为累计所有片段的实际已分配 runs
4. 对 `FILE_ATTRIBUTE_SYSTEM` 的记录，若没有未命名 `$DATA`，允许使用命名系统流或目录元数据属性作为 `allocated_size` 兜底
5. 目录元数据只补 `allocated_size`，不改 `logical_size`

## 风险控制

- resident 小文件的 `allocated_size = 0` 逻辑保持不变
- 普通命名 ADS 仍不计入目录大小
- 只有系统记录才启用命名系统流/目录元数据兜底，避免把普通文件 ADS 误计入
