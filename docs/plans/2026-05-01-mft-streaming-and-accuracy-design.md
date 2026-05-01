# MFT 流式读取与解析准确性优化设计

## 背景

NTFS 快路径之前以「逐条 IOCTL」方式枚举 MFT：

```c
request_record = highest_record_number;
for (;;) {
    DeviceIoControl(handle, FSCTL_GET_NTFS_FILE_RECORD, &input, sizeof(input),
                    output, output_size, &bytes_returned, NULL);
    /* 解析 1 条记录 */
    request_record = actual_record - 1;
}
```

在 1TB 量级、记录数百万的卷上，这意味着：

1. 百万级内核态往返；每次 IOCTL 都要 FS 锁定 + 走完 NTFS 内部的 record-by-record 路径；
2. NTFS 驱动会在每条记录上做一次 USA 修复（再加上我们这一层重复的 USA 校验）；
3. 与扫描完全无关的 attribute-list extension 段也要重复 IOCTL（`mftscan_find_attribute_size_in_record` 已经这么做）。

同时 `src/ntfs_record.c` 自身存在两处准确性隐患：

- `mftscan_apply_update_sequence_array` 在 USN/扇区尾部不匹配时**静默返回 OK**，让带 torn write 的记录继续走解析路径；
- `mftscan_record_layout_is_valid` 只验证了 `first_attribute_offset / used_size / record_length` 三者大小关系，对 `allocated_size`、对齐、end-marker 余量都没要求；
- 同一份 mapping pairs 解码逻辑写了两遍（一份算 allocated size，一份做 attribute-list 拷贝），口径漂移风险高。

## 目标

- 用「读一次 $MFT runlist + 4MB 块顺序读」替换逐条 IOCTL，目标整盘扫描时间下降一个数量级；
- USA 校验失败不再吞错；非 FILE 签名 / 全零 / `BAAD` 标记记录被正确识别为「跳过」而不是「整盘扫描失败」；
- mapping pairs 只保留一份解码实现，allocated-size 累计、attribute-list 拷贝、$MFT runlist 发现都复用它；
- streaming 路径任何环节失败都自动 fallback 到原 IOCTL 单条读取，扫描永不中断。

## 非目标

- 不引入多线程；
- 不修改非 NTFS 平台 API 降级路径；
- 不修改聚合层、JSON/Table 输出层、命令行选项；
- 不处理 `$MFT` 自身 runlist 跨 `$ATTRIBUTE_LIST` extension 的极端情况（TB 级且 MFT 高度碎片的卷）—— 这种场景下 streaming 初始化失败，自动 fallback 到 IOCTL 全盘扫描。

## 方案

整个改造分三个独立 stage，每个 stage 对调用方都是渐进的，可独立 review。

### Stage 1：USA 校验硬化 + 记录头边界检查

**`mftscan_apply_update_sequence_array`** 函数签名增加一个 `bool *torn_write` 出参：

```c
static MftscanError mftscan_apply_update_sequence_array(
    uint8_t *record_buffer,
    size_t record_length,
    uint32_t bytes_per_sector,
    bool *torn_write);
```

- USN 与扇区尾部不一致 → 写 `*torn_write = true`，写 detail，返回 `MFTSCAN_ERROR_MFT_PARSE`（原来是静默返回 OK）；
- 同时新增 `update_sequence_count == (record_length / bytes_per_sector) + 1` 的强一致性校验。

**调用方** (`mftscan_parse_file_record` 与 `mftscan_find_attribute_size_in_record`)：

- `parse_file_record`：观察到 `torn_write` 为 true 时，把记录视作 `in_use=false`、清空 detail、返回 OK，避免单条 torn write 撕掉整盘扫描；
- `find_attribute_size_in_record`：仍硬错（attribute-list 引用的 extension 段坏掉本身就是数据不一致，应当报告）。

**`mftscan_record_layout_is_valid`** 新增三条检查：

| 检查 | 理由 |
|------|------|
| `(first_attribute_offset & 0x07) == 0` | NTFS 属性按 8 字节对齐 |
| `allocated_size <= record_length` | 记录头自报长度不能大于实际 buffer |
| `used_size >= first_attribute_offset + 8` | 至少留得下 end-marker (`type=0xFFFFFFFF, length=0`) |

**Signature 检查前移**：

streaming 路径会读到全零槽和 `BAAD` 损坏标记记录。把 `signature != 'FILE'` 的检查从原来 USA 之后挪到 USA 之前，且作为「跳过」而不是「错误」处理。这样 streaming 不会把空槽误判成解析失败。

### Stage 2：mapping pairs 解码统一

抽出唯一的 callback 风格迭代器（声明在 `model.h`，实现在 `ntfs_record.c`）：

```c
typedef MftscanError (*MftscanRunlistCallback)(
    void *user_data,
    uint64_t cluster_count,
    uint64_t starting_lcn,
    bool sparse);

MftscanError mftscan_iterate_mapping_pairs(
    const uint8_t *mapping_pairs,
    size_t mapping_pairs_length,
    MftscanRunlistCallback callback,
    void *user_data);
```

合并的语义要点：

- `length_size == 0` 或 `length_size > 8` 或 `offset_size > 8` 都是非法 header；
- `cluster_count == 0` 非法（NTFS 不允许零长度 run）；
- `offset_size == 0` 表示 sparse run，`starting_lcn` 未定义；
- `current_lcn` 用 `int64_t` 累加，回退后变负数判非法；
- 缓冲区遍历完没看到终止字节 (`run_header == 0`) 也判非法 —— mapping pairs 必须显式终止。

辅助函数 `mftscan_runlist_bounds_for_attribute` 把「从 non-resident header 取 mapping pairs 起止」这一段也抽出来，避免「`mapping_pairs_offset >= attribute_length`」之类的边界检查写两遍。

两个原有调用点重写为 callback 形式：

| 调用点 | callback 行为 |
|--------|---------------|
| `mftscan_calculate_nonresident_allocated_size` | 累加非 sparse run 的 `cluster_count * bytes_per_cluster`，溢出检查 |
| `mftscan_copy_nonresident_attribute_value` | 按 cluster 调 `mftscan_read_volume_bytes`；sparse 段 `memset(0)`；游标到 `data_size` 截止 |

Stage 3 的 $MFT runlist 发现也直接复用 `iterate_mapping_pairs`，三处口径完全一致。

### Stage 3：流式 MFT 读取

新文件 `src/ntfs_stream.c`，提供 record stream 抽象。

#### 数据结构

`MftscanVolumeHandle` 增加两个字段，从 `FSCTL_GET_NTFS_VOLUME_DATA` 同时填充：

```c
uint64_t mft_start_lcn;
uint64_t mft_valid_data_length;
```

新结构：

```c
typedef struct MftscanRunlistExtent {
    uint64_t starting_lcn;     /* 仅 sparse=false 时有效 */
    uint64_t cluster_count;
    bool sparse;
} MftscanRunlistExtent;

typedef struct MftscanRunlist {
    MftscanRunlistExtent *extents;
    size_t count, capacity;
    uint64_t total_clusters;
} MftscanRunlist;

typedef struct MftscanRecordStream {
    const MftscanVolumeHandle *volume_handle;
    MftscanRunlist runlist;             /* $MFT 的 $DATA runlist */
    uint8_t *chunk_buffer;              /* 4MB 块缓存 */
    size_t chunk_capacity_bytes;
    size_t chunk_records;
    uint64_t chunk_first_record;
    size_t chunk_valid_records;
    uint64_t total_records;             /* mft_valid_data_length / bytes_per_record */
    bool runlist_initialized;
    NTFS_FILE_RECORD_OUTPUT_BUFFER *fallback_buffer;
    size_t fallback_buffer_size;
} MftscanRecordStream;
```

#### Open 流程

`mftscan_record_stream_open`：

1. 用一次 IOCTL 取 FRN 0（即 `$MFT` 自己）；
2. 本地 USA 修复（独立实现，不依赖 `ntfs_record.c` 的内部静态符号）；
3. 找未命名 `$DATA`、非 resident、resident 在 base record 内的那一个 attribute；
4. `mftscan_iterate_mapping_pairs` → `MftscanRunlist`；
5. 分配 4MB chunk buffer，记 `chunk_first_record = UINT64_MAX`（未加载）；
6. 始终再分配一次 IOCTL 兜底 buffer，无论 streaming 是否成功。

任一步失败 → `runlist_initialized = false`，clear detail，整个 stream 退化成纯 IOCTL。

#### Get 流程

`mftscan_record_stream_get(stream, record_number, ...)`：

```
if (!runlist_initialized)
    return get_via_ioctl(record_number);            // 兜底

if (record_number >= total_records) return not-available;

chunk_first = (record_number / chunk_records) * chunk_records;
if (chunk_first_record != chunk_first) {
    if (load_chunk(chunk_first) failed) {
        clear detail;
        return get_via_ioctl(record_number);        // 单次 I/O 失败也兜底
    }
}
offset = record_number - chunk_first_record;
if (offset >= chunk_valid_records) return not-available;
*record_buffer = chunk_buffer + offset * bytes_per_record;
*available = true;
```

#### Load chunk 内部

`load_chunk(first_record)` 把 [first_record, first_record + chunk_records) 这一段 MFT 字节通过 runlist 翻译成卷字节偏移 + `mftscan_read_volume_bytes`：

- 计算当前 MFT 字节偏移所在的 VCN + cluster 内偏移；
- 在 runlist 里 O(extents) 线性查找 → LCN（或 sparse）；
- 在该 extent 内一次性算出能连续填多少字节，单次 ReadFile 直接落入 `chunk_buffer + bytes_filled`；
- sparse run 用 `memset(0)` 填；
- runlist 走完了但还没填够 → 把剩余清零并把 `chunk_valid_records` 调小（防御性，正常不应触发）。

记录在 `chunk_buffer` 内不跨 cluster（一条 record ≤ 1024 字节，cluster 普遍 ≥ 4KB），所以一条记录始终在一次 read 内完整落地。

#### 扫描循环改造

`mftscan_scan_volume_ntfs` 由原来的「FRN 从高往低、利用 IOCTL `at-or-below` 跳空」改为正向扫：

```c
for (current_record = 0; current_record < total_records; ++current_record) {
    stream_get(...);
    if (!available) continue;        // 空槽 / BAAD / 末尾
    parse_file_record(...);
    if (in_use) ingest_record(...);
}
```

正向扫的好处：

- 卷上是顺序大块读，预读和 OS cache 都在帮我们；
- 同一 chunk 内多条记录共享一次 I/O；
- 与 streaming 的天然访问方向一致，避免 cache 反向抖动。

代价：streaming fallback 退化到纯 IOCTL 时，正向扫不再利用 `at-or-below` 的跳空特性，会做约 N 次 IOCTL。可接受 —— fallback 路径本身就是 corner case。

## 正确性论证

| 担心 | 应对 |
|------|------|
| FRN 0 ($MFT 自身) 在循环里被解析 | 与原 IOCTL 路径一致：当 FRN 0 是 in-use 文件时一并入账，名字 `$MFT`，由聚合层正常处理 |
| MFT extension 记录 (`base_file_record != 0`) | `parse_file_record` 早返回 `goto cleanup` 跳过，与原行为一致 |
| 全零 MFT 槽 / `BAAD` 损坏标记 | Stage 1 已把 signature 检查前移并改为「跳过」，不会撕掉扫描 |
| Torn write 导致 USA 不一致 | Stage 1 标记 `in_use=false` 跳过该条，detail 提示扇区号与 USN |
| 同一记录被 in-place USA 修复后再次读取 | 正向单次访问 + 每条记录只解析一次；attribute-list extension 仍走独立 IOCTL buffer |
| `chunk_buffer` 在 stream 关闭前被释放 | `stream_close` 集中释放；外部不持有跨 chunk 的指针 |
| MftValidDataLength 与 runlist 不一致 | `vcn_to_lcn` 越界返回 false → 当前 chunk 多余字节清零 + `chunk_valid_records` 收敛，不会读越界 |
| Streaming 初始化失败 (例如 $MFT runlist 在 $ATTRIBUTE_LIST 内) | `runlist_initialized = false` → 整个 stream 自动走 IOCTL 兜底，行为退化到改造前 |

## 改动文件清单

| 文件 | 内容 |
|------|------|
| `src/ntfs_record.c` | Stage 1 + Stage 2：USA 加 `torn_write`、header 校验加固、signature 前移、合并 mapping pairs 解码、暴露 `mftscan_iterate_mapping_pairs` |
| `src/ntfs_stream.c` (新增) | Stage 3：runlist 工具 + `MftscanRecordStream` 实现 |
| `src/ntfs_mft.c` | Stage 3：扫描循环替换为 stream + 正向遍历 |
| `src/ntfs_volume.c` | Stage 3：填充 `mft_start_lcn` / `mft_valid_data_length` |
| `src/model.h` | 新数据结构、callback typedef、stream API、runlist API |
| `folder-size-ranker-cli.vcxproj(.filters)` | 注册 `ntfs_stream.c` |

## 后续可做（不在本次范围）

- $MFT 自身 runlist 跨 `$ATTRIBUTE_LIST` extension 时也支持 streaming（先收集所有 extension records，逐段拼出完整 runlist）；
- chunk 大小做成 runtime 可调，根据 MFT 大小自适应（小卷 1MB、大卷 8–16MB）；
- 把 chunk read I/O 与 record 解析做 producer/consumer 双线程，进一步压缩 wall time；
- 给 streaming 路径加可选的 progress 回调（百万级记录扫描时反馈状态）。
