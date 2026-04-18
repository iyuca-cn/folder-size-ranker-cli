#include <windows.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <wchar.h>

#include "model.h"

static size_t mftscan_hash_uint64(uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return (size_t)value;
}

static size_t mftscan_next_power_of_two(size_t value) {
    size_t power = 16;
    while (power < value) {
        power <<= 1U;
    }
    return power;
}

static MftscanError mftscan_map_resize(MftscanUint64Map *map, size_t required_capacity) {
    MftscanUint64MapEntry *old_entries = map->entries;
    size_t old_capacity = map->capacity;
    size_t new_capacity = mftscan_next_power_of_two(required_capacity);
    MftscanUint64MapEntry *new_entries = (MftscanUint64MapEntry *)calloc(new_capacity, sizeof(MftscanUint64MapEntry));
    size_t index = 0;

    if (new_entries == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    map->entries = new_entries;
    map->capacity = new_capacity;
    map->count = 0;

    for (index = 0; index < old_capacity; ++index) {
        if (old_entries[index].occupied) {
            (void)mftscan_map_put(map, old_entries[index].key, old_entries[index].value);
        }
    }

    free(old_entries);
    return MFTSCAN_OK;
}

void *mftscan_realloc_array(void *buffer, size_t item_size, size_t *capacity, size_t required_count) {
    size_t new_capacity = *capacity;
    void *new_buffer = NULL;

    if (required_count <= *capacity) {
        return buffer;
    }

    if (new_capacity == 0) {
        new_capacity = 16;
    }
    while (new_capacity < required_count) {
        new_capacity <<= 1U;
    }

    new_buffer = realloc(buffer, item_size * new_capacity);
    if (new_buffer == NULL) {
        return NULL;
    }

    *capacity = new_capacity;
    return new_buffer;
}

wchar_t *mftscan_strdup_w(const wchar_t *source_text) {
    size_t text_length = 0;
    wchar_t *copy_text = NULL;

    if (source_text == NULL) {
        return NULL;
    }

    text_length = wcslen(source_text) + 1;
    copy_text = (wchar_t *)malloc(text_length * sizeof(wchar_t));
    if (copy_text == NULL) {
        return NULL;
    }

    memcpy(copy_text, source_text, text_length * sizeof(wchar_t));
    return copy_text;
}

char *mftscan_utf8_from_wide(const wchar_t *wide_text) {
    int required_length = 0;
    char *utf8_text = NULL;

    if (wide_text == NULL) {
        return NULL;
    }

    required_length = WideCharToMultiByte(CP_UTF8, 0, wide_text, -1, NULL, 0, NULL, NULL);
    if (required_length <= 0) {
        return NULL;
    }

    utf8_text = (char *)malloc((size_t)required_length);
    if (utf8_text == NULL) {
        return NULL;
    }

    if (WideCharToMultiByte(CP_UTF8, 0, wide_text, -1, utf8_text, required_length, NULL, NULL) <= 0) {
        free(utf8_text);
        return NULL;
    }

    return utf8_text;
}

const char *mftscan_error_message(MftscanError error_code) {
    switch (error_code) {
    case MFTSCAN_OK:
        return "成功";
    case MFTSCAN_ERROR_INVALID_ARGUMENT:
        return "命令行参数非法";
    case MFTSCAN_ERROR_NOT_ADMIN:
        return "需要管理员权限运行";
    case MFTSCAN_ERROR_UNSUPPORTED_FILESYSTEM:
        return "目标卷不是 NTFS";
    case MFTSCAN_ERROR_OPEN_VOLUME:
        return "无法打开目标卷";
    case MFTSCAN_ERROR_VOLUME_QUERY:
        return "无法获取 NTFS 卷信息";
    case MFTSCAN_ERROR_MFT_ENUM:
        return "MFT 记录枚举失败";
    case MFTSCAN_ERROR_MFT_PARSE:
        return "MFT 记录解析失败";
    case MFTSCAN_ERROR_OUT_OF_MEMORY:
        return "内存不足";
    case MFTSCAN_ERROR_JSON:
        return "JSON 输出失败";
    default:
        return "内部错误";
    }
}

bool mftscan_map_get(const MftscanUint64Map *map, uint64_t key, size_t *value) {
    size_t mask = 0;
    size_t slot = 0;

    if (map->capacity == 0) {
        return false;
    }

    mask = map->capacity - 1;
    slot = mftscan_hash_uint64(key) & mask;

    while (map->entries[slot].occupied) {
        if (map->entries[slot].key == key) {
            if (value != NULL) {
                *value = map->entries[slot].value;
            }
            return true;
        }
        slot = (slot + 1U) & mask;
    }

    return false;
}

MftscanError mftscan_map_put(MftscanUint64Map *map, uint64_t key, size_t value) {
    size_t mask = 0;
    size_t slot = 0;

    if (map->capacity == 0 || (map->count + 1U) * 10U >= map->capacity * 7U) {
        MftscanError resize_error = mftscan_map_resize(map, (map->capacity == 0) ? 32U : (map->capacity << 1U));
        if (resize_error != MFTSCAN_OK) {
            return resize_error;
        }
    }

    mask = map->capacity - 1U;
    slot = mftscan_hash_uint64(key) & mask;
    while (map->entries[slot].occupied) {
        if (map->entries[slot].key == key) {
            map->entries[slot].value = value;
            return MFTSCAN_OK;
        }
        slot = (slot + 1U) & mask;
    }

    map->entries[slot].occupied = true;
    map->entries[slot].key = key;
    map->entries[slot].value = value;
    map->count += 1U;
    return MFTSCAN_OK;
}

bool mftscan_set_contains(const MftscanUint64Map *set_map, uint64_t key) {
    return mftscan_map_get(set_map, key, NULL);
}

MftscanError mftscan_set_add(MftscanUint64Map *set_map, uint64_t key) {
    return mftscan_map_put(set_map, key, 1U);
}

void mftscan_map_free(MftscanUint64Map *map) {
    free(map->entries);
    map->entries = NULL;
    map->count = 0;
    map->capacity = 0;
}

void mftscan_free_record_info(MftscanRecordInfo *record_info) {
    if (record_info == NULL) {
        return;
    }

    free(record_info->name);
    memset(record_info, 0, sizeof(*record_info));
}

static bool mftscan_parse_uint64_text(const wchar_t *text, uint64_t *value) {
    wchar_t *end_text = NULL;
    unsigned long long parsed_value = 0;

    errno = 0;
    parsed_value = wcstoull(text, &end_text, 10);
    if (errno != 0 || end_text == NULL || *end_text != L'\0') {
        return false;
    }

    *value = (uint64_t)parsed_value;
    return true;
}

void mftscan_print_help(FILE *stream) {
    fprintf(
        stream,
        "用法:\n"
        "  mftscan.exe --volume C: --sort <logical|allocated> [--min-size bytes] [--format <table|json>] [--limit N]\n\n"
        "说明:\n"
        "  - 直接读取指定 NTFS 卷的 MFT\n"
        "  - 只输出没有子文件夹的文件夹\n"
        "  - 需要管理员权限运行\n");
}

MftscanError mftscan_parse_options(int argc, wchar_t **argv, MftscanOptions *options, bool *show_help) {
    int index = 0;
    bool has_volume = false;
    bool has_sort = false;

    if (options == NULL || show_help == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    memset(options, 0, sizeof(*options));
    options->format = MFTSCAN_FORMAT_TABLE;
    *show_help = false;

    for (index = 1; index < argc; ++index) {
        const wchar_t *argument = argv[index];

        if (_wcsicmp(argument, L"--help") == 0 || _wcsicmp(argument, L"-h") == 0) {
            *show_help = true;
            return MFTSCAN_OK;
        }

        if ((index + 1) >= argc) {
            return MFTSCAN_ERROR_INVALID_ARGUMENT;
        }

        if (_wcsicmp(argument, L"--volume") == 0) {
            const wchar_t *volume_text = argv[++index];
            if (wcslen(volume_text) != 2 || volume_text[1] != L':' ||
                !((volume_text[0] >= L'a' && volume_text[0] <= L'z') ||
                    (volume_text[0] >= L'A' && volume_text[0] <= L'Z'))) {
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }
            options->volume[0] = (wchar_t)towupper(volume_text[0]);
            options->volume[1] = L':';
            options->volume[2] = L'\0';
            has_volume = true;
        } else if (_wcsicmp(argument, L"--sort") == 0) {
            const wchar_t *sort_text = argv[++index];
            if (_wcsicmp(sort_text, L"logical") == 0) {
                options->sort_mode = MFTSCAN_SORT_LOGICAL;
            } else if (_wcsicmp(sort_text, L"allocated") == 0) {
                options->sort_mode = MFTSCAN_SORT_ALLOCATED;
            } else {
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }
            has_sort = true;
        } else if (_wcsicmp(argument, L"--min-size") == 0) {
            if (!mftscan_parse_uint64_text(argv[++index], &options->min_size)) {
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }
        } else if (_wcsicmp(argument, L"--format") == 0) {
            const wchar_t *format_text = argv[++index];
            if (_wcsicmp(format_text, L"table") == 0) {
                options->format = MFTSCAN_FORMAT_TABLE;
            } else if (_wcsicmp(format_text, L"json") == 0) {
                options->format = MFTSCAN_FORMAT_JSON;
            } else {
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }
        } else if (_wcsicmp(argument, L"--limit") == 0) {
            uint64_t parsed_limit = 0;
            if (!mftscan_parse_uint64_text(argv[++index], &parsed_limit)) {
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }
            options->limit = (size_t)parsed_limit;
            options->has_limit = true;
        } else {
            return MFTSCAN_ERROR_INVALID_ARGUMENT;
        }
    }

    if (!has_volume || !has_sort) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    return MFTSCAN_OK;
}
