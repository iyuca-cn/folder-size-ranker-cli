# NTFS Attribute List 逻辑大小修复设计

## 目标

修复 NTFS 快路径下 `--all` 输出中目录 `bytes` 的逻辑大小偏大问题，尤其是像 `C:\Temp` 这类目录在 `--sort logical` 时与资源管理器不一致的情况。

根治方式不是修改目录汇总算法，而是补齐 NTFS 文件记录中 `$ATTRIBUTE_LIST` 与 extension record 的解析，让单文件 `logical_size` 与 `allocated_size` 都来自完整的未命名 `$DATA` 数据流。

## 背景

当前实现优先从 base record 中读取未命名 `$DATA` 属性；若拿不到，再退回 `FILE_NAME.real_size`。这个兜底在简单文件上通常可用，但对带 `$ATTRIBUTE_LIST` 的复杂 NTFS 记录并不可靠，可能导致 `logical_size` 偏大。

`--all` 根节点 `bytes` 是对子目录和文件大小的递归汇总，因此单文件 `logical_size` 一旦偏大，目录总量也会随之偏大。现有 `aggregate` 与 `output_json` 的汇总逻辑本身不需要调整。

## 范围

- 修改 NTFS 快路径的记录解析流程。
- 支持从 `$ATTRIBUTE_LIST` 跳转到 extension record，补齐未命名 `$DATA` 的大小解析。
- 在需要时支持读取非驻留 `$ATTRIBUTE_LIST` 的内容。
- 保持现有 `allocated`/`logical` 输出接口不变。
- 保持平台 API 降级路径不变。

## 非目标

- 不改变 `--all` JSON 结构。
- 不把命名 ADS 计入目录大小。
- 不修改非 NTFS 平台 API 扫描逻辑。
- 不在本次设计中引入新的独立测试框架。

## 架构

### 读取层

`src/ntfs_mft.c` 目前只服务于“按 FRN 倒序全卷枚举”。本次需要把“按指定 FRN 读取单条 file record”的能力抽成可复用 helper，供 parser 在遇到 `$ATTRIBUTE_LIST` 时按需读取 extension record。

同时，`src/model.h` 中的 `MftscanVolumeHandle` 需要补齐做非驻留属性读取所需的卷几何信息，例如 cluster 大小，以便在需要时读取非驻留 `$ATTRIBUTE_LIST` 的实际字节。

### 解析层

`src/ntfs_record.c` 改为两阶段解析：

1. **Base record 解析阶段**
   - 提取文件名、父目录、目录标记。
   - 提取 base record 内可直接读取的未命名 `$DATA`。
   - 识别 `$ATTRIBUTE_LIST` 是否存在，并缓存其内容或位置信息。

2. **Attribute list 扩展解析阶段**
   - 解析 list entry。
   - 只跟进 `type == $DATA` 且 `name_length == 0` 的未命名数据流。
   - 按 list entry 指向的 FRN 读取 extension record。
   - 找到同一条未命名 `$DATA` 的属性片段。
   - 以 `lowest_vcn == 0` 的片段作为整条数据流的权威大小来源。

### 最终大小决策

最终大小来源优先级统一为：

1. 通过 `$ATTRIBUTE_LIST` + extension record 完整解析出的未命名 `$DATA`
2. base record 内直接可得的未命名 `$DATA`
3. 仅当完全不存在未命名 `$DATA` 且没有 `$ATTRIBUTE_LIST` 时，才退回 `FILE_NAME.real_size`

这样可以避免“复杂记录静默退回 `FILE_NAME.real_size`”导致的逻辑大小偏大。

## 数据流

### 正常文件

对没有 `$ATTRIBUTE_LIST` 的普通文件，解析流程与当前版本接近，只是把大小决策集中到统一出口，避免不同分支各自写入 `record_info->logical_size`。

### 带 `$ATTRIBUTE_LIST` 的文件

1. 解析 base record，记录其 FRN、base FRN、attribute list。
2. 遍历 attribute list entry，只保留未命名 `$DATA`。
3. 若 entry 指向 base record，则继续在 base record 中解析对应属性。
4. 若 entry 指向 extension record，则调用新的单 record 读取 helper 拉取目标记录。
5. 校验 extension record 与 base file 的关联关系。
6. 找到匹配的 `$DATA` 片段后，使用 `lowest_vcn == 0` 片段上的 `data_size` / `allocated_size` 作为整条流的最终大小。

目录输出、叶子目录输出和 `--all` 递归汇总继续使用修正后的单文件大小，不需要改动汇总算法。

## 错误处理

### Fail-fast 场景

以下情况直接返回 NTFS 解析错误，不再静默退回 `FILE_NAME.real_size`：

- `$ATTRIBUTE_LIST` 结构越界或畸形
- list entry 指向的 extension record 无法读取
- extension record 不可用、损坏，或与 base file 不匹配
- 非驻留 `$ATTRIBUTE_LIST` 的 runlist 无法正确解析
- 解析链路出现循环引用或超过合理跳转上限

### 忽略场景

以下情况直接忽略，不视为错误：

- 命名 `$DATA`（ADS）
- 与未命名数据流无关的 attribute list entry
- `lowest_vcn != 0` 的后续数据片段本身

### 安全保护

- 跟随 extension record 时记录已访问 FRN，避免循环。
- 对单文件可跟随的 extension 数量设置上限，超过即报错。
- 所有 attribute 与 list entry 均执行边界检查。

## 验证

### 回归目标

- `--sort logical --all` 下的目录 `bytes` 不再系统性偏大。
- `--sort allocated` 行为保持不变。
- 命名 ADS 不被计入目录逻辑大小。
- 复杂 NTFS 文件不再因为静默退回 `FILE_NAME.real_size` 而出现“看起来成功但数值错误”的情况。

### 验证方式

- 构建 `x64 Release` 并在管理员终端下运行 NTFS 快路径。
- 对 `C:\Temp` 执行 `--sort logical --all`，核对根节点 `bytes` 与资源管理器。
- 对同一目录执行 `--sort allocated --all`，确认未引入回归。
- 再对普通目录做一次 smoke test，确认无 `$ATTRIBUTE_LIST` 的文件解析路径仍正常。
