#ifndef MFTSCAN_MODEL_H
#define MFTSCAN_MODEL_H

#include <windows.h>
#include <winioctl.h>

#include "../include/mftscan.h"

#define MFTSCAN_FRN_MASK 0x0000FFFFFFFFFFFFULL
#define MFTSCAN_ROOT_FRN 5ULL

typedef enum MftscanFilesystemKind {
    MFTSCAN_FILESYSTEM_NTFS = 0,
    MFTSCAN_FILESYSTEM_OTHER = 1
} MftscanFilesystemKind;

typedef struct MftscanDirNode {
    uint64_t frn;
    uint64_t parent_frn;
    wchar_t *name;
    uint64_t logical_size;
    uint64_t allocated_size;
    uint8_t name_priority;
    bool has_child_dir;
    bool metadata_ready;
} MftscanDirNode;

typedef struct MftscanDirVector {
    MftscanDirNode *items;
    size_t count;
    size_t capacity;
} MftscanDirVector;

typedef struct MftscanFileNode {
    uint64_t frn;
    uint64_t parent_frn;
    wchar_t *name;
    uint64_t logical_size;
    uint64_t allocated_size;
} MftscanFileNode;

typedef struct MftscanFileVector {
    MftscanFileNode *items;
    size_t count;
    size_t capacity;
} MftscanFileVector;

typedef struct MftscanUint64MapEntry {
    uint64_t key;
    size_t value;
    bool occupied;
} MftscanUint64MapEntry;

typedef struct MftscanUint64Map {
    MftscanUint64MapEntry *entries;
    size_t count;
    size_t capacity;
} MftscanUint64Map;

typedef struct MftscanRecordInfo {
    uint64_t frn;
    uint64_t parent_frn;
    uint64_t logical_size;
    uint64_t allocated_size;
    wchar_t *name;
    uint8_t name_priority;
    bool has_data_size;
    bool in_use;
    bool is_directory;
} MftscanRecordInfo;

typedef struct MftscanVolumeHandle {
    HANDLE handle;
    wchar_t volume[3];
    DWORD bytes_per_sector;
    DWORD bytes_per_cluster;
    DWORD bytes_per_file_record;
    uint64_t highest_record_number;
} MftscanVolumeHandle;

struct MftscanContext {
    wchar_t volume[3];
    MftscanDirVector directories;
    MftscanFileVector files;
    MftscanUint64Map directory_index;
    MftscanUint64Map seen_files;
};

void *mftscan_realloc_array(void *buffer, size_t item_size, size_t *capacity, size_t required_count);
bool mftscan_map_get(const MftscanUint64Map *map, uint64_t key, size_t *value);
MftscanError mftscan_map_put(MftscanUint64Map *map, uint64_t key, size_t value);
bool mftscan_set_contains(const MftscanUint64Map *set_map, uint64_t key);
MftscanError mftscan_set_add(MftscanUint64Map *set_map, uint64_t key);
void mftscan_map_free(MftscanUint64Map *map);
void mftscan_free_record_info(MftscanRecordInfo *record_info);
void mftscan_set_error_detail(const char *format_text, ...);
MftscanError mftscan_open_volume(const MftscanOptions *options, MftscanVolumeHandle *volume_handle);
void mftscan_close_volume(MftscanVolumeHandle *volume_handle);
MftscanError mftscan_probe_volume_filesystem(const MftscanOptions *options, MftscanFilesystemKind *filesystem_kind);
MftscanError mftscan_read_file_record(
    const MftscanVolumeHandle *volume_handle,
    uint64_t request_record,
    NTFS_FILE_RECORD_OUTPUT_BUFFER *output_buffer,
    size_t output_buffer_size,
    uint64_t *actual_record,
    bool *reached_end);
MftscanError mftscan_read_volume_bytes(
    const MftscanVolumeHandle *volume_handle,
    uint64_t byte_offset,
    uint8_t *buffer,
    size_t buffer_size);
MftscanError mftscan_scan_volume_ntfs(MftscanContext *context, const MftscanOptions *options);
MftscanError mftscan_scan_volume_platform(MftscanContext *context, const MftscanOptions *options);
MftscanError mftscan_parse_file_record(
    const MftscanVolumeHandle *volume_handle,
    uint8_t *record_buffer,
    size_t record_length,
    uint64_t record_number,
    MftscanRecordInfo *record_info);
MftscanError mftscan_ingest_record(MftscanContext *context, MftscanRecordInfo *record_info);
MftscanError mftscan_build_path(const MftscanContext *context, uint64_t directory_frn, wchar_t **path_text);

#endif
