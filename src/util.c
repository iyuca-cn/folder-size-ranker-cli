#include <windows.h>

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wctype.h>
#include <wchar.h>

#include "model.h"

static char g_mftscan_error_detail[256] = { 0 };

static void mftscan_clear_error_detail(void) {
    g_mftscan_error_detail[0] = '\0';
}

static void mftscan_set_error_detail(const char *format_text, ...) {
    va_list argument_list;

    if (format_text == NULL) {
        g_mftscan_error_detail[0] = '\0';
        return;
    }

    va_start(argument_list, format_text);
    (void)vsnprintf(g_mftscan_error_detail, sizeof(g_mftscan_error_detail), format_text, argument_list);
    va_end(argument_list);
}

const char *mftscan_error_detail(void) {
    return g_mftscan_error_detail;
}

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

void mftscan_free_options(MftscanOptions *options) {
    if (options == NULL) {
        return;
    }

    free(options->location);
    free(options->filter_root);
    options->location = NULL;
    options->filter_root = NULL;
    options->filter_by_location = false;
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
        return "无法获取卷信息";
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

typedef struct MftscanExprParser {
    const wchar_t *source_text;
    const wchar_t *cursor_text;
    const wchar_t *error_text;
} MftscanExprParser;

static void mftscan_expr_skip_spaces(MftscanExprParser *parser) {
    while (parser->cursor_text != NULL && iswspace(*parser->cursor_text)) {
        parser->cursor_text += 1;
    }
}

static size_t mftscan_expr_position(const MftscanExprParser *parser, const wchar_t *position_text) {
    if (parser == NULL || parser->source_text == NULL || position_text == NULL || position_text < parser->source_text) {
        return 1;
    }

    return (size_t)(position_text - parser->source_text) + 1U;
}

static bool mftscan_expr_fail(MftscanExprParser *parser, const wchar_t *position_text, const char *format_text, ...) {
    va_list argument_list;
    char message_text[220] = { 0 };

    parser->error_text = position_text;
    va_start(argument_list, format_text);
    (void)vsnprintf(message_text, sizeof(message_text), format_text, argument_list);
    va_end(argument_list);
    mftscan_set_error_detail("--min-size 表达式错误（位置 %zu）：%s", mftscan_expr_position(parser, position_text), message_text);
    return false;
}

static bool mftscan_expr_parse_expression(MftscanExprParser *parser, double *value);

static bool mftscan_expr_parse_number(MftscanExprParser *parser, double *value) {
    wchar_t *end_text = NULL;
    double parsed_value = 0.0;

    errno = 0;
    parsed_value = wcstod(parser->cursor_text, &end_text);
    if (end_text == parser->cursor_text) {
        return mftscan_expr_fail(parser, parser->cursor_text, "缺少数字");
    }
    if (errno == ERANGE || !isfinite(parsed_value)) {
        return mftscan_expr_fail(parser, parser->cursor_text, "数字超出范围");
    }

    parser->cursor_text = end_text;
    *value = parsed_value;
    return true;
}

static bool mftscan_expr_parse_factor(MftscanExprParser *parser, double *value) {
    double inner_value = 0.0;

    mftscan_expr_skip_spaces(parser);
    if (*parser->cursor_text == L'\0') {
        return mftscan_expr_fail(parser, parser->cursor_text, "表达式意外结束");
    }

    if (*parser->cursor_text == L'+') {
        parser->cursor_text += 1;
        return mftscan_expr_parse_factor(parser, value);
    }

    if (*parser->cursor_text == L'-') {
        parser->cursor_text += 1;
        if (!mftscan_expr_parse_factor(parser, &inner_value)) {
            return false;
        }
        *value = -inner_value;
        return true;
    }

    if (*parser->cursor_text == L'(') {
        parser->cursor_text += 1;
        if (!mftscan_expr_parse_expression(parser, &inner_value)) {
            return false;
        }
        mftscan_expr_skip_spaces(parser);
        if (*parser->cursor_text != L')') {
            return mftscan_expr_fail(parser, parser->cursor_text, "缺少右括号 ')'");
        }
        parser->cursor_text += 1;
        *value = inner_value;
        return true;
    }

    return mftscan_expr_parse_number(parser, value);
}

static bool mftscan_expr_parse_term(MftscanExprParser *parser, double *value) {
    double current_value = 0.0;

    if (!mftscan_expr_parse_factor(parser, &current_value)) {
        return false;
    }

    while (true) {
        wchar_t operator_char = L'\0';
        double right_value = 0.0;

        mftscan_expr_skip_spaces(parser);
        operator_char = *parser->cursor_text;
        if (operator_char != L'*' && operator_char != L'/') {
            break;
        }

        parser->cursor_text += 1;
        if (!mftscan_expr_parse_factor(parser, &right_value)) {
            return false;
        }

        if (operator_char == L'*') {
            current_value *= right_value;
        } else {
            if (right_value == 0.0) {
                return mftscan_expr_fail(parser, parser->cursor_text - 1, "除数不能为 0");
            }
            current_value /= right_value;
        }

        if (!isfinite(current_value)) {
            return mftscan_expr_fail(parser, parser->cursor_text, "计算结果超出范围");
        }
    }

    *value = current_value;
    return true;
}

static bool mftscan_expr_parse_expression(MftscanExprParser *parser, double *value) {
    double current_value = 0.0;

    if (!mftscan_expr_parse_term(parser, &current_value)) {
        return false;
    }

    while (true) {
        wchar_t operator_char = L'\0';
        double right_value = 0.0;

        mftscan_expr_skip_spaces(parser);
        operator_char = *parser->cursor_text;
        if (operator_char != L'+' && operator_char != L'-') {
            break;
        }

        parser->cursor_text += 1;
        if (!mftscan_expr_parse_term(parser, &right_value)) {
            return false;
        }

        if (operator_char == L'+') {
            current_value += right_value;
        } else {
            current_value -= right_value;
        }

        if (!isfinite(current_value)) {
            return mftscan_expr_fail(parser, parser->cursor_text, "计算结果超出范围");
        }
    }

    *value = current_value;
    return true;
}

static bool mftscan_parse_size_expression(const wchar_t *text, uint64_t *value) {
    MftscanExprParser parser = { 0 };
    double parsed_value = 0.0;
    double rounded_value = 0.0;

    if (text == NULL || value == NULL) {
        mftscan_set_error_detail("--min-size 不能为空");
        return false;
    }

    parser.source_text = text;
    parser.cursor_text = text;
    parser.error_text = NULL;

    mftscan_expr_skip_spaces(&parser);
    if (*parser.cursor_text == L'\0') {
        mftscan_set_error_detail("--min-size 表达式不能为空");
        return false;
    }

    if (!mftscan_expr_parse_expression(&parser, &parsed_value)) {
        return false;
    }

    mftscan_expr_skip_spaces(&parser);
    if (*parser.cursor_text != L'\0') {
        return mftscan_expr_fail(&parser, parser.cursor_text, "存在无法识别的字符");
    }

    if (!isfinite(parsed_value)) {
        mftscan_set_error_detail("--min-size 表达式结果不是有限数值");
        return false;
    }

    if (parsed_value < 0.0) {
        mftscan_set_error_detail("--min-size 表达式结果不能为负数");
        return false;
    }

    rounded_value = ceil(parsed_value);
    if (rounded_value > (double)UINT64_MAX) {
        mftscan_set_error_detail("--min-size 表达式结果超出 uint64 范围");
        return false;
    }

    *value = (uint64_t)rounded_value;
    return true;
}

static bool mftscan_is_ascii_drive_letter(wchar_t character) {
    return (character >= L'a' && character <= L'z') || (character >= L'A' && character <= L'Z');
}

static bool mftscan_is_path_separator(wchar_t character) {
    return character == L'\\' || character == L'/';
}

static bool mftscan_is_drive_designator(const wchar_t *path_text) {
    return path_text != NULL &&
        wcslen(path_text) == 2U &&
        mftscan_is_ascii_drive_letter(path_text[0]) &&
        path_text[1] == L':';
}

static bool mftscan_is_drive_root_path(const wchar_t *path_text) {
    return path_text != NULL &&
        wcslen(path_text) == 3U &&
        mftscan_is_ascii_drive_letter(path_text[0]) &&
        path_text[1] == L':' &&
        mftscan_is_path_separator(path_text[2]);
}

static void mftscan_normalize_path_separators(wchar_t *path_text) {
    if (path_text == NULL) {
        return;
    }

    while (*path_text != L'\0') {
        if (*path_text == L'/') {
            *path_text = L'\\';
        }
        path_text += 1;
    }
}

static void mftscan_trim_trailing_separators(wchar_t *path_text) {
    size_t length = 0;

    if (path_text == NULL) {
        return;
    }

    length = wcslen(path_text);
    while (length > 0U && mftscan_is_path_separator(path_text[length - 1U])) {
        if (length == 3U && mftscan_is_drive_root_path(path_text)) {
            break;
        }
        path_text[--length] = L'\0';
    }
}

static wchar_t *mftscan_alloc_drive_root_path(wchar_t drive_letter) {
    wchar_t *path_text = (wchar_t *)calloc(4U, sizeof(wchar_t));

    if (path_text == NULL) {
        return NULL;
    }

    path_text[0] = (wchar_t)towupper(drive_letter);
    path_text[1] = L':';
    path_text[2] = L'\\';
    path_text[3] = L'\0';
    return path_text;
}

static bool mftscan_extract_drive_root(const wchar_t *path_text, wchar_t *drive_root, size_t drive_root_capacity) {
    if (path_text == NULL || drive_root == NULL || drive_root_capacity < 4U) {
        return false;
    }

    if (wcslen(path_text) < 3U) {
        return false;
    }

    if (!mftscan_is_ascii_drive_letter(path_text[0]) || path_text[1] != L':' || !mftscan_is_path_separator(path_text[2])) {
        return false;
    }

    drive_root[0] = (wchar_t)towupper(path_text[0]);
    drive_root[1] = L':';
    drive_root[2] = L'\\';
    drive_root[3] = L'\0';
    return true;
}

static wchar_t *mftscan_get_full_path(const wchar_t *path_text) {
    DWORD required_length = 0;
    wchar_t *full_path = NULL;
    DWORD actual_length = 0;

    required_length = GetFullPathNameW(path_text, 0, NULL, NULL);
    if (required_length == 0U) {
        return NULL;
    }

    full_path = (wchar_t *)calloc((size_t)required_length, sizeof(wchar_t));
    if (full_path == NULL) {
        return NULL;
    }

    actual_length = GetFullPathNameW(path_text, required_length, full_path, NULL);
    if (actual_length == 0U || actual_length >= required_length) {
        free(full_path);
        return NULL;
    }

    return full_path;
}

static bool mftscan_is_volume_mount_point_path(const wchar_t *path_text) {
    wchar_t *mount_point_path = NULL;
    wchar_t volume_name[MAX_PATH] = { 0 };
    size_t path_length = 0;
    bool is_mount_point = false;

    if (path_text == NULL) {
        return false;
    }

    path_length = wcslen(path_text);
    mount_point_path = (wchar_t *)calloc(path_length + 2U, sizeof(wchar_t));
    if (mount_point_path == NULL) {
        return false;
    }

    memcpy(mount_point_path, path_text, path_length * sizeof(wchar_t));
    if (path_length == 0U || !mftscan_is_path_separator(mount_point_path[path_length - 1U])) {
        mount_point_path[path_length++] = L'\\';
    }
    mount_point_path[path_length] = L'\0';

    is_mount_point = GetVolumeNameForVolumeMountPointW(mount_point_path, volume_name, ARRAYSIZE(volume_name)) != 0;
    free(mount_point_path);
    return is_mount_point;
}

static MftscanError mftscan_parse_location_option(const wchar_t *location_text, MftscanOptions *options) {
    wchar_t *full_path = NULL;
    wchar_t volume_root[MAX_PATH] = { 0 };
    DWORD attributes = INVALID_FILE_ATTRIBUTES;

    if (location_text == NULL || location_text[0] == L'\0') {
        mftscan_set_error_detail("--location 不能为空");
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (mftscan_is_drive_designator(location_text) || mftscan_is_drive_root_path(location_text)) {
        options->volume[0] = (wchar_t)towupper(location_text[0]);
        options->volume[1] = L':';
        options->volume[2] = L'\0';
        options->location = mftscan_alloc_drive_root_path(location_text[0]);
        if (options->location == NULL) {
            return MFTSCAN_ERROR_OUT_OF_MEMORY;
        }
        options->filter_by_location = false;
        return MFTSCAN_OK;
    }

    full_path = mftscan_get_full_path(location_text);
    if (full_path == NULL) {
        mftscan_set_error_detail("--location 无法解析为有效路径");
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    mftscan_normalize_path_separators(full_path);
    mftscan_trim_trailing_separators(full_path);

    attributes = GetFileAttributesW(full_path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        free(full_path);
        mftscan_set_error_detail("--location 指定路径不存在或无法访问");
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
        free(full_path);
        mftscan_set_error_detail("--location 必须是盘符或文件夹路径");
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (!GetVolumePathNameW(full_path, volume_root, ARRAYSIZE(volume_root))) {
        free(full_path);
        mftscan_set_error_detail("--location 无法确定所属盘符");
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    mftscan_normalize_path_separators(volume_root);
    mftscan_trim_trailing_separators(volume_root);
    if (!mftscan_is_drive_root_path(volume_root)) {
        if (mftscan_is_volume_mount_point_path(volume_root)) {
            free(full_path);
            mftscan_set_error_detail("--location 暂不支持挂载到文件夹的卷，请改用对应盘符");
            return MFTSCAN_ERROR_INVALID_ARGUMENT;
        }

        if (!mftscan_extract_drive_root(full_path, volume_root, ARRAYSIZE(volume_root))) {
            free(full_path);
            mftscan_set_error_detail("--location 必须位于类似 C: 的本地盘符");
            return MFTSCAN_ERROR_INVALID_ARGUMENT;
        }
    }

    options->volume[0] = (wchar_t)towupper(volume_root[0]);
    options->volume[1] = L':';
    options->volume[2] = L'\0';
    options->location = mftscan_strdup_w(full_path);
    if (options->location == NULL) {
        free(full_path);
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    if (!mftscan_is_drive_root_path(full_path)) {
        options->filter_root = full_path;
        options->filter_by_location = true;
        return MFTSCAN_OK;
    }

    free(full_path);
    options->filter_by_location = false;
    return MFTSCAN_OK;
}

void mftscan_print_help(FILE *stream) {
    fprintf(
        stream,
        "用法:\n"
        "  folder-size-ranker-cli.exe --location <path> --sort <logical|allocated> [--min-size expr] [--format <table|json>] [--limit N]\n"
        "  folder-size-ranker-cli.exe --location <path> --sort <logical|allocated> --all [--min-size expr] [--limit N]\n\n"
        "说明:\n"
        "  - NTFS 卷直接读取 MFT，其他文件系统使用平台 API 降级扫描\n"
        "  - --location 支持盘符根目录或具体文件夹路径\n"
        "  - 指向 NTFS 子目录时，仍会扫描整卷 MFT，再只输出该目录子树\n"
        "  - --volume 已废弃，请改用 --location\n"
        "  - 默认只输出没有子文件夹的文件夹\n"
        "  - --all 输出指定位置下所有层级目录的紧凑 JSON 树，且不接受 --format\n"
        "  - --all 模式下 --limit 表示每层最多输出的直接子目录数量\n"
        "  - NTFS MFT 路径需要管理员权限，平台 API 降级路径不做管理员前置要求\n");
}

MftscanError mftscan_parse_options(int argc, wchar_t **argv, MftscanOptions *options, bool *show_help) {
    int index = 0;
    bool has_location = false;
    bool has_sort = false;
    bool has_format = false;
    bool has_all = false;

    if (options == NULL || show_help == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    memset(options, 0, sizeof(*options));
    options->format = MFTSCAN_FORMAT_TABLE;
    *show_help = false;
    mftscan_clear_error_detail();

    for (index = 1; index < argc; ++index) {
        const wchar_t *argument = argv[index];

        if (_wcsicmp(argument, L"--help") == 0 || _wcsicmp(argument, L"-h") == 0) {
            *show_help = true;
            return MFTSCAN_OK;
        }

        if (_wcsicmp(argument, L"--volume") == 0) {
            mftscan_set_error_detail("--volume 已废弃，请改用 --location");
            return MFTSCAN_ERROR_INVALID_ARGUMENT;
        } else if (_wcsicmp(argument, L"--location") == 0) {
            MftscanError location_error = MFTSCAN_OK;

            if ((index + 1) >= argc) {
                mftscan_set_error_detail("--location 缺少参数值");
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }
            if (has_location) {
                mftscan_set_error_detail("不能重复提供 --location");
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }

            location_error = mftscan_parse_location_option(argv[++index], options);
            if (location_error != MFTSCAN_OK) {
                return location_error;
            }
            has_location = true;
        } else if (_wcsicmp(argument, L"--sort") == 0) {
            const wchar_t *sort_text = NULL;

            if ((index + 1) >= argc) {
                mftscan_set_error_detail("--sort 缺少参数值");
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }
            sort_text = argv[++index];
            if (_wcsicmp(sort_text, L"logical") == 0) {
                options->sort_mode = MFTSCAN_SORT_LOGICAL;
            } else if (_wcsicmp(sort_text, L"allocated") == 0) {
                options->sort_mode = MFTSCAN_SORT_ALLOCATED;
            } else {
                mftscan_set_error_detail("--sort 只能是 logical 或 allocated");
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }
            has_sort = true;
        } else if (_wcsicmp(argument, L"--min-size") == 0) {
            if ((index + 1) >= argc) {
                mftscan_set_error_detail("--min-size 缺少参数值");
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }
            if (!mftscan_parse_size_expression(argv[++index], &options->min_size)) {
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }
        } else if (_wcsicmp(argument, L"--format") == 0) {
            const wchar_t *format_text = NULL;

            if ((index + 1) >= argc) {
                mftscan_set_error_detail("--format 缺少参数值");
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }
            if (has_format) {
                mftscan_set_error_detail("不能重复提供 --format");
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }
            format_text = argv[++index];
            if (_wcsicmp(format_text, L"table") == 0) {
                options->format = MFTSCAN_FORMAT_TABLE;
            } else if (_wcsicmp(format_text, L"json") == 0) {
                options->format = MFTSCAN_FORMAT_JSON;
            } else {
                mftscan_set_error_detail("--format 只能是 table 或 json");
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }
            has_format = true;
        } else if (_wcsicmp(argument, L"--limit") == 0) {
            uint64_t parsed_limit = 0;

            if ((index + 1) >= argc) {
                mftscan_set_error_detail("--limit 缺少参数值");
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }
            if (!mftscan_parse_uint64_text(argv[++index], &parsed_limit)) {
                mftscan_set_error_detail("--limit 必须是非负整数");
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }
            options->limit = (size_t)parsed_limit;
            options->has_limit = true;
        } else if (_wcsicmp(argument, L"--all") == 0) {
            if (has_all) {
                mftscan_set_error_detail("不能重复提供 --all");
                return MFTSCAN_ERROR_INVALID_ARGUMENT;
            }
            options->output_all = true;
            options->format = MFTSCAN_FORMAT_JSON;
            has_all = true;
        } else {
            mftscan_set_error_detail("无法识别的参数: 请检查 %ls", argument);
            return MFTSCAN_ERROR_INVALID_ARGUMENT;
        }
    }

    if (!has_location || !has_sort) {
        mftscan_set_error_detail("必须同时提供 --location 和 --sort");
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (options->output_all && has_format) {
        mftscan_set_error_detail("--all 固定输出 JSON，不接受 --format");
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    return MFTSCAN_OK;
}
