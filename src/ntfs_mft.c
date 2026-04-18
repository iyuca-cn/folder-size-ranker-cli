#include <windows.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "model.h"

MftscanError mftscan_scan_volume_ntfs(MftscanContext *context, const MftscanOptions *options) {
    MftscanVolumeHandle volume_handle;
    NTFS_FILE_RECORD_INPUT_BUFFER input_buffer = { 0 };
    NTFS_FILE_RECORD_OUTPUT_BUFFER *output_buffer = NULL;
    DWORD bytes_returned = 0;
    size_t output_buffer_size = 0;
    uint64_t request_record = 0;
    MftscanError error_code = MFTSCAN_OK;

    if (context == NULL || options == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    context->volume[0] = options->volume[0];
    context->volume[1] = options->volume[1];
    context->volume[2] = L'\0';

    error_code = mftscan_open_volume(options, &volume_handle);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    output_buffer_size = offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) + volume_handle.bytes_per_file_record;
    output_buffer = (NTFS_FILE_RECORD_OUTPUT_BUFFER *)malloc(output_buffer_size);
    if (output_buffer == NULL) {
        mftscan_close_volume(&volume_handle);
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    request_record = volume_handle.highest_record_number;
    while (request_record >= 1ULL) {
        MftscanRecordInfo record_info;
        uint64_t actual_record = 0;
        size_t file_record_length = 0;

        memset(output_buffer, 0, output_buffer_size);
        memset(&input_buffer, 0, sizeof(input_buffer));
        input_buffer.FileReferenceNumber.QuadPart = request_record;

        if (!DeviceIoControl(
                volume_handle.handle,
                FSCTL_GET_NTFS_FILE_RECORD,
                &input_buffer,
                sizeof(input_buffer),
                output_buffer,
                (DWORD)output_buffer_size,
                &bytes_returned,
                NULL)) {
            DWORD last_error = GetLastError();
            if (last_error == ERROR_HANDLE_EOF || last_error == ERROR_NO_MORE_FILES) {
                break;
            }

            error_code = MFTSCAN_ERROR_MFT_ENUM;
            goto cleanup;
        }

        actual_record = (uint64_t)output_buffer->FileReferenceNumber.QuadPart & MFTSCAN_FRN_MASK;
        if (actual_record > request_record) {
            error_code = MFTSCAN_ERROR_MFT_ENUM;
            goto cleanup;
        }

        file_record_length = volume_handle.bytes_per_file_record;
        error_code = mftscan_parse_file_record(
            output_buffer->FileRecordBuffer,
            file_record_length,
            volume_handle.bytes_per_sector,
            actual_record,
            &record_info);
        if (error_code != MFTSCAN_OK) {
            goto cleanup;
        }

        if (record_info.in_use) {
            error_code = mftscan_ingest_record(context, &record_info);
            if (error_code != MFTSCAN_OK) {
                mftscan_free_record_info(&record_info);
                goto cleanup;
            }
        }

        mftscan_free_record_info(&record_info);
        if (actual_record <= 1ULL) {
            break;
        }
        request_record = actual_record - 1ULL;
    }

cleanup:
    free(output_buffer);
    mftscan_close_volume(&volume_handle);
    return error_code;
}
