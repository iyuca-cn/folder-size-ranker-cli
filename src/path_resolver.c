#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "model.h"

MftscanError mftscan_build_path(const MftscanContext *context, uint64_t directory_frn, wchar_t **path_text) {
    uint64_t current_frn = directory_frn;
    wchar_t **segments = NULL;
    size_t segment_count = 0;
    size_t segment_capacity = 0;
    size_t total_length = 0;
    wchar_t *built_path = NULL;
    size_t write_offset = 0;

    if (context == NULL || path_text == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    *path_text = NULL;

    while (current_frn != 0ULL && current_frn != MFTSCAN_ROOT_FRN) {
        size_t directory_index = 0;
        const MftscanDirNode *directory_node = NULL;
        wchar_t **grown_segments = NULL;

        if (!mftscan_map_get(&context->directory_index, current_frn, &directory_index)) {
            break;
        }

        directory_node = &context->directories.items[directory_index];
        if (directory_node->name == NULL || directory_node->name[0] == L'\0') {
            break;
        }

        grown_segments = (wchar_t **)mftscan_realloc_array(
            segments,
            sizeof(wchar_t *),
            &segment_capacity,
            segment_count + 1U);
        if (grown_segments == NULL) {
            free(segments);
            return MFTSCAN_ERROR_OUT_OF_MEMORY;
        }

        segments = grown_segments;
        segments[segment_count++] = directory_node->name;

        if (directory_node->parent_frn == 0ULL || directory_node->parent_frn == current_frn) {
            break;
        }

        current_frn = directory_node->parent_frn;
    }

    total_length = wcslen(context->volume) + 1U;
    if (segment_count > 0U) {
        size_t index = 0;
        for (index = 0; index < segment_count; ++index) {
            total_length += wcslen(segments[index]) + 1U;
        }
    }

    built_path = (wchar_t *)calloc(total_length + 1U, sizeof(wchar_t));
    if (built_path == NULL) {
        free(segments);
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    wcscpy(built_path, context->volume);
    wcscat(built_path, L"\\");

    if (segment_count > 0U) {
        size_t reverse_index = 0;
        write_offset = wcslen(built_path);
        for (reverse_index = segment_count; reverse_index > 0U; --reverse_index) {
            const wchar_t *segment = segments[reverse_index - 1U];
            size_t segment_length = wcslen(segment);
            memcpy(built_path + write_offset, segment, segment_length * sizeof(wchar_t));
            write_offset += segment_length;
            if (reverse_index > 1U) {
                built_path[write_offset++] = L'\\';
            }
        }
        built_path[write_offset] = L'\0';
    }

    free(segments);
    *path_text = built_path;
    return MFTSCAN_OK;
}
