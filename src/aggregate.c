#include <stdlib.h>
#include <string.h>

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

static uint64_t mftscan_sort_value(const MftscanLeafResult *item, MftscanSortMode sort_mode) {
    return (sort_mode == MFTSCAN_SORT_ALLOCATED) ? item->allocated_size : item->logical_size;
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

    free(context->directories.items);
    context->directories.items = NULL;
    context->directories.count = 0;
    context->directories.capacity = 0;
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
