#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "model.h"
#include "../third_party/yyjson/yyjson.h"

#define MFTSCAN_NO_INDEX ((size_t)-1)

typedef struct MftscanAllTree {
    uint64_t *logical_totals;
    uint64_t *allocated_totals;
    size_t *parent_indices;
    size_t *child_offsets;
    size_t *child_indices;
    size_t count;
    size_t child_count;
    size_t root_index;
} MftscanAllTree;

typedef struct MftscanAllSortContext {
    const MftscanContext *context;
    const MftscanOptions *options;
    const MftscanAllTree *tree;
} MftscanAllSortContext;

static uint64_t mftscan_all_sort_value(const MftscanOptions *options, const MftscanAllTree *tree, size_t index) {
    return (options->sort_mode == MFTSCAN_SORT_ALLOCATED)
        ? tree->allocated_totals[index]
        : tree->logical_totals[index];
}

static bool mftscan_all_directory_ready(const MftscanDirNode *directory_node) {
    return directory_node != NULL &&
        (directory_node->metadata_ready || directory_node->frn == MFTSCAN_ROOT_FRN);
}

static int __cdecl mftscan_compare_all_children(void *context, const void *left_value, const void *right_value) {
    const MftscanAllSortContext *sort_context = (const MftscanAllSortContext *)context;
    size_t left_index = *(const size_t *)left_value;
    size_t right_index = *(const size_t *)right_value;
    const MftscanDirNode *left_node = &sort_context->context->directories.items[left_index];
    const MftscanDirNode *right_node = &sort_context->context->directories.items[right_index];
    uint64_t left_sort = mftscan_all_sort_value(sort_context->options, sort_context->tree, left_index);
    uint64_t right_sort = mftscan_all_sort_value(sort_context->options, sort_context->tree, right_index);

    if (left_sort < right_sort) {
        return 1;
    }
    if (left_sort > right_sort) {
        return -1;
    }
    if (left_node->name == NULL && right_node->name != NULL) {
        return 1;
    }
    if (left_node->name != NULL && right_node->name == NULL) {
        return -1;
    }
    if (left_node->name != NULL && right_node->name != NULL) {
        int name_compare = _wcsicmp(left_node->name, right_node->name);
        if (name_compare != 0) {
            return name_compare;
        }
    }
    if (left_node->frn < right_node->frn) {
        return -1;
    }
    if (left_node->frn > right_node->frn) {
        return 1;
    }
    return 0;
}

static void mftscan_all_tree_free(MftscanAllTree *tree) {
    if (tree == NULL) {
        return;
    }

    free(tree->logical_totals);
    free(tree->allocated_totals);
    free(tree->parent_indices);
    free(tree->child_offsets);
    free(tree->child_indices);
    memset(tree, 0, sizeof(*tree));
    tree->root_index = MFTSCAN_NO_INDEX;
}

static MftscanError mftscan_all_find_root_index(
    const MftscanContext *context,
    const MftscanOptions *options,
    size_t *root_index) {
    size_t index = 0;

    if (context == NULL || options == NULL || root_index == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    *root_index = MFTSCAN_NO_INDEX;
    if (!options->filter_by_location) {
        return mftscan_map_get(&context->directory_index, MFTSCAN_ROOT_FRN, root_index)
            ? MFTSCAN_OK
            : MFTSCAN_ERROR_INTERNAL;
    }

    for (index = 0; index < context->directories.count; ++index) {
        const MftscanDirNode *directory_node = &context->directories.items[index];
        wchar_t *path_text = NULL;
        MftscanError error_code = MFTSCAN_OK;

        if (!mftscan_all_directory_ready(directory_node)) {
            continue;
        }

        error_code = mftscan_build_path(context, directory_node->frn, &path_text);
        if (error_code != MFTSCAN_OK) {
            return error_code;
        }

        if (path_text != NULL && _wcsicmp(path_text, options->filter_root) == 0) {
            free(path_text);
            *root_index = index;
            return MFTSCAN_OK;
        }

        free(path_text);
    }

    return MFTSCAN_ERROR_INTERNAL;
}

static MftscanError mftscan_all_tree_build(
    const MftscanContext *context,
    const MftscanOptions *options,
    MftscanAllTree *tree) {
    size_t *child_counts = NULL;
    size_t *write_offsets = NULL;
    size_t index = 0;
    size_t running_offset = 0;
    MftscanError error_code = MFTSCAN_OK;

    if (context == NULL || options == NULL || tree == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    memset(tree, 0, sizeof(*tree));
    tree->root_index = MFTSCAN_NO_INDEX;
    tree->count = context->directories.count;
    if (tree->count == 0U) {
        return MFTSCAN_ERROR_INTERNAL;
    }

    tree->logical_totals = (uint64_t *)calloc(tree->count, sizeof(uint64_t));
    tree->allocated_totals = (uint64_t *)calloc(tree->count, sizeof(uint64_t));
    tree->parent_indices = (size_t *)calloc(tree->count, sizeof(size_t));
    tree->child_offsets = (size_t *)calloc(tree->count + 1U, sizeof(size_t));
    child_counts = (size_t *)calloc(tree->count, sizeof(size_t));
    write_offsets = (size_t *)calloc(tree->count, sizeof(size_t));
    if (tree->logical_totals == NULL ||
        tree->allocated_totals == NULL ||
        tree->parent_indices == NULL ||
        tree->child_offsets == NULL ||
        child_counts == NULL ||
        write_offsets == NULL) {
        error_code = MFTSCAN_ERROR_OUT_OF_MEMORY;
        goto cleanup;
    }

    for (index = 0; index < tree->count; ++index) {
        const MftscanDirNode *directory_node = &context->directories.items[index];
        size_t parent_index = MFTSCAN_NO_INDEX;

        tree->logical_totals[index] = directory_node->logical_size;
        tree->allocated_totals[index] = directory_node->allocated_size;
        tree->parent_indices[index] = MFTSCAN_NO_INDEX;

        if (directory_node->parent_frn != 0ULL &&
            directory_node->parent_frn != directory_node->frn &&
            mftscan_map_get(&context->directory_index, directory_node->parent_frn, &parent_index)) {
            tree->parent_indices[index] = parent_index;
            child_counts[parent_index] += 1U;
        }
    }

    for (index = 0; index < tree->count; ++index) {
        tree->child_offsets[index] = running_offset;
        running_offset += child_counts[index];
    }
    tree->child_offsets[tree->count] = running_offset;
    tree->child_count = running_offset;

    if (tree->child_count > 0U) {
        tree->child_indices = (size_t *)calloc(tree->child_count, sizeof(size_t));
        if (tree->child_indices == NULL) {
            error_code = MFTSCAN_ERROR_OUT_OF_MEMORY;
            goto cleanup;
        }

        memcpy(write_offsets, tree->child_offsets, tree->count * sizeof(size_t));
        for (index = 0; index < tree->count; ++index) {
            size_t parent_index = tree->parent_indices[index];
            if (parent_index != MFTSCAN_NO_INDEX) {
                tree->child_indices[write_offsets[parent_index]++] = index;
            }
        }
    }

    for (index = 0; index < tree->count; ++index) {
        uint64_t logical_size = context->directories.items[index].logical_size;
        uint64_t allocated_size = context->directories.items[index].allocated_size;
        size_t current_index = index;
        size_t guard_count = 0;

        if (logical_size == 0ULL && allocated_size == 0ULL) {
            continue;
        }

        while (tree->parent_indices[current_index] != MFTSCAN_NO_INDEX) {
            size_t parent_index = tree->parent_indices[current_index];

            tree->logical_totals[parent_index] += logical_size;
            tree->allocated_totals[parent_index] += allocated_size;
            current_index = parent_index;
            guard_count += 1U;
            if (guard_count > tree->count) {
                error_code = MFTSCAN_ERROR_INTERNAL;
                goto cleanup;
            }
        }
    }

    error_code = mftscan_all_find_root_index(context, options, &tree->root_index);
    if (error_code != MFTSCAN_OK) {
        goto cleanup;
    }

cleanup:
    free(write_offsets);
    free(child_counts);
    if (error_code != MFTSCAN_OK) {
        mftscan_all_tree_free(tree);
    }
    return error_code;
}

static MftscanError mftscan_all_sort_children(
    const MftscanContext *context,
    const MftscanOptions *options,
    MftscanAllTree *tree) {
    size_t index = 0;
    MftscanAllSortContext sort_context;

    if (context == NULL || options == NULL || tree == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    sort_context.context = context;
    sort_context.options = options;
    sort_context.tree = tree;

    for (index = 0; index < tree->count; ++index) {
        size_t child_start = tree->child_offsets[index];
        size_t child_count = tree->child_offsets[index + 1U] - child_start;
        if (child_count > 1U) {
            qsort_s(
                tree->child_indices + child_start,
                child_count,
                sizeof(size_t),
                mftscan_compare_all_children,
                &sort_context);
        }
    }

    return MFTSCAN_OK;
}

static MftscanError mftscan_all_add_json_node(
    yyjson_mut_doc *document,
    yyjson_mut_val *node_object,
    const MftscanContext *context,
    const MftscanOptions *options,
    const MftscanAllTree *tree,
    size_t directory_index) {
    yyjson_mut_val *children_array = NULL;
    wchar_t *path_text = NULL;
    char *path_utf8 = NULL;
    size_t child_position = 0;
    size_t emitted_count = 0;
    MftscanError error_code = MFTSCAN_OK;

    if (document == NULL || node_object == NULL || context == NULL || options == NULL || tree == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    children_array = yyjson_mut_arr(document);
    if (children_array == NULL) {
        return MFTSCAN_ERROR_JSON;
    }

    error_code = mftscan_build_path(context, context->directories.items[directory_index].frn, &path_text);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    path_utf8 = mftscan_utf8_from_wide(path_text);
    free(path_text);
    if (path_utf8 == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    if (!yyjson_mut_obj_add_strcpy(document, node_object, "path", path_utf8) ||
        !yyjson_mut_obj_add_uint(document, node_object, "bytes", mftscan_all_sort_value(options, tree, directory_index)) ||
        !yyjson_mut_obj_add_val(document, node_object, "children", children_array)) {
        free(path_utf8);
        return MFTSCAN_ERROR_JSON;
    }
    free(path_utf8);

    for (child_position = tree->child_offsets[directory_index];
         child_position < tree->child_offsets[directory_index + 1U];
         ++child_position) {
        size_t child_index = tree->child_indices[child_position];
        const MftscanDirNode *child_node = &context->directories.items[child_index];
        uint64_t child_size = mftscan_all_sort_value(options, tree, child_index);
        yyjson_mut_val *child_object = NULL;

        if (!mftscan_all_directory_ready(child_node)) {
            continue;
        }
        if (child_size < options->min_size) {
            break;
        }
        if (options->has_limit && emitted_count >= options->limit) {
            break;
        }

        child_object = yyjson_mut_arr_add_obj(document, children_array);
        if (child_object == NULL) {
            return MFTSCAN_ERROR_JSON;
        }

        error_code = mftscan_all_add_json_node(document, child_object, context, options, tree, child_index);
        if (error_code != MFTSCAN_OK) {
            return error_code;
        }

        emitted_count += 1U;
    }

    return MFTSCAN_OK;
}

MftscanError mftscan_output_json(const MftscanOptions *options, const MftscanScanResult *scan_result) {
    yyjson_mut_doc *document = NULL;
    yyjson_mut_val *root_object = NULL;
    yyjson_mut_val *items_array = NULL;
    char *volume_utf8 = NULL;
    char *location_utf8 = NULL;
    char *json_text = NULL;
    size_t json_length = 0;
    size_t index = 0;
    MftscanError error_code = MFTSCAN_OK;

    if (options == NULL || scan_result == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    document = yyjson_mut_doc_new(NULL);
    if (document == NULL) {
        return MFTSCAN_ERROR_JSON;
    }

    root_object = yyjson_mut_obj(document);
    items_array = yyjson_mut_arr(document);
    if (root_object == NULL || items_array == NULL) {
        error_code = MFTSCAN_ERROR_JSON;
        goto cleanup;
    }

    yyjson_mut_doc_set_root(document, root_object);
    volume_utf8 = mftscan_utf8_from_wide(options->volume);
    if (volume_utf8 == NULL) {
        error_code = MFTSCAN_ERROR_OUT_OF_MEMORY;
        goto cleanup;
    }

    location_utf8 = mftscan_utf8_from_wide((options->location != NULL) ? options->location : options->volume);
    if (location_utf8 == NULL) {
        error_code = MFTSCAN_ERROR_OUT_OF_MEMORY;
        goto cleanup;
    }

    if (!yyjson_mut_obj_add_strcpy(document, root_object, "volume", volume_utf8) ||
        !yyjson_mut_obj_add_strcpy(document, root_object, "location", location_utf8) ||
        !yyjson_mut_obj_add_strcpy(document, root_object, "sort_by",
            (options->sort_mode == MFTSCAN_SORT_ALLOCATED) ? "allocated" : "logical") ||
        !yyjson_mut_obj_add_uint(document, root_object, "min_size", options->min_size) ||
        !yyjson_mut_obj_add_uint(document, root_object, "total_leaf_dirs", (uint64_t)scan_result->count) ||
        !yyjson_mut_obj_add_val(document, root_object, "items", items_array)) {
        error_code = MFTSCAN_ERROR_JSON;
        goto cleanup;
    }

    for (index = 0; index < scan_result->count; ++index) {
        yyjson_mut_val *item_object = yyjson_mut_arr_add_obj(document, items_array);
        char *path_utf8 = NULL;

        if (item_object == NULL) {
            error_code = MFTSCAN_ERROR_JSON;
            goto cleanup;
        }

        path_utf8 = mftscan_utf8_from_wide(scan_result->items[index].path);
        if (path_utf8 == NULL) {
            error_code = MFTSCAN_ERROR_OUT_OF_MEMORY;
            goto cleanup;
        }

        if (!yyjson_mut_obj_add_strcpy(document, item_object, "path", path_utf8) ||
            !yyjson_mut_obj_add_uint(
                document,
                item_object,
                "bytes",
                (options->sort_mode == MFTSCAN_SORT_ALLOCATED)
                    ? scan_result->items[index].allocated_size
                    : scan_result->items[index].logical_size)) {
            free(path_utf8);
            error_code = MFTSCAN_ERROR_JSON;
            goto cleanup;
        }

        free(path_utf8);
    }

    json_text = yyjson_mut_write(document, YYJSON_WRITE_PRETTY_TWO_SPACES, &json_length);
    if (json_text == NULL) {
        error_code = MFTSCAN_ERROR_JSON;
        goto cleanup;
    }

    if (fwrite(json_text, 1, json_length, stdout) != json_length || fputc('\n', stdout) == EOF) {
        error_code = MFTSCAN_ERROR_JSON;
        goto cleanup;
    }

cleanup:
    free(location_utf8);
    free(volume_utf8);
    free(json_text);
    yyjson_mut_doc_free(document);
    return error_code;
}

MftscanError mftscan_output_all_json(const MftscanOptions *options, const MftscanContext *context) {
    yyjson_mut_doc *document = NULL;
    yyjson_mut_val *root_object = NULL;
    char *json_text = NULL;
    size_t json_length = 0;
    MftscanAllTree tree;
    MftscanError error_code = MFTSCAN_OK;

    if (options == NULL || context == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    memset(&tree, 0, sizeof(tree));
    tree.root_index = MFTSCAN_NO_INDEX;

    error_code = mftscan_all_tree_build(context, options, &tree);
    if (error_code != MFTSCAN_OK) {
        goto cleanup;
    }

    error_code = mftscan_all_sort_children(context, options, &tree);
    if (error_code != MFTSCAN_OK) {
        goto cleanup;
    }

    document = yyjson_mut_doc_new(NULL);
    if (document == NULL) {
        error_code = MFTSCAN_ERROR_JSON;
        goto cleanup;
    }

    root_object = yyjson_mut_obj(document);
    if (root_object == NULL) {
        error_code = MFTSCAN_ERROR_JSON;
        goto cleanup;
    }

    yyjson_mut_doc_set_root(document, root_object);
    error_code = mftscan_all_add_json_node(document, root_object, context, options, &tree, tree.root_index);
    if (error_code != MFTSCAN_OK) {
        goto cleanup;
    }

    json_text = yyjson_mut_write(document, YYJSON_WRITE_NOFLAG, &json_length);
    if (json_text == NULL) {
        error_code = MFTSCAN_ERROR_JSON;
        goto cleanup;
    }

    if (fwrite(json_text, 1, json_length, stdout) != json_length || fputc('\n', stdout) == EOF) {
        error_code = MFTSCAN_ERROR_JSON;
        goto cleanup;
    }

cleanup:
    free(json_text);
    if (document != NULL) {
        yyjson_mut_doc_free(document);
    }
    mftscan_all_tree_free(&tree);
    return error_code;
}
