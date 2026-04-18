#include <windows.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "model.h"

MftscanError mftscan_output_table(const MftscanScanResult *scan_result) {
    size_t index = 0;

    if (scan_result == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    (void)SetConsoleOutputCP(CP_UTF8);
    printf("%20s %20s %s\n", "LogicalBytes", "AllocatedBytes", "Path");

    for (index = 0; index < scan_result->count; ++index) {
        char *utf8_path = mftscan_utf8_from_wide(scan_result->items[index].path);
        if (utf8_path == NULL) {
            return MFTSCAN_ERROR_OUT_OF_MEMORY;
        }

        printf(
            "%20" PRIu64 " %20" PRIu64 " %s\n",
            scan_result->items[index].logical_size,
            scan_result->items[index].allocated_size,
            utf8_path);
        free(utf8_path);
    }

    return MFTSCAN_OK;
}
