#include <windows.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "model.h"

typedef struct MftscanPendingDirectory {
    uint64_t frn;
    wchar_t *path;
} MftscanPendingDirectory;

typedef struct MftscanPendingDirectoryStack {
    MftscanPendingDirectory *items;
    size_t count;
    size_t capacity;
} MftscanPendingDirectoryStack;

static bool mftscan_is_dot_directory(const wchar_t *name) {
    return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0;
}

static uint64_t mftscan_platform_file_size(const WIN32_FIND_DATAW *find_data) {
    return ((uint64_t)find_data->nFileSizeHigh << 32U) | find_data->nFileSizeLow;
}

static uint64_t mftscan_platform_allocated_size(const wchar_t *path, uint64_t logical_size) {
    DWORD high_size = 0;
    DWORD low_size = 0;
    DWORD last_error = ERROR_SUCCESS;

    SetLastError(ERROR_SUCCESS);
    low_size = GetCompressedFileSizeW(path, &high_size);
    last_error = GetLastError();
    if (low_size == INVALID_FILE_SIZE && last_error != ERROR_SUCCESS) {
        return logical_size;
    }

    return ((uint64_t)high_size << 32U) | low_size;
}

static wchar_t *mftscan_join_child_path(const wchar_t *parent_path, const wchar_t *child_name) {
    size_t parent_length = 0;
    size_t child_length = 0;
    size_t total_length = 0;
    bool needs_separator = false;
    wchar_t *joined_path = NULL;

    if (parent_path == NULL || child_name == NULL) {
        return NULL;
    }

    parent_length = wcslen(parent_path);
    child_length = wcslen(child_name);
    needs_separator = parent_length > 0U && parent_path[parent_length - 1U] != L'\\';
    total_length = parent_length + child_length + (needs_separator ? 1U : 0U);

    joined_path = (wchar_t *)calloc(total_length + 1U, sizeof(wchar_t));
    if (joined_path == NULL) {
        return NULL;
    }

    memcpy(joined_path, parent_path, parent_length * sizeof(wchar_t));
    if (needs_separator) {
        joined_path[parent_length++] = L'\\';
    }
    memcpy(joined_path + parent_length, child_name, child_length * sizeof(wchar_t));
    joined_path[total_length] = L'\0';
    return joined_path;
}

static wchar_t *mftscan_build_search_path(const wchar_t *directory_path) {
    return mftscan_join_child_path(directory_path, L"*");
}

static MftscanError mftscan_pending_push(
    MftscanPendingDirectoryStack *stack,
    uint64_t frn,
    wchar_t *path) {
    MftscanPendingDirectory *grown_items = NULL;

    grown_items = (MftscanPendingDirectory *)mftscan_realloc_array(
        stack->items,
        sizeof(MftscanPendingDirectory),
        &stack->capacity,
        stack->count + 1U);
    if (grown_items == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    stack->items = grown_items;
    stack->items[stack->count].frn = frn;
    stack->items[stack->count].path = path;
    stack->count += 1U;
    return MFTSCAN_OK;
}

static bool mftscan_pending_pop(MftscanPendingDirectoryStack *stack, MftscanPendingDirectory *directory) {
    if (stack->count == 0U) {
        return false;
    }

    stack->count -= 1U;
    *directory = stack->items[stack->count];
    stack->items[stack->count].frn = 0ULL;
    stack->items[stack->count].path = NULL;
    return true;
}

static void mftscan_pending_free(MftscanPendingDirectoryStack *stack) {
    size_t index = 0;

    if (stack == NULL) {
        return;
    }

    for (index = 0; index < stack->count; ++index) {
        free(stack->items[index].path);
    }
    free(stack->items);
    memset(stack, 0, sizeof(*stack));
}

static MftscanError mftscan_platform_ingest_directory(
    MftscanContext *context,
    uint64_t frn,
    uint64_t parent_frn,
    const wchar_t *name) {
    MftscanRecordInfo record_info;
    MftscanError error_code = MFTSCAN_OK;

    memset(&record_info, 0, sizeof(record_info));
    record_info.frn = frn;
    record_info.parent_frn = parent_frn;
    record_info.in_use = true;
    record_info.is_directory = true;
    record_info.name_priority = 3U;

    if (name != NULL) {
        record_info.name = mftscan_strdup_w(name);
        if (record_info.name == NULL) {
            return MFTSCAN_ERROR_OUT_OF_MEMORY;
        }
    }

    error_code = mftscan_ingest_record(context, &record_info);
    mftscan_free_record_info(&record_info);
    return error_code;
}

static MftscanError mftscan_platform_ingest_file(
    MftscanContext *context,
    uint64_t frn,
    uint64_t parent_frn,
    uint64_t logical_size,
    uint64_t allocated_size) {
    MftscanRecordInfo record_info;

    memset(&record_info, 0, sizeof(record_info));
    record_info.frn = frn;
    record_info.parent_frn = parent_frn;
    record_info.logical_size = logical_size;
    record_info.allocated_size = allocated_size;
    record_info.in_use = true;
    record_info.is_directory = false;
    return mftscan_ingest_record(context, &record_info);
}

static bool mftscan_next_platform_id(uint64_t *next_frn, uint64_t *frn) {
    if (*next_frn == UINT64_MAX) {
        return false;
    }

    *frn = *next_frn;
    *next_frn += 1ULL;
    return true;
}

MftscanError mftscan_scan_volume_platform(MftscanContext *context, const MftscanOptions *options) {
    MftscanPendingDirectoryStack stack = { 0 };
    MftscanPendingDirectory current_directory = { 0 };
    wchar_t root_path[4] = { 0 };
    uint64_t next_frn = MFTSCAN_ROOT_FRN + 1ULL;
    MftscanError error_code = MFTSCAN_OK;

    if (context == NULL || options == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    context->volume[0] = options->volume[0];
    context->volume[1] = options->volume[1];
    context->volume[2] = L'\0';

    root_path[0] = options->volume[0];
    root_path[1] = L':';
    root_path[2] = L'\\';
    root_path[3] = L'\0';

    error_code = mftscan_platform_ingest_directory(context, MFTSCAN_ROOT_FRN, 0ULL, NULL);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    error_code = mftscan_pending_push(&stack, MFTSCAN_ROOT_FRN, mftscan_strdup_w(root_path));
    if (error_code != MFTSCAN_OK) {
        mftscan_pending_free(&stack);
        return error_code;
    }
    if (stack.items[0].path == NULL) {
        mftscan_pending_free(&stack);
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    while (mftscan_pending_pop(&stack, &current_directory)) {
        WIN32_FIND_DATAW find_data;
        HANDLE find_handle = INVALID_HANDLE_VALUE;
        wchar_t *search_path = mftscan_build_search_path(current_directory.path);

        if (search_path == NULL) {
            free(current_directory.path);
            current_directory.path = NULL;
            error_code = MFTSCAN_ERROR_OUT_OF_MEMORY;
            break;
        }

        memset(&find_data, 0, sizeof(find_data));
        find_handle = FindFirstFileExW(
            search_path,
            FindExInfoBasic,
            &find_data,
            FindExSearchNameMatch,
            NULL,
            FIND_FIRST_EX_LARGE_FETCH);
        free(search_path);

        if (find_handle == INVALID_HANDLE_VALUE) {
            free(current_directory.path);
            current_directory.path = NULL;
            continue;
        }

        do {
            bool is_directory = (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
            bool is_reparse_point = (find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
            uint64_t record_frn = 0;

            if (mftscan_is_dot_directory(find_data.cFileName)) {
                continue;
            }

            if (is_directory) {
                wchar_t *child_path = NULL;

                if (is_reparse_point) {
                    continue;
                }

                if (!mftscan_next_platform_id(&next_frn, &record_frn)) {
                    error_code = MFTSCAN_ERROR_INTERNAL;
                    break;
                }

                child_path = mftscan_join_child_path(current_directory.path, find_data.cFileName);
                if (child_path == NULL) {
                    error_code = MFTSCAN_ERROR_OUT_OF_MEMORY;
                    break;
                }

                error_code = mftscan_platform_ingest_directory(
                    context,
                    record_frn,
                    current_directory.frn,
                    find_data.cFileName);
                if (error_code != MFTSCAN_OK) {
                    free(child_path);
                    break;
                }

                error_code = mftscan_pending_push(&stack, record_frn, child_path);
                if (error_code != MFTSCAN_OK) {
                    free(child_path);
                    break;
                }
            } else {
                wchar_t *child_path = NULL;
                uint64_t logical_size = 0;
                uint64_t allocated_size = 0;

                if (!mftscan_next_platform_id(&next_frn, &record_frn)) {
                    error_code = MFTSCAN_ERROR_INTERNAL;
                    break;
                }

                logical_size = mftscan_platform_file_size(&find_data);
                child_path = mftscan_join_child_path(current_directory.path, find_data.cFileName);
                if (child_path == NULL) {
                    error_code = MFTSCAN_ERROR_OUT_OF_MEMORY;
                    break;
                }

                allocated_size = mftscan_platform_allocated_size(child_path, logical_size);
                free(child_path);

                error_code = mftscan_platform_ingest_file(
                    context,
                    record_frn,
                    current_directory.frn,
                    logical_size,
                    allocated_size);
                if (error_code != MFTSCAN_OK) {
                    break;
                }
            }
        } while (FindNextFileW(find_handle, &find_data));

        FindClose(find_handle);
        free(current_directory.path);
        current_directory.path = NULL;

        if (error_code != MFTSCAN_OK) {
            break;
        }
    }

    free(current_directory.path);
    mftscan_pending_free(&stack);
    return error_code;
}
