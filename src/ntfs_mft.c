#include <windows.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "model.h"

MftscanError mftscan_read_file_record(
    const MftscanVolumeHandle *volume_handle,
    uint64_t request_record,
    NTFS_FILE_RECORD_OUTPUT_BUFFER *output_buffer,
    size_t output_buffer_size,
    uint64_t *actual_record,
    bool *reached_end) {
    NTFS_FILE_RECORD_INPUT_BUFFER input_buffer = { 0 };
    DWORD bytes_returned = 0;
    size_t minimum_size = 0;

    if (volume_handle == NULL ||
        output_buffer == NULL ||
        actual_record == NULL ||
        reached_end == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    minimum_size = offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) + volume_handle->bytes_per_file_record;
    if (output_buffer_size < minimum_size) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    *actual_record = 0ULL;
    *reached_end = false;
    memset(output_buffer, 0, output_buffer_size);
    input_buffer.FileReferenceNumber.QuadPart = request_record;

    if (!DeviceIoControl(
            volume_handle->handle,
            FSCTL_GET_NTFS_FILE_RECORD,
            &input_buffer,
            sizeof(input_buffer),
            output_buffer,
            (DWORD)output_buffer_size,
            &bytes_returned,
            NULL)) {
        DWORD last_error = GetLastError();
        if (last_error == ERROR_HANDLE_EOF || last_error == ERROR_NO_MORE_FILES) {
            *reached_end = true;
            return MFTSCAN_OK;
        }

        return MFTSCAN_ERROR_MFT_ENUM;
    }

    *actual_record = (uint64_t)output_buffer->FileReferenceNumber.QuadPart & MFTSCAN_FRN_MASK;
    return MFTSCAN_OK;
}

MftscanError mftscan_read_volume_bytes(
    const MftscanVolumeHandle *volume_handle,
    uint64_t byte_offset,
    uint8_t *buffer,
    size_t buffer_size) {
    LARGE_INTEGER position;
    size_t total_read = 0;

    if (volume_handle == NULL || (buffer == NULL && buffer_size > 0U)) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (buffer_size == 0U) {
        return MFTSCAN_OK;
    }

    position.QuadPart = (LONGLONG)byte_offset;
    if (!SetFilePointerEx(volume_handle->handle, position, NULL, FILE_BEGIN)) {
        return MFTSCAN_ERROR_MFT_ENUM;
    }

    while (total_read < buffer_size) {
        size_t remaining = buffer_size - total_read;
        DWORD chunk_size = (remaining > (size_t)MAXDWORD) ? MAXDWORD : (DWORD)remaining;
        DWORD bytes_read = 0;

        if (!ReadFile(volume_handle->handle, buffer + total_read, chunk_size, &bytes_read, NULL)) {
            return MFTSCAN_ERROR_MFT_ENUM;
        }
        if (bytes_read != chunk_size) {
            return MFTSCAN_ERROR_MFT_ENUM;
        }

        total_read += bytes_read;
    }

    return MFTSCAN_OK;
}

MftscanError mftscan_scan_volume_ntfs(MftscanContext *context, const MftscanOptions *options) {
    MftscanVolumeHandle volume_handle;
    NTFS_FILE_RECORD_OUTPUT_BUFFER *output_buffer = NULL;
    size_t output_buffer_size = 0;
    uint64_t request_record = 0;
    MftscanError error_code = MFTSCAN_OK;

    if (context == NULL || options == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    context->volume[0] = options->volume[0];
    context->volume[1] = options->volume[1];
    context->volume[2] = L'\0';
    context->bytes_per_cluster = 0U;

    error_code = mftscan_open_volume(options, &volume_handle);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    context->bytes_per_cluster = volume_handle.bytes_per_cluster;

    output_buffer_size = offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) + volume_handle.bytes_per_file_record;
    output_buffer = (NTFS_FILE_RECORD_OUTPUT_BUFFER *)malloc(output_buffer_size);
    if (output_buffer == NULL) {
        mftscan_close_volume(&volume_handle);
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    request_record = volume_handle.highest_record_number;
    for (;;) {
        MftscanRecordInfo record_info;
        uint64_t actual_record = 0;
        bool reached_end = false;
        size_t file_record_length = 0;

        error_code = mftscan_read_file_record(
            &volume_handle,
            request_record,
            output_buffer,
            output_buffer_size,
            &actual_record,
            &reached_end);
        if (error_code != MFTSCAN_OK) {
            goto cleanup;
        }
        if (reached_end) {
            break;
        }

        if (actual_record > request_record) {
            error_code = MFTSCAN_ERROR_MFT_ENUM;
            goto cleanup;
        }

        file_record_length = volume_handle.bytes_per_file_record;
        error_code = mftscan_parse_file_record(
            &volume_handle,
            output_buffer->FileRecordBuffer,
            file_record_length,
            actual_record,
            &record_info);
        if (error_code != MFTSCAN_OK) {
            if (mftscan_error_detail()[0] == '\0') {
                mftscan_set_error_detail("解析 FRN %llu 失败", (unsigned long long)actual_record);
            }
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
        if (actual_record == 0ULL) {
            break;
        }
        request_record = actual_record - 1ULL;
    }

cleanup:
    free(output_buffer);
    mftscan_close_volume(&volume_handle);
    return error_code;
}
