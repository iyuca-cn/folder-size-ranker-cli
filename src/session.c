#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "model.h"
#include "session.h"

#define MFTSCAN_NO_INDEX ((size_t)-1)
#define MFTSCAN_INVALID_NODE_ID UINT32_MAX

typedef struct MftscanSessionTree {
    uint64_t *logical_totals;
    uint64_t *allocated_totals;
    size_t *parent_indices;
    size_t *child_offsets;
    size_t *child_indices;
    size_t *file_offsets;
    size_t *file_indices;
    uint32_t *direct_file_counts;
    uint32_t *direct_child_directory_counts;
    uint32_t *total_file_counts;
    uint32_t *total_directory_counts;
    size_t count;
    size_t child_count;
    size_t file_count;
    size_t root_index;
} MftscanSessionTree;

struct MftscanSession {
    MftscanOptions options;
    MftscanContext context;
    MftscanSessionTree tree;
};

typedef struct MftscanSessionSortContext {
    const MftscanContext *context;
    const MftscanOptions *options;
    const MftscanSessionTree *tree;
} MftscanSessionSortContext;

typedef struct MftscanFilteredFileTotals {
    uint64_t logical_size;
    uint64_t allocated_size;
} MftscanFilteredFileTotals;

static uint32_t mftscan_saturate_uint32(size_t value) {
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static uint32_t mftscan_add_saturated_uint32(uint32_t left, uint32_t right) {
    return (UINT32_MAX - left < right) ? UINT32_MAX : (left + right);
}

static bool mftscan_session_directory_ready(const MftscanDirNode *directory_node) {
    return directory_node != NULL &&
        (directory_node->metadata_ready || directory_node->frn == MFTSCAN_ROOT_FRN);
}

static uint64_t mftscan_session_sort_value(
    const MftscanOptions *options,
    const MftscanSessionTree *tree,
    size_t directory_index) {
    return (options->sort_mode == MFTSCAN_SORT_ALLOCATED)
        ? tree->allocated_totals[directory_index]
        : tree->logical_totals[directory_index];
}

static uint64_t mftscan_session_file_sort_value(
    const MftscanOptions *options,
    const MftscanFileNode *file_node) {
    return (options->sort_mode == MFTSCAN_SORT_ALLOCATED)
        ? file_node->allocated_size
        : file_node->logical_size;
}

static bool mftscan_session_file_visible(
    const MftscanOptions *options,
    const MftscanFileNode *file_node) {
    if (options == NULL || file_node == NULL) {
        return false;
    }

    return mftscan_session_file_sort_value(options, file_node) >= options->min_size;
}

static bool mftscan_session_directory_is_under_root(
    const MftscanSessionTree *tree,
    size_t directory_index) {
    size_t current_index = directory_index;
    size_t guard_count = 0;

    if (tree == NULL || directory_index >= tree->count || tree->root_index >= tree->count) {
        return false;
    }

    while (current_index != MFTSCAN_NO_INDEX) {
        if (current_index == tree->root_index) {
            return true;
        }
        if (tree->parent_indices[current_index] == MFTSCAN_NO_INDEX) {
            break;
        }

        current_index = tree->parent_indices[current_index];
        guard_count += 1U;
        if (guard_count > tree->count) {
            return false;
        }
    }

    return false;
}

static int __cdecl mftscan_session_compare_children(void *context, const void *left_value, const void *right_value) {
    const MftscanSessionSortContext *sort_context = (const MftscanSessionSortContext *)context;
    size_t left_index = *(const size_t *)left_value;
    size_t right_index = *(const size_t *)right_value;
    const MftscanDirNode *left_node = &sort_context->context->directories.items[left_index];
    const MftscanDirNode *right_node = &sort_context->context->directories.items[right_index];
    uint64_t left_sort = mftscan_session_sort_value(sort_context->options, sort_context->tree, left_index);
    uint64_t right_sort = mftscan_session_sort_value(sort_context->options, sort_context->tree, right_index);

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

static int __cdecl mftscan_session_compare_files(void *context, const void *left_value, const void *right_value) {
    const MftscanSessionSortContext *sort_context = (const MftscanSessionSortContext *)context;
    size_t left_index = *(const size_t *)left_value;
    size_t right_index = *(const size_t *)right_value;
    const MftscanFileNode *left_node = &sort_context->context->files.items[left_index];
    const MftscanFileNode *right_node = &sort_context->context->files.items[right_index];
    uint64_t left_sort = mftscan_session_file_sort_value(sort_context->options, left_node);
    uint64_t right_sort = mftscan_session_file_sort_value(sort_context->options, right_node);

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

static void mftscan_session_tree_free(MftscanSessionTree *tree) {
    if (tree == NULL) {
        return;
    }

    free(tree->logical_totals);
    free(tree->allocated_totals);
    free(tree->parent_indices);
    free(tree->child_offsets);
    free(tree->child_indices);
    free(tree->file_offsets);
    free(tree->file_indices);
    free(tree->direct_file_counts);
    free(tree->direct_child_directory_counts);
    free(tree->total_file_counts);
    free(tree->total_directory_counts);
    memset(tree, 0, sizeof(*tree));
    tree->root_index = MFTSCAN_NO_INDEX;
}

static MftscanError mftscan_session_find_root_index(
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

        if (!mftscan_session_directory_ready(directory_node)) {
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

static MftscanError mftscan_session_tree_build(
    const MftscanContext *context,
    const MftscanOptions *options,
    MftscanSessionTree *tree) {
    size_t *child_counts = NULL;
    size_t *file_counts = NULL;
    size_t *write_offsets = NULL;
    size_t *write_file_offsets = NULL;
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
    tree->file_offsets = (size_t *)calloc(tree->count + 1U, sizeof(size_t));
    tree->direct_file_counts = (uint32_t *)calloc(tree->count, sizeof(uint32_t));
    tree->direct_child_directory_counts = (uint32_t *)calloc(tree->count, sizeof(uint32_t));
    tree->total_file_counts = (uint32_t *)calloc(tree->count, sizeof(uint32_t));
    tree->total_directory_counts = (uint32_t *)calloc(tree->count, sizeof(uint32_t));
    child_counts = (size_t *)calloc(tree->count, sizeof(size_t));
    file_counts = (size_t *)calloc(tree->count, sizeof(size_t));
    write_offsets = (size_t *)calloc(tree->count, sizeof(size_t));
    write_file_offsets = (size_t *)calloc(tree->count, sizeof(size_t));
    if (tree->logical_totals == NULL ||
        tree->allocated_totals == NULL ||
        tree->parent_indices == NULL ||
        tree->child_offsets == NULL ||
        tree->file_offsets == NULL ||
        tree->direct_file_counts == NULL ||
        tree->direct_child_directory_counts == NULL ||
        tree->total_file_counts == NULL ||
        tree->total_directory_counts == NULL ||
        child_counts == NULL ||
        file_counts == NULL ||
        write_offsets == NULL ||
        write_file_offsets == NULL) {
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

    for (index = 0; index < context->files.count; ++index) {
        size_t parent_index = MFTSCAN_NO_INDEX;
        const MftscanFileNode *file_node = &context->files.items[index];

        if (file_node->parent_frn == 0ULL || file_node->parent_frn == file_node->frn) {
            continue;
        }
        if (file_node->name == NULL || file_node->name[0] == L'\0') {
            continue;
        }
        if (mftscan_map_get(&context->directory_index, file_node->parent_frn, &parent_index)) {
            file_counts[parent_index] += 1U;
        }
    }

    for (index = 0; index < tree->count; ++index) {
        tree->child_offsets[index] = running_offset;
        running_offset += child_counts[index];
    }
    tree->child_offsets[tree->count] = running_offset;
    tree->child_count = running_offset;
    running_offset = 0U;
    for (index = 0; index < tree->count; ++index) {
        tree->file_offsets[index] = running_offset;
        running_offset += file_counts[index];
    }
    tree->file_offsets[tree->count] = running_offset;
    tree->file_count = running_offset;

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

    if (tree->file_count > 0U) {
        tree->file_indices = (size_t *)calloc(tree->file_count, sizeof(size_t));
        if (tree->file_indices == NULL) {
            error_code = MFTSCAN_ERROR_OUT_OF_MEMORY;
            goto cleanup;
        }

        memcpy(write_file_offsets, tree->file_offsets, tree->count * sizeof(size_t));
        for (index = 0; index < context->files.count; ++index) {
            size_t parent_index = MFTSCAN_NO_INDEX;
            const MftscanFileNode *file_node = &context->files.items[index];

            if (file_node->parent_frn == 0ULL || file_node->parent_frn == file_node->frn) {
                continue;
            }
            if (file_node->name == NULL || file_node->name[0] == L'\0') {
                continue;
            }
            if (mftscan_map_get(&context->directory_index, file_node->parent_frn, &parent_index)) {
                tree->file_indices[write_file_offsets[parent_index]++] = index;
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

    error_code = mftscan_session_find_root_index(context, options, &tree->root_index);
    if (error_code != MFTSCAN_OK) {
        goto cleanup;
    }

cleanup:
    free(write_file_offsets);
    free(write_offsets);
    free(file_counts);
    free(child_counts);
    if (error_code != MFTSCAN_OK) {
        mftscan_session_tree_free(tree);
    }
    return error_code;
}

static MftscanError mftscan_session_sort_children(
    const MftscanContext *context,
    const MftscanOptions *options,
    MftscanSessionTree *tree) {
    size_t index = 0;
    MftscanSessionSortContext sort_context;

    if (context == NULL || options == NULL || tree == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    sort_context.context = context;
    sort_context.options = options;
    sort_context.tree = tree;

    for (index = 0; index < tree->count; ++index) {
        size_t child_start = tree->child_offsets[index];
        size_t child_count = tree->child_offsets[index + 1U] - child_start;
        size_t file_start = tree->file_offsets[index];
        size_t file_count = tree->file_offsets[index + 1U] - file_start;
        if (child_count > 1U) {
            qsort_s(
                tree->child_indices + child_start,
                child_count,
                sizeof(size_t),
                mftscan_session_compare_children,
                &sort_context);
        }
        if (file_count > 1U) {
            qsort_s(
                tree->file_indices + file_start,
                file_count,
                sizeof(size_t),
                mftscan_session_compare_files,
                &sort_context);
        }
    }

    return MFTSCAN_OK;
}

static MftscanError mftscan_session_recompute_visible_logical_total(
    const MftscanContext *context,
    const MftscanOptions *options,
    MftscanSessionTree *tree,
    size_t directory_index,
    uint8_t *visit_states) {
    size_t file_position = 0;
    size_t child_position = 0;
    size_t emitted_file_count = 0;
    size_t emitted_child_count = 0;
    size_t child_start = 0;
    size_t child_count = 0;
    uint64_t total_size = 0ULL;
    MftscanSessionSortContext sort_context;

    if (context == NULL || options == NULL || tree == NULL || visit_states == NULL ||
        directory_index >= tree->count) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (visit_states[directory_index] == 2U) {
        return MFTSCAN_OK;
    }
    if (visit_states[directory_index] == 1U) {
        return MFTSCAN_ERROR_INTERNAL;
    }

    visit_states[directory_index] = 1U;
    tree->logical_totals[directory_index] = 0ULL;

    for (file_position = tree->file_offsets[directory_index];
         file_position < tree->file_offsets[directory_index + 1U];
         ++file_position) {
        size_t file_index = tree->file_indices[file_position];
        const MftscanFileNode *file_node = &context->files.items[file_index];

        if (!mftscan_session_file_visible(options, file_node)) {
            break;
        }
        if (options->has_limit && emitted_file_count >= options->limit) {
            break;
        }

        total_size += file_node->logical_size;
        emitted_file_count += 1U;
    }

    for (child_position = tree->child_offsets[directory_index];
         child_position < tree->child_offsets[directory_index + 1U];
         ++child_position) {
        size_t child_index = tree->child_indices[child_position];
        const MftscanDirNode *child_node = &context->directories.items[child_index];
        MftscanError error_code = MFTSCAN_OK;

        if (!mftscan_session_directory_ready(child_node)) {
            tree->logical_totals[child_index] = 0ULL;
            continue;
        }

        error_code = mftscan_session_recompute_visible_logical_total(
            context,
            options,
            tree,
            child_index,
            visit_states);
        if (error_code != MFTSCAN_OK) {
            return error_code;
        }
    }

    child_start = tree->child_offsets[directory_index];
    child_count = tree->child_offsets[directory_index + 1U] - child_start;
    if (child_count > 1U) {
        sort_context.context = context;
        sort_context.options = options;
        sort_context.tree = tree;
        qsort_s(
            tree->child_indices + child_start,
            child_count,
            sizeof(size_t),
            mftscan_session_compare_children,
            &sort_context);
    }

    for (child_position = child_start;
         child_position < tree->child_offsets[directory_index + 1U];
         ++child_position) {
        size_t child_index = tree->child_indices[child_position];
        const MftscanDirNode *child_node = &context->directories.items[child_index];
        uint64_t child_size = tree->logical_totals[child_index];

        if (!mftscan_session_directory_ready(child_node)) {
            continue;
        }
        if (child_size < options->min_size) {
            break;
        }
        if (options->has_limit && emitted_child_count >= options->limit) {
            break;
        }

        total_size += child_size;
        emitted_child_count += 1U;
    }

    tree->logical_totals[directory_index] = total_size;
    visit_states[directory_index] = 2U;
    return MFTSCAN_OK;
}

static MftscanError mftscan_session_recompute_visible_logical_totals(
    const MftscanContext *context,
    const MftscanOptions *options,
    MftscanSessionTree *tree) {
    uint8_t *visit_states = NULL;
    MftscanError error_code = MFTSCAN_OK;

    if (context == NULL || options == NULL || tree == NULL || tree->root_index >= tree->count) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    visit_states = (uint8_t *)calloc(tree->count, sizeof(uint8_t));
    if (visit_states == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    memset(tree->logical_totals, 0, tree->count * sizeof(uint64_t));
    error_code = mftscan_session_recompute_visible_logical_total(
        context,
        options,
        tree,
        tree->root_index,
        visit_states);

    free(visit_states);
    return error_code;
}

static MftscanError mftscan_session_recompute_filtered_totals(
    const MftscanContext *context,
    const MftscanOptions *options,
    MftscanSessionTree *tree) {
    MftscanFilteredFileTotals *direct_file_totals = NULL;
    MftscanUint64Map charged_allocated_file_frns = { 0 };
    size_t index = 0;
    MftscanError error_code = MFTSCAN_OK;

    if (context == NULL || options == NULL || tree == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (options->sort_mode == MFTSCAN_SORT_LOGICAL) {
        return mftscan_session_recompute_visible_logical_totals(context, options, tree);
    }

    direct_file_totals = (MftscanFilteredFileTotals *)calloc(tree->count, sizeof(MftscanFilteredFileTotals));
    if (direct_file_totals == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    memset(tree->logical_totals, 0, tree->count * sizeof(uint64_t));
    memset(tree->allocated_totals, 0, tree->count * sizeof(uint64_t));

    for (index = 0; index < tree->count; ++index) {
        uint64_t allocated_size = context->directories.items[index].metadata_allocated_size;
        size_t current_index = index;
        size_t guard_count = 0;

        if (allocated_size == 0ULL || !mftscan_session_directory_is_under_root(tree, index)) {
            continue;
        }

        tree->allocated_totals[current_index] += allocated_size;
        while (tree->parent_indices[current_index] != MFTSCAN_NO_INDEX) {
            size_t parent_index = tree->parent_indices[current_index];

            tree->allocated_totals[parent_index] += allocated_size;
            if (parent_index == tree->root_index) {
                break;
            }
            current_index = parent_index;
            guard_count += 1U;
            if (guard_count > tree->count) {
                free(direct_file_totals);
                mftscan_map_free(&charged_allocated_file_frns);
                return MFTSCAN_ERROR_INTERNAL;
            }
        }
    }

    for (index = 0; index < tree->count; ++index) {
        size_t file_position = 0;
        size_t emitted_file_count = 0;

        if (!mftscan_session_directory_is_under_root(tree, index)) {
            continue;
        }

        for (file_position = tree->file_offsets[index];
             file_position < tree->file_offsets[index + 1U];
             ++file_position) {
            size_t file_index = tree->file_indices[file_position];
            const MftscanFileNode *file_node = &context->files.items[file_index];
            size_t ignored_index = 0;

            if (!mftscan_session_file_visible(options, file_node)) {
                break;
            }
            if (options->has_limit && emitted_file_count >= options->limit) {
                break;
            }

            direct_file_totals[index].logical_size += file_node->logical_size;

            if (!mftscan_map_get(&charged_allocated_file_frns, file_node->frn, &ignored_index)) {
                error_code = mftscan_map_put(&charged_allocated_file_frns, file_node->frn, file_index);
                if (error_code != MFTSCAN_OK) {
                    free(direct_file_totals);
                    mftscan_map_free(&charged_allocated_file_frns);
                    return error_code;
                }

                direct_file_totals[index].allocated_size += file_node->allocated_size;
            }

            emitted_file_count += 1U;
        }
    }

    for (index = 0; index < tree->count; ++index) {
        uint64_t logical_size = direct_file_totals[index].logical_size;
        uint64_t allocated_size = direct_file_totals[index].allocated_size;
        size_t current_index = index;
        size_t guard_count = 0;

        if (logical_size == 0ULL && allocated_size == 0ULL) {
            continue;
        }

        tree->logical_totals[current_index] += logical_size;
        tree->allocated_totals[current_index] += allocated_size;

        while (tree->parent_indices[current_index] != MFTSCAN_NO_INDEX) {
            size_t parent_index = tree->parent_indices[current_index];

            tree->logical_totals[parent_index] += logical_size;
            tree->allocated_totals[parent_index] += allocated_size;
            if (parent_index == tree->root_index) {
                break;
            }
            current_index = parent_index;
            guard_count += 1U;
            if (guard_count > tree->count) {
                free(direct_file_totals);
                mftscan_map_free(&charged_allocated_file_frns);
                return MFTSCAN_ERROR_INTERNAL;
            }
        }
    }

    free(direct_file_totals);
    mftscan_map_free(&charged_allocated_file_frns);
    return MFTSCAN_OK;
}

static MftscanError mftscan_session_compute_counts_for_directory(
    const MftscanContext *context,
    const MftscanOptions *options,
    MftscanSessionTree *tree,
    size_t directory_index,
    uint8_t *visit_states) {
    size_t file_position = 0;
    size_t child_position = 0;
    size_t direct_files = 0;
    size_t direct_directories = 0;
    uint32_t total_files = 0;
    uint32_t total_directories = 0;

    if (context == NULL || options == NULL || tree == NULL || visit_states == NULL ||
        directory_index >= tree->count) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (visit_states[directory_index] == 2U) {
        return MFTSCAN_OK;
    }
    if (visit_states[directory_index] == 1U) {
        return MFTSCAN_ERROR_INTERNAL;
    }

    visit_states[directory_index] = 1U;

    for (file_position = tree->file_offsets[directory_index];
         file_position < tree->file_offsets[directory_index + 1U];
         ++file_position) {
        size_t file_index = tree->file_indices[file_position];
        const MftscanFileNode *file_node = &context->files.items[file_index];

        if (!mftscan_session_file_visible(options, file_node)) {
            break;
        }
        if (options->has_limit && direct_files >= options->limit) {
            break;
        }

        direct_files += 1U;
    }

    tree->direct_file_counts[directory_index] = mftscan_saturate_uint32(direct_files);
    total_files = tree->direct_file_counts[directory_index];

    for (child_position = tree->child_offsets[directory_index];
         child_position < tree->child_offsets[directory_index + 1U];
         ++child_position) {
        size_t child_index = tree->child_indices[child_position];
        const MftscanDirNode *child_node = &context->directories.items[child_index];
        uint64_t child_size = mftscan_session_sort_value(options, tree, child_index);
        MftscanError error_code = MFTSCAN_OK;

        if (!mftscan_session_directory_ready(child_node)) {
            continue;
        }
        if (child_size < options->min_size) {
            break;
        }
        if (options->has_limit && direct_directories >= options->limit) {
            break;
        }

        error_code = mftscan_session_compute_counts_for_directory(
            context,
            options,
            tree,
            child_index,
            visit_states);
        if (error_code != MFTSCAN_OK) {
            return error_code;
        }

        direct_directories += 1U;
        total_files = mftscan_add_saturated_uint32(total_files, tree->total_file_counts[child_index]);
        total_directories = mftscan_add_saturated_uint32(total_directories, 1U);
        total_directories = mftscan_add_saturated_uint32(total_directories, tree->total_directory_counts[child_index]);
    }

    tree->direct_child_directory_counts[directory_index] = mftscan_saturate_uint32(direct_directories);
    tree->total_file_counts[directory_index] = total_files;
    tree->total_directory_counts[directory_index] = total_directories;
    visit_states[directory_index] = 2U;
    return MFTSCAN_OK;
}

static MftscanError mftscan_session_compute_counts(
    const MftscanContext *context,
    const MftscanOptions *options,
    MftscanSessionTree *tree) {
    uint8_t *visit_states = NULL;
    MftscanError error_code = MFTSCAN_OK;

    if (context == NULL || options == NULL || tree == NULL || tree->root_index >= tree->count) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    visit_states = (uint8_t *)calloc(tree->count, sizeof(uint8_t));
    if (visit_states == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    error_code = mftscan_session_compute_counts_for_directory(
        context,
        options,
        tree,
        tree->root_index,
        visit_states);

    free(visit_states);
    return error_code;
}

static MftscanError mftscan_session_build_scan_options(
    const MftscanSessionOptions *source_options,
    MftscanOptions *options) {
    wchar_t *argv[6] = { 0 };
    wchar_t *sort_text = NULL;
    bool show_help = false;
    MftscanError error_code = MFTSCAN_OK;

    if (source_options == NULL || options == NULL || source_options->location == NULL ||
        source_options->location[0] == L'\0') {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    sort_text = source_options->sort_mode == MFTSCAN_SORT_LOGICAL ? L"logical" : L"allocated";
    argv[0] = L"mftscan";
    argv[1] = L"--location";
    argv[2] = (wchar_t *)source_options->location;
    argv[3] = L"--sort";
    argv[4] = sort_text;
    argv[5] = L"--all";

    error_code = mftscan_parse_options(6, argv, options, &show_help);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    options->min_size = source_options->min_size;
    options->has_limit = source_options->has_limit;
    options->limit = source_options->limit;
    options->output_all = true;
    options->format = MFTSCAN_FORMAT_JSON;
    return MFTSCAN_OK;
}

static MftscanError mftscan_session_prepare_tree(MftscanSession *session) {
    MftscanError error_code = MFTSCAN_OK;

    if (session == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    error_code = mftscan_session_tree_build(&session->context, &session->options, &session->tree);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    error_code = mftscan_session_sort_children(&session->context, &session->options, &session->tree);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    error_code = mftscan_session_recompute_filtered_totals(&session->context, &session->options, &session->tree);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    error_code = mftscan_session_sort_children(&session->context, &session->options, &session->tree);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    return mftscan_session_compute_counts(&session->context, &session->options, &session->tree);
}

static void mftscan_session_fill_node_info(
    const MftscanSession *session,
    size_t directory_index,
    MftscanNodeInfo *node) {
    const MftscanDirNode *directory_node = &session->context.directories.items[directory_index];

    memset(node, 0, sizeof(*node));
    node->node_id = (uint32_t)directory_index;
    node->parent_node_id = session->tree.parent_indices[directory_index] == MFTSCAN_NO_INDEX
        ? MFTSCAN_INVALID_NODE_ID
        : (uint32_t)session->tree.parent_indices[directory_index];
    node->name = directory_index == session->tree.root_index
        ? session->options.location
        : (directory_node->name == NULL ? L"" : directory_node->name);
    node->path = directory_index == session->tree.root_index ? session->options.location : NULL;
    node->logical_size = session->tree.logical_totals[directory_index];
    node->allocated_size = session->tree.allocated_totals[directory_index];
    node->bytes = mftscan_session_sort_value(&session->options, &session->tree, directory_index);
    node->direct_file_count = session->tree.direct_file_counts[directory_index];
    node->total_file_count = session->tree.total_file_counts[directory_index];
    node->total_directory_count = session->tree.total_directory_counts[directory_index];
    node->direct_child_directory_count = session->tree.direct_child_directory_counts[directory_index];
    node->has_children = node->direct_file_count > 0U || node->direct_child_directory_count > 0U;
}

static void mftscan_session_fill_directory_child(
    const MftscanSession *session,
    size_t parent_index,
    size_t directory_index,
    MftscanChildInfo *child) {
    const MftscanDirNode *directory_node = &session->context.directories.items[directory_index];

    memset(child, 0, sizeof(*child));
    child->kind = MFTSCAN_NODE_DIRECTORY;
    child->node_id = (uint32_t)directory_index;
    child->parent_node_id = (uint32_t)parent_index;
    child->name = directory_node->name == NULL ? L"" : directory_node->name;
    child->logical_size = session->tree.logical_totals[directory_index];
    child->allocated_size = session->tree.allocated_totals[directory_index];
    child->bytes = mftscan_session_sort_value(&session->options, &session->tree, directory_index);
    child->direct_file_count = session->tree.direct_file_counts[directory_index];
    child->total_file_count = session->tree.total_file_counts[directory_index];
    child->total_directory_count = session->tree.total_directory_counts[directory_index];
    child->direct_child_directory_count = session->tree.direct_child_directory_counts[directory_index];
    child->has_children = child->direct_file_count > 0U || child->direct_child_directory_count > 0U;
}

static void mftscan_session_fill_file_child(
    const MftscanSession *session,
    size_t parent_index,
    size_t file_index,
    MftscanChildInfo *child) {
    const MftscanFileNode *file_node = &session->context.files.items[file_index];

    memset(child, 0, sizeof(*child));
    child->kind = MFTSCAN_NODE_FILE;
    child->node_id = MFTSCAN_INVALID_NODE_ID;
    child->parent_node_id = (uint32_t)parent_index;
    child->name = file_node->name == NULL ? L"" : file_node->name;
    child->logical_size = file_node->logical_size;
    child->allocated_size = file_node->allocated_size;
    child->bytes = mftscan_session_file_sort_value(&session->options, file_node);
    child->direct_file_count = 0U;
    child->total_file_count = 1U;
    child->total_directory_count = 0U;
    child->direct_child_directory_count = 0U;
    child->has_children = false;
}

MftscanError mftscan_session_scan(const MftscanSessionOptions *options, MftscanSession **session) {
    MftscanSession *created_session = NULL;
    MftscanError error_code = MFTSCAN_OK;

    if (options == NULL || session == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    *session = NULL;
    created_session = (MftscanSession *)calloc(1U, sizeof(MftscanSession));
    if (created_session == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }
    created_session->tree.root_index = MFTSCAN_NO_INDEX;

    error_code = mftscan_session_build_scan_options(options, &created_session->options);
    if (error_code != MFTSCAN_OK) {
        mftscan_session_free(created_session);
        return error_code;
    }

    mftscan_context_init(&created_session->context);
    error_code = mftscan_scan_volume(&created_session->context, &created_session->options);
    if (error_code == MFTSCAN_OK) {
        error_code = mftscan_finalize_metadata_tree(&created_session->context);
    }
    if (error_code == MFTSCAN_OK) {
        error_code = mftscan_session_prepare_tree(created_session);
    }
    if (error_code != MFTSCAN_OK) {
        mftscan_session_free(created_session);
        return error_code;
    }

    *session = created_session;
    return MFTSCAN_OK;
}

void mftscan_session_free(MftscanSession *session) {
    if (session == NULL) {
        return;
    }

    mftscan_session_tree_free(&session->tree);
    mftscan_context_free(&session->context);
    mftscan_free_options(&session->options);
    free(session);
}

MftscanError mftscan_session_get_root_node(
    const MftscanSession *session,
    MftscanNodeInfo *node) {
    if (session == NULL || node == NULL || session->tree.root_index >= session->tree.count) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    mftscan_session_fill_node_info(session, session->tree.root_index, node);
    return MFTSCAN_OK;
}

MftscanError mftscan_session_get_node(
    const MftscanSession *session,
    uint32_t node_id,
    MftscanNodeInfo *node) {
    size_t directory_index = (size_t)node_id;

    if (session == NULL || node == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }
    if (directory_index >= session->tree.count) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    mftscan_session_fill_node_info(session, directory_index, node);
    return MFTSCAN_OK;
}

MftscanError mftscan_session_get_children(
    const MftscanSession *session,
    uint32_t node_id,
    uint32_t start,
    uint32_t count,
    MftscanChildBuffer *children) {
    size_t directory_index = (size_t)node_id;
    uint32_t total_count = 0;
    uint32_t remaining = 0;
    uint32_t seen = 0;
    uint32_t emitted = 0;
    size_t file_position = 0;
    size_t child_position = 0;

    if (session == NULL || children == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    memset(children, 0, sizeof(*children));
    if (directory_index >= session->tree.count) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    total_count = mftscan_add_saturated_uint32(
        session->tree.direct_file_counts[directory_index],
        session->tree.direct_child_directory_counts[directory_index]);
    children->total_count = total_count;
    if (count == 0U || start >= total_count) {
        return MFTSCAN_OK;
    }

    remaining = total_count - start;
    if (remaining > count) {
        remaining = count;
    }

    children->items = (MftscanChildInfo *)calloc(remaining, sizeof(MftscanChildInfo));
    if (children->items == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    for (file_position = session->tree.file_offsets[directory_index];
         file_position < session->tree.file_offsets[directory_index + 1U];
         ++file_position) {
        size_t file_index = session->tree.file_indices[file_position];
        const MftscanFileNode *file_node = &session->context.files.items[file_index];

        if (!mftscan_session_file_visible(&session->options, file_node)) {
            break;
        }
        if (session->options.has_limit && seen >= session->tree.direct_file_counts[directory_index]) {
            break;
        }

        if (seen >= start && emitted < remaining) {
            mftscan_session_fill_file_child(session, directory_index, file_index, &children->items[emitted++]);
        }
        seen += 1U;
        if (emitted >= remaining) {
            children->count = emitted;
            return MFTSCAN_OK;
        }
    }

    for (child_position = session->tree.child_offsets[directory_index];
         child_position < session->tree.child_offsets[directory_index + 1U];
         ++child_position) {
        size_t child_index = session->tree.child_indices[child_position];
        const MftscanDirNode *child_node = &session->context.directories.items[child_index];
        uint64_t child_size = mftscan_session_sort_value(&session->options, &session->tree, child_index);
        uint32_t seen_directories = seen - session->tree.direct_file_counts[directory_index];

        if (!mftscan_session_directory_ready(child_node)) {
            continue;
        }
        if (child_size < session->options.min_size) {
            break;
        }
        if (session->options.has_limit &&
            seen_directories >= session->tree.direct_child_directory_counts[directory_index]) {
            break;
        }

        if (seen >= start && emitted < remaining) {
            mftscan_session_fill_directory_child(session, directory_index, child_index, &children->items[emitted++]);
        }
        seen += 1U;
        if (emitted >= remaining) {
            break;
        }
    }

    children->count = emitted;
    return MFTSCAN_OK;
}

void mftscan_child_buffer_free(MftscanChildBuffer *children) {
    if (children == NULL) {
        return;
    }

    free(children->items);
    memset(children, 0, sizeof(*children));
}
