# 非 NTFS 平台 API 降级设计

## 目标

在保留 NTFS MFT 快速扫描路径的前提下，为非 NTFS 文件系统增加平台 API 降级扫描能力。降级路径不使用递归函数，而是使用显式栈迭代遍历目录树，并保持现有叶子目录统计口径。

## 范围

- NTFS 卷继续使用现有 MFT 后端，仍需要管理员权限。
- 非 NTFS 卷使用 Win32 平台 API 后端，不做管理员前置要求。
- 输出仍然只包含没有子文件夹的目录。
- 目录大小仍然只统计直属文件，不汇总子目录。
- 目录型 reparse point 直接跳过，不跟进、不计为子目录。

## 架构

公共扫描入口 `mftscan_scan_volume()` 负责探测卷文件系统类型并分发：

- `NTFS`：检查管理员权限后调用 `mftscan_scan_volume_ntfs()`。
- 其他文件系统：调用 `mftscan_scan_volume_platform()`。

平台 API 后端将目录和文件转换成现有 `MftscanRecordInfo` 数据结构，再复用 `mftscan_ingest_record()`、`mftscan_build_results()`、表格输出和 JSON 输出。

## 平台 API 数据流

平台 API 后端从根目录开始，把待扫描目录放入显式栈：

1. 登记根目录为 `MFTSCAN_ROOT_FRN`。
2. 弹出一个目录，使用 `FindFirstFileExW` / `FindNextFileW` 枚举直属项。
3. 普通子目录分配进程内单调递增 ID，登记父子关系并压栈。
4. 普通文件按直属父目录统计大小。
5. 目录型 reparse point 跳过。

该流程不使用递归函数。

## 大小语义

- `logical_size` 使用 `WIN32_FIND_DATAW` 的文件长度字段。
- `allocated_size` 优先使用 `GetCompressedFileSizeW`。
- 单个文件分配大小查询失败时，降级为逻辑大小，不中断整个扫描。

## 错误处理

- 卷信息查询失败返回 `MFTSCAN_ERROR_VOLUME_QUERY`。
- NTFS 后端非管理员返回 `MFTSCAN_ERROR_NOT_ADMIN`。
- 平台后端局部目录枚举失败时跳过该目录并继续。
- 内存不足立即返回 `MFTSCAN_ERROR_OUT_OF_MEMORY`。

