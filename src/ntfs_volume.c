#include <windows.h>

#include <string.h>

#include "model.h"

static void mftscan_build_root_path(const wchar_t *volume_text, wchar_t *root_path, size_t root_path_count) {
    if (root_path_count < 4) {
        return;
    }

    root_path[0] = volume_text[0];
    root_path[1] = L':';
    root_path[2] = L'\\';
    root_path[3] = L'\0';
}

static void mftscan_build_device_path(const wchar_t *volume_text, wchar_t *device_path, size_t device_path_count) {
    if (device_path_count < 7) {
        return;
    }

    device_path[0] = L'\\';
    device_path[1] = L'\\';
    device_path[2] = L'.';
    device_path[3] = L'\\';
    device_path[4] = volume_text[0];
    device_path[5] = L':';
    device_path[6] = L'\0';
}

MftscanError mftscan_probe_volume_filesystem(const MftscanOptions *options, MftscanFilesystemKind *filesystem_kind) {
    wchar_t root_path[4] = { 0 };
    wchar_t filesystem_name[MAX_PATH] = { 0 };

    if (options == NULL || filesystem_kind == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    mftscan_build_root_path(options->volume, root_path, ARRAYSIZE(root_path));
    if (!GetVolumeInformationW(root_path, NULL, 0, NULL, NULL, NULL, filesystem_name, ARRAYSIZE(filesystem_name))) {
        return MFTSCAN_ERROR_VOLUME_QUERY;
    }

    *filesystem_kind = (_wcsicmp(filesystem_name, L"NTFS") == 0)
        ? MFTSCAN_FILESYSTEM_NTFS
        : MFTSCAN_FILESYSTEM_OTHER;
    return MFTSCAN_OK;
}

MftscanError mftscan_open_volume(const MftscanOptions *options, MftscanVolumeHandle *volume_handle) {
    wchar_t root_path[4] = { 0 };
    wchar_t device_path[7] = { 0 };
    wchar_t filesystem_name[MAX_PATH] = { 0 };
    DWORD bytes_returned = 0;
    NTFS_VOLUME_DATA_BUFFER volume_data = { 0 };
    ULONGLONG record_count = 0;

    if (options == NULL || volume_handle == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    memset(volume_handle, 0, sizeof(*volume_handle));
    volume_handle->handle = INVALID_HANDLE_VALUE;
    volume_handle->volume[0] = options->volume[0];
    volume_handle->volume[1] = options->volume[1];
    volume_handle->volume[2] = L'\0';

    mftscan_build_root_path(options->volume, root_path, ARRAYSIZE(root_path));
    mftscan_build_device_path(options->volume, device_path, ARRAYSIZE(device_path));

    if (!GetVolumeInformationW(root_path, NULL, 0, NULL, NULL, NULL, filesystem_name, ARRAYSIZE(filesystem_name))) {
        return MFTSCAN_ERROR_VOLUME_QUERY;
    }

    if (_wcsicmp(filesystem_name, L"NTFS") != 0) {
        return MFTSCAN_ERROR_UNSUPPORTED_FILESYSTEM;
    }

    volume_handle->handle = CreateFileW(
        device_path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (volume_handle->handle == INVALID_HANDLE_VALUE) {
        return MFTSCAN_ERROR_OPEN_VOLUME;
    }

    if (!DeviceIoControl(
            volume_handle->handle,
            FSCTL_GET_NTFS_VOLUME_DATA,
            NULL,
            0,
            &volume_data,
            sizeof(volume_data),
            &bytes_returned,
            NULL)) {
        mftscan_close_volume(volume_handle);
        return MFTSCAN_ERROR_VOLUME_QUERY;
    }

    volume_handle->bytes_per_sector = volume_data.BytesPerSector;
    volume_handle->bytes_per_file_record = volume_data.BytesPerFileRecordSegment;
    record_count = (ULONGLONG)(volume_data.MftValidDataLength.QuadPart / volume_data.BytesPerFileRecordSegment);
    volume_handle->highest_record_number = (record_count > 0ULL) ? (uint64_t)(record_count - 1ULL) : 0ULL;
    return MFTSCAN_OK;
}

void mftscan_close_volume(MftscanVolumeHandle *volume_handle) {
    if (volume_handle != NULL && volume_handle->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(volume_handle->handle);
        volume_handle->handle = INVALID_HANDLE_VALUE;
    }
}
