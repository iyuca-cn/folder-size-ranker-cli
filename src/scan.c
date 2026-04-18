#include <string.h>

#include "model.h"

MftscanError mftscan_scan_volume(MftscanContext *context, const MftscanOptions *options) {
    MftscanFilesystemKind filesystem_kind = MFTSCAN_FILESYSTEM_OTHER;
    MftscanError error_code = MFTSCAN_OK;

    if (context == NULL || options == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    context->volume[0] = options->volume[0];
    context->volume[1] = options->volume[1];
    context->volume[2] = L'\0';

    error_code = mftscan_probe_volume_filesystem(options, &filesystem_kind);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    if (filesystem_kind == MFTSCAN_FILESYSTEM_NTFS) {
        if (!mftscan_is_process_elevated()) {
            return MFTSCAN_ERROR_NOT_ADMIN;
        }
        return mftscan_scan_volume_ntfs(context, options);
    }

    return mftscan_scan_volume_platform(context, options);
}

