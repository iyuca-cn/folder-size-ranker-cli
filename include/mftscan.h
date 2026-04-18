#ifndef MFTSCAN_H
#define MFTSCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum MftscanSortMode {
    MFTSCAN_SORT_LOGICAL = 0,
    MFTSCAN_SORT_ALLOCATED = 1
} MftscanSortMode;

typedef enum MftscanOutputFormat {
    MFTSCAN_FORMAT_TABLE = 0,
    MFTSCAN_FORMAT_JSON = 1
} MftscanOutputFormat;

typedef enum MftscanError {
    MFTSCAN_OK = 0,
    MFTSCAN_ERROR_INVALID_ARGUMENT = 1,
    MFTSCAN_ERROR_NOT_ADMIN = 2,
    MFTSCAN_ERROR_UNSUPPORTED_FILESYSTEM = 3,
    MFTSCAN_ERROR_OPEN_VOLUME = 4,
    MFTSCAN_ERROR_VOLUME_QUERY = 5,
    MFTSCAN_ERROR_MFT_ENUM = 6,
    MFTSCAN_ERROR_MFT_PARSE = 7,
    MFTSCAN_ERROR_OUT_OF_MEMORY = 8,
    MFTSCAN_ERROR_JSON = 9,
    MFTSCAN_ERROR_INTERNAL = 10
} MftscanError;

typedef struct MftscanOptions {
    wchar_t volume[3];
    MftscanSortMode sort_mode;
    MftscanOutputFormat format;
    uint64_t min_size;
    size_t limit;
    bool has_limit;
} MftscanOptions;

typedef struct MftscanLeafResult {
    wchar_t *path;
    uint64_t logical_size;
    uint64_t allocated_size;
} MftscanLeafResult;

typedef struct MftscanScanResult {
    MftscanLeafResult *items;
    size_t count;
    size_t capacity;
} MftscanScanResult;

typedef struct MftscanContext MftscanContext;

bool mftscan_is_process_elevated(void);
MftscanError mftscan_parse_options(int argc, wchar_t **argv, MftscanOptions *options, bool *show_help);
void mftscan_print_help(FILE *stream);
const char *mftscan_error_message(MftscanError error_code);
const char *mftscan_error_detail(void);
void mftscan_context_init(MftscanContext *context);
void mftscan_context_free(MftscanContext *context);
MftscanError mftscan_scan_volume(MftscanContext *context, const MftscanOptions *options);
MftscanError mftscan_build_results(const MftscanContext *context, const MftscanOptions *options, MftscanScanResult *scan_result);
void mftscan_free_results(MftscanScanResult *scan_result);
MftscanError mftscan_output_table(const MftscanOptions *options, const MftscanScanResult *scan_result);
MftscanError mftscan_output_json(const MftscanOptions *options, const MftscanScanResult *scan_result);
char *mftscan_utf8_from_wide(const wchar_t *wide_text);
wchar_t *mftscan_strdup_w(const wchar_t *source_text);

#endif
