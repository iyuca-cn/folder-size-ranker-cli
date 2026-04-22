#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "model.h"

static MftscanError mftscan_ensure_directory(MftscanContext *context, uint64_t frn, MftscanDirNode **directory_node) {
    size_t directory_index = 0;
    MftscanDirNode *grown_items = NULL;
    MftscanError error_code = MFTSCAN_OK;

    if (mftscan_map_get(&context->directory_index, frn, &directory_index)) {
        *directory_node = &context->directories.items[directory_index];
        return MFTSCAN_OK;
    }

    grown_items = (MftscanDirNode *)mftscan_realloc_array(
        context->directories.items,
        sizeof(MftscanDirNode),
        &context->directories.capacity,
        context->directories.count + 1U);
    if (grown_items == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    context->directories.items = grown_items;
    directory_index = context->directories.count++;
    memset(&context->directories.items[directory_index], 0, sizeof(MftscanDirNode));
    context->directories.items[directory_index].frn = frn;

    error_code = mftscan_map_put(&context->directory_index, frn, directory_index);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    *directory_node = &context->directories.items[directory_index];
    return MFTSCAN_OK;
}

static MftscanError mftscan_append_file(MftscanContext *context, MftscanRecordInfo *record_info) {
    size_t file_index = 0;
    MftscanFileNode *grown_items = NULL;

    if (context == NULL || record_info == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    grown_items = (MftscanFileNode *)mftscan_realloc_array(
        context->files.items,
        sizeof(MftscanFileNode),
        &context->files.capacity,
        context->files.count + 1U);
    if (grown_items == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    context->files.items = grown_items;
    file_index = context->files.count++;
    memset(&context->files.items[file_index], 0, sizeof(MftscanFileNode));
    context->files.items[file_index].frn = record_info->frn;
    context->files.items[file_index].parent_frn = record_info->parent_frn;
    context->files.items[file_index].name = record_info->name;
    context->files.items[file_index].logical_size = record_info->logical_size;
    context->files.items[file_index].allocated_size = record_info->allocated_size;
    context->files.items[file_index].metadata_fallback_logical_size = record_info->metadata_fallback_logical_size;
    context->files.items[file_index].metadata_fallback_allocated_size = record_info->metadata_fallback_allocated_size;
    context->files.items[file_index].has_primary_stream_size = record_info->has_primary_stream_size;
    context->files.items[file_index].has_metadata_fallback_size = record_info->has_metadata_fallback_size;
    record_info->name = NULL;
    return MFTSCAN_OK;
}

static bool mftscan_name_equals(const wchar_t *left, const wchar_t *right) {
    if (left == NULL || right == NULL) {
        return false;
    }

    return _wcsicmp(left, right) == 0;
}

static bool mftscan_directory_is_in_metadata_tree(
    MftscanContext *context,
    size_t directory_index,
    uint64_t extend_frn) {
    MftscanDirNode *directory_node = NULL;
    uint64_t current_parent = 0ULL;

    if (context == NULL || directory_index >= context->directories.count) {
        return false;
    }

    directory_node = &context->directories.items[directory_index];
    if (directory_node->in_metadata_tree || directory_node->frn == extend_frn) {
        directory_node->in_metadata_tree = true;
        return true;
    }

    current_parent = directory_node->parent_frn;
    while (current_parent != 0ULL && current_parent != MFTSCAN_ROOT_FRN) {
        size_t parent_index = 0;
        MftscanDirNode *parent_node = NULL;

        if (current_parent == extend_frn) {
            directory_node->in_metadata_tree = true;
            return true;
        }
        if (!mftscan_map_get(&context->directory_index, current_parent, &parent_index)) {
            break;
        }

        parent_node = &context->directories.items[parent_index];
        if (parent_node->in_metadata_tree || parent_node->frn == extend_frn) {
            directory_node->in_metadata_tree = true;
            return true;
        }

        current_parent = parent_node->parent_frn;
    }

    return false;
}

static void mftscan_adjust_directory_totals(
    MftscanContext *context,
    uint64_t parent_frn,
    uint64_t old_logical_size,
    uint64_t new_logical_size,
    uint64_t old_allocated_size,
    uint64_t new_allocated_size) {
    size_t parent_index = 0;
    MftscanDirNode *directory_node = NULL;

    if (context == NULL || parent_frn == 0ULL || parent_frn == MFTSCAN_ROOT_FRN) {
        if (parent_frn == MFTSCAN_ROOT_FRN &&
            mftscan_map_get(&context->directory_index, parent_frn, &parent_index)) {
            directory_node = &context->directories.items[parent_index];
        } else {
            return;
        }
    } else if (mftscan_map_get(&context->directory_index, parent_frn, &parent_index)) {
        directory_node = &context->directories.items[parent_index];
    } else {
        return;
    }

    if (new_logical_size >= old_logical_size) {
        directory_node->logical_size += new_logical_size - old_logical_size;
    } else {
        directory_node->logical_size -= old_logical_size - new_logical_size;
    }

    if (new_allocated_size >= old_allocated_size) {
        directory_node->allocated_size += new_allocated_size - old_allocated_size;
    } else {
        directory_node->allocated_size -= old_allocated_size - new_allocated_size;
    }
}

static bool mftscan_try_query_allocated_size(const wchar_t *path, uint64_t *allocated_size) {
    DWORD high_size = 0;
    DWORD low_size = 0;
    DWORD last_error = ERROR_SUCCESS;

    if (path == NULL || allocated_size == NULL) {
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    low_size = GetCompressedFileSizeW(path, &high_size);
    last_error = GetLastError();
    if (low_size == INVALID_FILE_SIZE && last_error != ERROR_SUCCESS) {
        return false;
    }

    *allocated_size = ((uint64_t)high_size << 32U) | low_size;
    return true;
}

static MftscanError mftscan_build_file_path(
    const MftscanContext *context,
    const MftscanFileNode *file_node,
    wchar_t **path_text) {
    wchar_t *parent_path = NULL;
    wchar_t *built_path = NULL;
    size_t parent_length = 0;
    size_t name_length = 0;
    bool needs_separator = false;
    MftscanError error_code = MFTSCAN_OK;

    if (context == NULL || file_node == NULL || path_text == NULL || file_node->name == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    *path_text = NULL;
    error_code = mftscan_build_path(context, file_node->parent_frn, &parent_path);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    parent_length = wcslen(parent_path);
    name_length = wcslen(file_node->name);
    needs_separator = parent_length > 0U && parent_path[parent_length - 1U] != L'\\';
    built_path = (wchar_t *)calloc(parent_length + name_length + (needs_separator ? 2U : 1U), sizeof(wchar_t));
    if (built_path == NULL) {
        free(parent_path);
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    wcscpy(built_path, parent_path);
    if (needs_separator) {
        wcscat(built_path, L"\\");
    }
    wcscat(built_path, file_node->name);

    free(parent_path);
    *path_text = built_path;
    return MFTSCAN_OK;
}

static uint64_t mftscan_sort_value(const MftscanLeafResult *item, MftscanSortMode sort_mode) {
    return (sort_mode == MFTSCAN_SORT_ALLOCATED) ? item->allocated_size : item->logical_size;
}

static bool mftscan_is_path_separator(wchar_t character) {
    return character == L'\\' || character == L'/';
}

static bool mftscan_path_matches_filter(const wchar_t *path_text, const wchar_t *filter_root) {
    size_t filter_length = 0;

    if (path_text == NULL || filter_root == NULL) {
        return false;
    }

    filter_length = wcslen(filter_root);
    if (_wcsnicmp(path_text, filter_root, filter_length) != 0) {
        return false;
    }

    return path_text[filter_length] == L'\0' || mftscan_is_path_separator(path_text[filter_length]);
}

static int __cdecl mftscan_compare_results(void *context, const void *left_value, const void *right_value) {
    const MftscanOptions *options = (const MftscanOptions *)context;
    const MftscanLeafResult *left_item = (const MftscanLeafResult *)left_value;
    const MftscanLeafResult *right_item = (const MftscanLeafResult *)right_value;
    uint64_t left_sort = mftscan_sort_value(left_item, options->sort_mode);
    uint64_t right_sort = mftscan_sort_value(right_item, options->sort_mode);

    if (left_sort < right_sort) {
        return 1;
    }
    if (left_sort > right_sort) {
        return -1;
    }
    if (left_item->path == NULL && right_item->path != NULL) {
        return 1;
    }
    if (left_item->path != NULL && right_item->path == NULL) {
        return -1;
    }
    if (left_item->path == NULL || right_item->path == NULL) {
        return 0;
    }
    return _wcsicmp(left_item->path, right_item->path);
}

void mftscan_context_init(MftscanContext *context) {
    if (context != NULL) {
        memset(context, 0, sizeof(*context));
    }
}

void mftscan_context_free(MftscanContext *context) {
    size_t index = 0;

    if (context == NULL) {
        return;
    }

    for (index = 0; index < context->directories.count; ++index) {
        free(context->directories.items[index].name);
    }
    for (index = 0; index < context->files.count; ++index) {
        free(context->files.items[index].name);
    }

    free(context->directories.items);
    free(context->files.items);
    context->directories.items = NULL;
    context->directories.count = 0;
    context->directories.capacity = 0;
    context->files.items = NULL;
    context->files.count = 0;
    context->files.capacity = 0;
    mftscan_map_free(&context->directory_index);
    mftscan_map_free(&context->seen_files);
}

MftscanError mftscan_ingest_record(MftscanContext *context, MftscanRecordInfo *record_info) {
    MftscanDirNode *directory_node = NULL;
    MftscanError error_code = MFTSCAN_OK;

    if (context == NULL || record_info == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (!record_info->in_use) {
        return MFTSCAN_OK;
    }

    if (record_info->is_directory) {
        error_code = mftscan_ensure_directory(context, record_info->frn, &directory_node);
        if (error_code != MFTSCAN_OK) {
            return error_code;
        }

        directory_node->parent_frn = record_info->parent_frn;
        directory_node->metadata_ready = true;

        if (record_info->name != NULL && record_info->name_priority >= directory_node->name_priority) {
            free(directory_node->name);
            directory_node->name = record_info->name;
            directory_node->name_priority = record_info->name_priority;
            record_info->name = NULL;
        }

        if (record_info->has_directory_metadata_size) {
            directory_node->metadata_allocated_size = record_info->directory_metadata_allocated_size;
        }

        if (record_info->parent_frn != 0ULL && record_info->parent_frn != record_info->frn) {
            MftscanDirNode *parent_node = NULL;
            error_code = mftscan_ensure_directory(context, record_info->parent_frn, &parent_node);
            if (error_code != MFTSCAN_OK) {
                return error_code;
            }
            parent_node->has_child_dir = true;
        }

        return MFTSCAN_OK;
    }

    if (mftscan_set_contains(&context->seen_files, record_info->frn)) {
        return MFTSCAN_OK;
    }

    error_code = mftscan_set_add(&context->seen_files, record_info->frn);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    if (record_info->parent_frn != 0ULL && record_info->parent_frn != record_info->frn) {
        error_code = mftscan_ensure_directory(context, record_info->parent_frn, &directory_node);
        if (error_code != MFTSCAN_OK) {
            return error_code;
        }
        directory_node->logical_size += record_info->logical_size;
        directory_node->allocated_size += record_info->allocated_size;
    }

    if (record_info->name == NULL || record_info->name[0] == L'\0') {
        return MFTSCAN_OK;
    }

    return mftscan_append_file(context, record_info);
}

MftscanError mftscan_finalize_metadata_tree(MftscanContext *context) {
    size_t extend_index = SIZE_MAX;
    size_t index = 0;

    if (context == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    for (index = 0; index < context->directories.count; ++index) {
        const MftscanDirNode *directory_node = &context->directories.items[index];

        if (directory_node->parent_frn == MFTSCAN_ROOT_FRN &&
            mftscan_name_equals(directory_node->name, L"$Extend")) {
            extend_index = index;
            break;
        }
    }

    if (extend_index == SIZE_MAX) {
        return MFTSCAN_OK;
    }

    context->directories.items[extend_index].in_metadata_tree = true;
    for (index = 0; index < context->directories.count; ++index) {
        MftscanDirNode *directory_node = &context->directories.items[index];

        if (mftscan_directory_is_in_metadata_tree(context, index, context->directories.items[extend_index].frn) &&
            directory_node->metadata_allocated_size > 0ULL) {
            directory_node->allocated_size += directory_node->metadata_allocated_size;
        }
    }

    for (index = 0; index < context->files.count; ++index) {
        MftscanFileNode *file_node = &context->files.items[index];
        size_t parent_index = 0;

        if (!mftscan_map_get(&context->directory_index, file_node->parent_frn, &parent_index)) {
            continue;
        }

        if (mftscan_directory_is_in_metadata_tree(context, parent_index, context->directories.items[extend_index].frn)) {
            uint64_t new_logical_size = file_node->logical_size;
            uint64_t new_allocated_size = file_node->allocated_size;

            file_node->in_metadata_tree = true;
            if (file_node->has_metadata_fallback_size) {
                if (!file_node->has_primary_stream_size && new_logical_size == 0ULL) {
                    new_logical_size = file_node->metadata_fallback_logical_size;
                }
                new_allocated_size += file_node->metadata_fallback_allocated_size;
                mftscan_adjust_directory_totals(
                    context,
                    file_node->parent_frn,
                    file_node->logical_size,
                    new_logical_size,
                    file_node->allocated_size,
                    new_allocated_size);
                file_node->logical_size = new_logical_size;
                file_node->allocated_size = new_allocated_size;
            }
        }
    }

    return MFTSCAN_OK;
}

MftscanError mftscan_backfill_zero_allocated_files(MftscanContext *context) {
    size_t index = 0;

    if (context == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (context->bytes_per_cluster == 0U) {
        return MFTSCAN_OK;
    }

    for (index = 0; index < context->files.count; ++index) {
        MftscanFileNode *file_node = &context->files.items[index];
        wchar_t *path_text = NULL;
        uint64_t allocated_size = 0ULL;
        MftscanError error_code = MFTSCAN_OK;

        if (file_node->name == NULL || file_node->name[0] == L'\0') {
            continue;
        }
        if (file_node->allocated_size != 0ULL || file_node->logical_size < context->bytes_per_cluster) {
            continue;
        }

        error_code = mftscan_build_file_path(context, file_node, &path_text);
        if (error_code != MFTSCAN_OK) {
            return error_code;
        }

        if (mftscan_try_query_allocated_size(path_text, &allocated_size) &&
            allocated_size != file_node->allocated_size) {
            mftscan_adjust_directory_totals(
                context,
                file_node->parent_frn,
                file_node->logical_size,
                file_node->logical_size,
                file_node->allocated_size,
                allocated_size);
            file_node->allocated_size = allocated_size;
        }

        free(path_text);
    }

    return MFTSCAN_OK;
}

MftscanError mftscan_build_results(const MftscanContext *context, const MftscanOptions *options, MftscanScanResult *scan_result) {
    size_t index = 0;

    if (context == NULL || options == NULL || scan_result == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    memset(scan_result, 0, sizeof(*scan_result));

    for (index = 0; index < context->directories.count; ++index) {
        const MftscanDirNode *directory_node = &context->directories.items[index];
        uint64_t filter_value = 0;
        wchar_t *path_text = NULL;
        MftscanLeafResult *grown_items = NULL;
        MftscanError error_code = MFTSCAN_OK;

        if (directory_node->has_child_dir) {
            continue;
        }
        if (!directory_node->metadata_ready && directory_node->frn != MFTSCAN_ROOT_FRN) {
            continue;
        }

        filter_value = (options->sort_mode == MFTSCAN_SORT_ALLOCATED)
            ? directory_node->allocated_size
            : directory_node->logical_size;
        if (filter_value < options->min_size) {
            continue;
        }

        error_code = mftscan_build_path(context, directory_node->frn, &path_text);
        if (error_code != MFTSCAN_OK) {
            return error_code;
        }

        if (options->filter_by_location && !mftscan_path_matches_filter(path_text, options->filter_root)) {
            free(path_text);
            continue;
        }

        grown_items = (MftscanLeafResult *)mftscan_realloc_array(
            scan_result->items,
            sizeof(MftscanLeafResult),
            &scan_result->capacity,
            scan_result->count + 1U);
        if (grown_items == NULL) {
            free(path_text);
            return MFTSCAN_ERROR_OUT_OF_MEMORY;
        }

        scan_result->items = grown_items;
        scan_result->items[scan_result->count].path = path_text;
        scan_result->items[scan_result->count].logical_size = directory_node->logical_size;
        scan_result->items[scan_result->count].allocated_size = directory_node->allocated_size;
        scan_result->count += 1U;
    }

    if (scan_result->count > 1U) {
        qsort_s(scan_result->items, scan_result->count, sizeof(MftscanLeafResult), mftscan_compare_results, (void *)options);
    }

    if (options->has_limit && scan_result->count > options->limit) {
        for (index = options->limit; index < scan_result->count; ++index) {
            free(scan_result->items[index].path);
            scan_result->items[index].path = NULL;
        }
        scan_result->count = options->limit;
    }

    return MFTSCAN_OK;
}

void mftscan_free_results(MftscanScanResult *scan_result) {
    size_t index = 0;

    if (scan_result == NULL) {
        return;
    }

    for (index = 0; index < scan_result->count; ++index) {
        free(scan_result->items[index].path);
    }

    free(scan_result->items);
    memset(scan_result, 0, sizeof(*scan_result));
}
