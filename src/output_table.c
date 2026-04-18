#include <windows.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "model.h"

static uint64_t mftscan_output_value(const MftscanOptions *options, const MftscanLeafResult *item) {
    return (options->sort_mode == MFTSCAN_SORT_ALLOCATED) ? item->allocated_size : item->logical_size;
}

MftscanError mftscan_output_table(const MftscanOptions *options, const MftscanScanResult *scan_result) {
    size_t index = 0;

    if (options == NULL || scan_result == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    (void)SetConsoleOutputCP(CP_UTF8);
    printf("%20s %s\n", "Bytes", "Path");

    for (index = 0; index < scan_result->count; ++index) {
        char *utf8_path = mftscan_utf8_from_wide(scan_result->items[index].path);
        if (utf8_path == NULL) {
            return MFTSCAN_ERROR_OUT_OF_MEMORY;
        }

        printf(
            "%20" PRIu64 " %s\n",
            mftscan_output_value(options, &scan_result->items[index]),
            utf8_path);
        free(utf8_path);
    }

    return MFTSCAN_OK;
}
