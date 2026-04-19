#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "model.h"
#include "../third_party/yyjson/yyjson.h"

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
