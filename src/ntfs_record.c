#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "model.h"

#define MFTSCAN_FILE_RECORD_SIGNATURE 0x454c4946UL
#define MFTSCAN_ATTRIBUTE_END 0xffffffffUL
#define MFTSCAN_ATTRIBUTE_ATTRIBUTE_LIST 0x20UL
#define MFTSCAN_ATTRIBUTE_FILE_NAME 0x30UL
#define MFTSCAN_ATTRIBUTE_DATA 0x80UL
#define MFTSCAN_ATTRIBUTE_INDEX_ALLOCATION 0xa0UL
#define MFTSCAN_ATTRIBUTE_BITMAP 0xb0UL
#define MFTSCAN_ATTRIBUTE_LOGGED_UTILITY_STREAM 0x100UL
#define MFTSCAN_ATTRIBUTE_FLAG_COMPRESSED 0x0001U
#define MFTSCAN_ATTRIBUTE_FLAG_SPARSE 0x8000U
#define MFTSCAN_FILE_NAME_NAMESPACE_DOS 2U

#pragma pack(push, 1)
typedef struct MftscanNtfsFileRecordHeader {
    DWORD signature;
    WORD update_sequence_offset;
    WORD update_sequence_count;
    ULONGLONG log_file_sequence_number;
    WORD sequence_number;
    WORD hard_link_count;
    WORD first_attribute_offset;
    WORD flags;
    DWORD used_size;
    DWORD allocated_size;
    ULONGLONG base_file_record;
    WORD next_attribute_number;
    WORD reserved;
    DWORD record_number;
} MftscanNtfsFileRecordHeader;

typedef struct MftscanNtfsAttributeHeader {
    DWORD type;
    DWORD length;
    BYTE non_resident;
    BYTE name_length;
    WORD name_offset;
    WORD flags;
    WORD attribute_number;
} MftscanNtfsAttributeHeader;

typedef struct MftscanNtfsResidentAttributeHeader {
    MftscanNtfsAttributeHeader header;
    DWORD value_length;
    WORD value_offset;
    BYTE resident_flags;
    BYTE reserved;
} MftscanNtfsResidentAttributeHeader;

typedef struct MftscanNtfsNonResidentAttributeHeader {
    MftscanNtfsAttributeHeader header;
    ULONGLONG lowest_vcn;
    ULONGLONG highest_vcn;
    WORD mapping_pairs_offset;
    BYTE compression_unit;
    BYTE reserved[5];
    ULONGLONG allocated_size;
    ULONGLONG data_size;
    ULONGLONG initialized_size;
} MftscanNtfsNonResidentAttributeHeader;

typedef struct MftscanNtfsFileNameAttribute {
    ULONGLONG parent_directory;
    LONGLONG creation_time;
    LONGLONG last_modification_time;
    LONGLONG last_mft_modification_time;
    LONGLONG last_access_time;
    ULONGLONG allocated_size;
    ULONGLONG real_size;
    DWORD file_attributes;
    DWORD extended_attribute_size_or_reparse_tag;
    BYTE file_name_length;
    BYTE file_name_type;
    WCHAR file_name[1];
} MftscanNtfsFileNameAttribute;

typedef struct MftscanNtfsAttributeListEntry {
    DWORD type;
    WORD record_length;
    BYTE name_length;
    BYTE name_offset;
    ULONGLONG lowest_vcn;
    ULONGLONG segment_reference;
    WORD attribute_id;
    WCHAR attribute_name[1];
} MftscanNtfsAttributeListEntry;
#pragma pack(pop)

typedef struct MftscanNtfsDataSizeCandidate {
    uint64_t logical_size;
    uint64_t allocated_size;
    bool present;
} MftscanNtfsDataSizeCandidate;

typedef struct MftscanNtfsParseState {
    MftscanNtfsDataSizeCandidate file_name_size;
    MftscanNtfsDataSizeCandidate direct_data_size;
    MftscanNtfsDataSizeCandidate wof_backing_size;
    MftscanNtfsDataSizeCandidate fallback_stream_size;
    MftscanNtfsDataSizeCandidate directory_self_size;
    uint8_t *attribute_list_data;
    size_t attribute_list_length;
    bool has_attribute_list;
} MftscanNtfsParseState;

static uint8_t mftscan_name_priority(uint8_t namespace_type) {
    switch (namespace_type) {
    case 3:
        return 4;
    case 1:
        return 3;
    case 0:
        return 2;
    case 2:
        return 1;
    default:
        return 0;
    }
}

static void mftscan_parse_state_free(MftscanNtfsParseState *parse_state) {
    if (parse_state == NULL) {
        return;
    }

    free(parse_state->attribute_list_data);
    memset(parse_state, 0, sizeof(*parse_state));
}

static void mftscan_set_data_size_candidate(
    MftscanNtfsDataSizeCandidate *candidate,
    uint64_t logical_size,
    uint64_t allocated_size) {
    if (candidate == NULL) {
        return;
    }

    candidate->logical_size = logical_size;
    candidate->allocated_size = allocated_size;
    candidate->present = true;
}

static bool mftscan_add_data_size_candidate(
    MftscanNtfsDataSizeCandidate *candidate,
    const MftscanNtfsDataSizeCandidate *addition) {
    if (candidate == NULL || addition == NULL || !addition->present) {
        return true;
    }
    if (candidate->logical_size > UINT64_MAX - addition->logical_size ||
        candidate->allocated_size > UINT64_MAX - addition->allocated_size) {
        return false;
    }

    candidate->logical_size += addition->logical_size;
    candidate->allocated_size += addition->allocated_size;
    candidate->present = true;
    return true;
}

static bool mftscan_is_metadata_tree_file_fallback_attribute(const MftscanNtfsAttributeHeader *attribute_header) {
    if (attribute_header == NULL) {
        return false;
    }

    if (attribute_header->type == MFTSCAN_ATTRIBUTE_DATA) {
        return attribute_header->name_length != 0U;
    }

    return attribute_header->type == MFTSCAN_ATTRIBUTE_INDEX_ALLOCATION ||
        attribute_header->type == MFTSCAN_ATTRIBUTE_BITMAP ||
        attribute_header->type == MFTSCAN_ATTRIBUTE_LOGGED_UTILITY_STREAM;
}

static bool mftscan_is_metadata_tree_directory_self_attribute(const MftscanNtfsAttributeHeader *attribute_header) {
    if (attribute_header == NULL) {
        return false;
    }

    return attribute_header->type == MFTSCAN_ATTRIBUTE_INDEX_ALLOCATION ||
        attribute_header->type == MFTSCAN_ATTRIBUTE_BITMAP;
}

MftscanError mftscan_apply_update_sequence_array(
    uint8_t *record_buffer,
    size_t record_length,
    uint32_t bytes_per_sector,
    bool *torn_write) {
    MftscanNtfsFileRecordHeader *header = (MftscanNtfsFileRecordHeader *)record_buffer;
    WORD *update_sequence_words = NULL;
    WORD update_sequence_number = 0;
    WORD sector_index = 0;
    WORD expected_count = 0;

    if (torn_write != NULL) {
        *torn_write = false;
    }
    if (record_length < sizeof(MftscanNtfsFileRecordHeader) || bytes_per_sector < sizeof(WORD) * 2U) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    if (header->update_sequence_offset < sizeof(MftscanNtfsFileRecordHeader) ||
        header->update_sequence_count == 0U ||
        header->update_sequence_offset + header->update_sequence_count * sizeof(WORD) > record_length) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    expected_count = (WORD)((record_length / bytes_per_sector) + 1U);
    if (header->update_sequence_count != expected_count) {
        mftscan_set_error_detail(
            "USA 计数 %u 与扇区数不一致 (期望 %u)",
            (unsigned int)header->update_sequence_count,
            (unsigned int)expected_count);
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    update_sequence_words = (WORD *)(record_buffer + header->update_sequence_offset);
    update_sequence_number = update_sequence_words[0];

    for (sector_index = 1; sector_index < header->update_sequence_count; ++sector_index) {
        size_t fixup_offset = (size_t)sector_index * bytes_per_sector - sizeof(WORD);
        WORD *sector_trailer = NULL;
        if (fixup_offset + sizeof(WORD) > record_length) {
            return MFTSCAN_ERROR_MFT_PARSE;
        }

        sector_trailer = (WORD *)(record_buffer + fixup_offset);
        if (*sector_trailer != update_sequence_number) {
            if (torn_write != NULL) {
                *torn_write = true;
            }
            mftscan_set_error_detail(
                "USA 校验失败：扇区 %u 尾部 0x%04x 与 USN 0x%04x 不一致",
                (unsigned int)sector_index,
                (unsigned int)*sector_trailer,
                (unsigned int)update_sequence_number);
            return MFTSCAN_ERROR_MFT_PARSE;
        }

        *sector_trailer = update_sequence_words[sector_index];
    }

    return MFTSCAN_OK;
}

static bool mftscan_record_layout_is_valid(const MftscanNtfsFileRecordHeader *header, size_t record_length) {
    if (header == NULL || record_length < sizeof(MftscanNtfsFileRecordHeader)) {
        return false;
    }

    if ((header->first_attribute_offset & 0x07U) != 0U) {
        return false;
    }
    if (header->first_attribute_offset < sizeof(MftscanNtfsFileRecordHeader) ||
        header->first_attribute_offset > header->used_size ||
        header->used_size > record_length) {
        return false;
    }
    if (header->allocated_size > record_length) {
        return false;
    }
    if (header->used_size < (DWORD)header->first_attribute_offset + sizeof(DWORD) * 2U) {
        return false;
    }

    return true;
}

static bool mftscan_attribute_name_is_valid(
    const MftscanNtfsAttributeHeader *attribute_header,
    size_t attribute_length) {
    size_t name_size_bytes = 0;

    if (attribute_header == NULL) {
        return false;
    }

    if (attribute_header->name_length == 0U) {
        return true;
    }

    name_size_bytes = (size_t)attribute_header->name_length * sizeof(WCHAR);
    return attribute_header->name_offset >= sizeof(MftscanNtfsAttributeHeader) &&
        attribute_header->name_offset + name_size_bytes <= attribute_length;
}

static bool mftscan_attribute_name_matches_list_entry(
    const MftscanNtfsAttributeHeader *attribute_header,
    size_t attribute_length,
    const MftscanNtfsAttributeListEntry *entry) {
    size_t name_size_bytes = 0;
    const uint8_t *attribute_name = NULL;
    const uint8_t *entry_name = NULL;

    if (attribute_header == NULL || entry == NULL) {
        return false;
    }

    if (!mftscan_attribute_name_is_valid(attribute_header, attribute_length)) {
        return false;
    }

    if (attribute_header->name_length != entry->name_length) {
        return false;
    }

    if (attribute_header->name_length == 0U) {
        return true;
    }

    name_size_bytes = (size_t)attribute_header->name_length * sizeof(WCHAR);
    attribute_name = (const uint8_t *)attribute_header + attribute_header->name_offset;
    entry_name = (const uint8_t *)entry + entry->name_offset;
    return memcmp(attribute_name, entry_name, name_size_bytes) == 0;
}

static bool mftscan_attribute_name_equals(
    const MftscanNtfsAttributeHeader *attribute_header,
    size_t attribute_length,
    const wchar_t *expected_name) {
    size_t expected_length = 0;
    const wchar_t *attribute_name = NULL;

    if (attribute_header == NULL || expected_name == NULL) {
        return false;
    }
    if (!mftscan_attribute_name_is_valid(attribute_header, attribute_length)) {
        return false;
    }

    expected_length = wcslen(expected_name);
    if (attribute_header->name_length != expected_length) {
        return false;
    }
    if (expected_length == 0U) {
        return true;
    }

    attribute_name = (const wchar_t *)((const uint8_t *)attribute_header + attribute_header->name_offset);
    return _wcsnicmp(attribute_name, expected_name, expected_length) == 0;
}

static bool mftscan_attribute_list_entry_name_equals(
    const MftscanNtfsAttributeListEntry *entry,
    const wchar_t *expected_name) {
    size_t expected_length = 0;
    const wchar_t *entry_name = NULL;

    if (entry == NULL || expected_name == NULL) {
        return false;
    }

    expected_length = wcslen(expected_name);
    if (entry->name_length != expected_length) {
        return false;
    }
    if (expected_length == 0U) {
        return true;
    }

    entry_name = (const wchar_t *)((const uint8_t *)entry + entry->name_offset);
    return _wcsnicmp(entry_name, expected_name, expected_length) == 0;
}

static MftscanError mftscan_add_file_name_link(
    MftscanRecordInfo *record_info,
    uint64_t parent_frn,
    const wchar_t *name,
    size_t name_length,
    uint8_t name_priority) {
    size_t index = 0;
    wchar_t *copied_name = NULL;
    MftscanRecordFileNameLink *grown_links = NULL;

    if (record_info == NULL || name == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }
    if (parent_frn == 0ULL || name_length == 0U) {
        return MFTSCAN_OK;
    }

    for (index = 0; index < record_info->file_name_link_count; ++index) {
        MftscanRecordFileNameLink *link = &record_info->file_name_links[index];

        if (link->parent_frn == parent_frn &&
            wcslen(link->name) == name_length &&
            _wcsnicmp(link->name, name, name_length) == 0) {
            link->name_priority = name_priority;
            return MFTSCAN_OK;
        }
    }

    copied_name = (wchar_t *)malloc((name_length + 1U) * sizeof(wchar_t));
    if (copied_name == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copied_name, name, name_length * sizeof(wchar_t));
    copied_name[name_length] = L'\0';

    grown_links = (MftscanRecordFileNameLink *)mftscan_realloc_array(
        record_info->file_name_links,
        sizeof(MftscanRecordFileNameLink),
        &record_info->file_name_link_capacity,
        record_info->file_name_link_count + 1U);
    if (grown_links == NULL) {
        free(copied_name);
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    record_info->file_name_links = grown_links;
    record_info->file_name_links[record_info->file_name_link_count].parent_frn = parent_frn;
    record_info->file_name_links[record_info->file_name_link_count].name = copied_name;
    record_info->file_name_links[record_info->file_name_link_count].name_priority = name_priority;
    record_info->file_name_link_count += 1U;
    return MFTSCAN_OK;
}

static MftscanError mftscan_capture_file_name(
    const MftscanNtfsResidentAttributeHeader *resident_header,
    size_t attribute_length,
    MftscanRecordInfo *record_info,
    MftscanNtfsDataSizeCandidate *file_name_size) {
    const MftscanNtfsFileNameAttribute *file_name_attribute = NULL;
    uint8_t candidate_priority = 0;
    size_t name_size_bytes = 0;
    wchar_t *candidate_name = NULL;

    if (resident_header == NULL || record_info == NULL || file_name_size == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (resident_header->value_offset + resident_header->value_length > attribute_length ||
        resident_header->value_length < offsetof(MftscanNtfsFileNameAttribute, file_name)) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    file_name_attribute = (const MftscanNtfsFileNameAttribute *)((const uint8_t *)resident_header + resident_header->value_offset);
    name_size_bytes = (size_t)file_name_attribute->file_name_length * sizeof(WCHAR);
    if (resident_header->value_length < offsetof(MftscanNtfsFileNameAttribute, file_name) + name_size_bytes) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    if (!record_info->is_directory) {
        if (!file_name_size->present || file_name_attribute->real_size > file_name_size->logical_size) {
            file_name_size->logical_size = file_name_attribute->real_size;
        }
        if (!file_name_size->present || file_name_attribute->allocated_size > file_name_size->allocated_size) {
            file_name_size->allocated_size = file_name_attribute->allocated_size;
        }
        file_name_size->present = true;
    }
    candidate_priority = mftscan_name_priority(file_name_attribute->file_name_type);
    if (!record_info->is_directory) {
        if (file_name_attribute->file_name_type == MFTSCAN_FILE_NAME_NAMESPACE_DOS) {
            return MFTSCAN_OK;
        }
        return mftscan_add_file_name_link(
            record_info,
            file_name_attribute->parent_directory & MFTSCAN_FRN_MASK,
            file_name_attribute->file_name,
            file_name_attribute->file_name_length,
            candidate_priority);
    }

    if (candidate_priority < record_info->name_priority) {
        return MFTSCAN_OK;
    }

    candidate_name = (wchar_t *)malloc(name_size_bytes + sizeof(wchar_t));
    if (candidate_name == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    memcpy(candidate_name, file_name_attribute->file_name, name_size_bytes);
    candidate_name[file_name_attribute->file_name_length] = L'\0';

    free(record_info->name);
    record_info->name = candidate_name;
    record_info->name_priority = candidate_priority;
    record_info->parent_frn = file_name_attribute->parent_directory & MFTSCAN_FRN_MASK;
    return MFTSCAN_OK;
}

static uint64_t mftscan_read_unsigned_le(const uint8_t *bytes, size_t byte_count);
static int64_t mftscan_read_signed_le(const uint8_t *bytes, size_t byte_count);

MftscanError mftscan_iterate_mapping_pairs(
    const uint8_t *mapping_pairs,
    size_t mapping_pairs_length,
    MftscanRunlistCallback callback,
    void *user_data) {
    const uint8_t *cursor = mapping_pairs;
    const uint8_t *end = mapping_pairs + mapping_pairs_length;
    int64_t current_lcn = 0;

    if (mapping_pairs == NULL || callback == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    while (cursor < end) {
        BYTE run_header = *cursor++;
        size_t length_size = 0;
        size_t offset_size = 0;
        uint64_t cluster_count = 0ULL;
        bool sparse = false;
        uint64_t starting_lcn = 0ULL;
        MftscanError error_code = MFTSCAN_OK;

        if (run_header == 0U) {
            return MFTSCAN_OK;
        }

        length_size = run_header & 0x0FU;
        offset_size = run_header >> 4U;
        if (length_size == 0U || length_size > 8U || offset_size > 8U ||
            cursor + length_size + offset_size > end) {
            return MFTSCAN_ERROR_MFT_PARSE;
        }

        cluster_count = mftscan_read_unsigned_le(cursor, length_size);
        cursor += length_size;
        if (cluster_count == 0ULL) {
            return MFTSCAN_ERROR_MFT_PARSE;
        }

        if (offset_size == 0U) {
            sparse = true;
        } else {
            int64_t lcn_delta = mftscan_read_signed_le(cursor, offset_size);
            current_lcn += lcn_delta;
            if (current_lcn < 0) {
                return MFTSCAN_ERROR_MFT_PARSE;
            }
            starting_lcn = (uint64_t)current_lcn;
        }
        cursor += offset_size;

        error_code = callback(user_data, cluster_count, starting_lcn, sparse);
        if (error_code != MFTSCAN_OK) {
            return error_code;
        }
    }

    /* Reached end of mapping pairs buffer without an explicit terminator. */
    return MFTSCAN_ERROR_MFT_PARSE;
}

static MftscanError mftscan_runlist_bounds_for_attribute(
    const MftscanNtfsNonResidentAttributeHeader *non_resident_header,
    size_t attribute_length,
    const uint8_t **mapping_pairs_out,
    size_t *mapping_pairs_length_out) {
    if (non_resident_header == NULL || mapping_pairs_out == NULL || mapping_pairs_length_out == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }
    if (sizeof(MftscanNtfsNonResidentAttributeHeader) > attribute_length ||
        non_resident_header->mapping_pairs_offset < sizeof(MftscanNtfsNonResidentAttributeHeader) ||
        non_resident_header->mapping_pairs_offset >= attribute_length) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }
    *mapping_pairs_out = (const uint8_t *)non_resident_header + non_resident_header->mapping_pairs_offset;
    *mapping_pairs_length_out = attribute_length - non_resident_header->mapping_pairs_offset;
    return MFTSCAN_OK;
}

typedef struct MftscanAllocatedSizeAccumulator {
    uint32_t bytes_per_cluster;
    uint64_t allocated_size;
} MftscanAllocatedSizeAccumulator;

static MftscanError mftscan_accumulate_allocated_size_callback(
    void *user_data,
    uint64_t cluster_count,
    uint64_t starting_lcn,
    bool sparse) {
    MftscanAllocatedSizeAccumulator *accumulator = (MftscanAllocatedSizeAccumulator *)user_data;
    uint64_t run_size = 0ULL;

    (void)starting_lcn;
    if (sparse) {
        return MFTSCAN_OK;
    }
    if (cluster_count > UINT64_MAX / accumulator->bytes_per_cluster) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }
    run_size = cluster_count * accumulator->bytes_per_cluster;
    if (accumulator->allocated_size > UINT64_MAX - run_size) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }
    accumulator->allocated_size += run_size;
    return MFTSCAN_OK;
}

static MftscanError mftscan_calculate_nonresident_allocated_size(
    const MftscanVolumeHandle *volume_handle,
    const MftscanNtfsNonResidentAttributeHeader *non_resident_header,
    size_t attribute_length,
    uint64_t *allocated_size) {
    const uint8_t *mapping_pairs = NULL;
    size_t mapping_pairs_length = 0;
    MftscanAllocatedSizeAccumulator accumulator = { 0 };
    MftscanError error_code = MFTSCAN_OK;

    if (volume_handle == NULL || allocated_size == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }
    if (volume_handle->bytes_per_cluster == 0U) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    error_code = mftscan_runlist_bounds_for_attribute(
        non_resident_header, attribute_length, &mapping_pairs, &mapping_pairs_length);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    accumulator.bytes_per_cluster = volume_handle->bytes_per_cluster;
    error_code = mftscan_iterate_mapping_pairs(
        mapping_pairs, mapping_pairs_length,
        mftscan_accumulate_allocated_size_callback, &accumulator);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    *allocated_size = accumulator.allocated_size;
    return MFTSCAN_OK;
}

static bool mftscan_nonresident_attribute_uses_runlist_allocated_size(
    const MftscanNtfsAttributeHeader *attribute_header) {
    if (attribute_header == NULL) {
        return false;
    }

    return (attribute_header->flags &
        (MFTSCAN_ATTRIBUTE_FLAG_SPARSE | MFTSCAN_ATTRIBUTE_FLAG_COMPRESSED)) != 0U;
}

static MftscanError mftscan_capture_data_size_candidate(
    const MftscanVolumeHandle *volume_handle,
    const MftscanNtfsAttributeHeader *attribute_header,
    size_t attribute_length,
    MftscanNtfsDataSizeCandidate *candidate) {
    if (attribute_header == NULL || candidate == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (!mftscan_attribute_name_is_valid(attribute_header, attribute_length)) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    if (attribute_header->name_length != 0U) {
        return MFTSCAN_OK;
    }

    if (attribute_header->non_resident == 0U) {
        const MftscanNtfsResidentAttributeHeader *resident_header =
            (const MftscanNtfsResidentAttributeHeader *)attribute_header;

        if (sizeof(MftscanNtfsResidentAttributeHeader) > attribute_length ||
            resident_header->value_offset + resident_header->value_length > attribute_length) {
            return MFTSCAN_ERROR_MFT_PARSE;
        }

        mftscan_set_data_size_candidate(candidate, resident_header->value_length, 0ULL);
        return MFTSCAN_OK;
    }

    {
        const MftscanNtfsNonResidentAttributeHeader *non_resident_header =
            (const MftscanNtfsNonResidentAttributeHeader *)attribute_header;
        uint64_t allocated_size = 0ULL;

        if (volume_handle == NULL || sizeof(MftscanNtfsNonResidentAttributeHeader) > attribute_length) {
            return MFTSCAN_ERROR_MFT_PARSE;
        }

        if (non_resident_header->lowest_vcn != 0ULL) {
            return MFTSCAN_OK;
        }

        if (mftscan_nonresident_attribute_uses_runlist_allocated_size(&non_resident_header->header)) {
            MftscanError error_code = mftscan_calculate_nonresident_allocated_size(
                volume_handle,
                non_resident_header,
                attribute_length,
                &allocated_size);
            if (error_code != MFTSCAN_OK) {
                return error_code;
            }
        } else {
            allocated_size = non_resident_header->allocated_size;
        }

        mftscan_set_data_size_candidate(candidate, non_resident_header->data_size, allocated_size);
        return MFTSCAN_OK;
    }
}

static MftscanError mftscan_capture_data_size_fragment(
    const MftscanVolumeHandle *volume_handle,
    const MftscanNtfsAttributeHeader *attribute_header,
    size_t attribute_length,
    MftscanNtfsDataSizeCandidate *candidate) {
    if (attribute_header == NULL || candidate == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (!mftscan_attribute_name_is_valid(attribute_header, attribute_length)) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    if (attribute_header->name_length != 0U) {
        return MFTSCAN_OK;
    }

    if (attribute_header->non_resident == 0U) {
        return mftscan_capture_data_size_candidate(volume_handle, attribute_header, attribute_length, candidate);
    }

    {
        const MftscanNtfsNonResidentAttributeHeader *non_resident_header =
            (const MftscanNtfsNonResidentAttributeHeader *)attribute_header;
        uint64_t allocated_size = 0ULL;
        uint64_t logical_size = 0ULL;

        if (volume_handle == NULL || sizeof(MftscanNtfsNonResidentAttributeHeader) > attribute_length) {
            return MFTSCAN_ERROR_MFT_PARSE;
        }

        if (mftscan_nonresident_attribute_uses_runlist_allocated_size(&non_resident_header->header)) {
            MftscanError error_code = mftscan_calculate_nonresident_allocated_size(
                volume_handle,
                non_resident_header,
                attribute_length,
                &allocated_size);
            if (error_code != MFTSCAN_OK) {
                return error_code;
            }
        } else if (non_resident_header->lowest_vcn == 0ULL) {
            allocated_size = non_resident_header->allocated_size;
        }

        if (non_resident_header->lowest_vcn == 0ULL) {
            logical_size = non_resident_header->data_size;
        }
        mftscan_set_data_size_candidate(candidate, logical_size, allocated_size);
        return MFTSCAN_OK;
    }
}

static MftscanError mftscan_capture_storage_size_fragment(
    const MftscanVolumeHandle *volume_handle,
    const MftscanNtfsAttributeHeader *attribute_header,
    size_t attribute_length,
    bool require_unnamed,
    MftscanNtfsDataSizeCandidate *candidate) {
    if (attribute_header == NULL || candidate == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (!mftscan_attribute_name_is_valid(attribute_header, attribute_length)) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    if (require_unnamed && attribute_header->name_length != 0U) {
        return MFTSCAN_OK;
    }

    if (attribute_header->non_resident == 0U) {
        const MftscanNtfsResidentAttributeHeader *resident_header =
            (const MftscanNtfsResidentAttributeHeader *)attribute_header;

        if (sizeof(MftscanNtfsResidentAttributeHeader) > attribute_length ||
            resident_header->value_offset + resident_header->value_length > attribute_length) {
            return MFTSCAN_ERROR_MFT_PARSE;
        }

        mftscan_set_data_size_candidate(candidate, resident_header->value_length, 0ULL);
        return MFTSCAN_OK;
    }

    {
        const MftscanNtfsNonResidentAttributeHeader *non_resident_header =
            (const MftscanNtfsNonResidentAttributeHeader *)attribute_header;
        uint64_t allocated_size = 0ULL;
        uint64_t logical_size = 0ULL;

        if (volume_handle == NULL || sizeof(MftscanNtfsNonResidentAttributeHeader) > attribute_length) {
            return MFTSCAN_ERROR_MFT_PARSE;
        }

        if (mftscan_nonresident_attribute_uses_runlist_allocated_size(&non_resident_header->header)) {
            MftscanError error_code = mftscan_calculate_nonresident_allocated_size(
                volume_handle,
                non_resident_header,
                attribute_length,
                &allocated_size);
            if (error_code != MFTSCAN_OK) {
                return error_code;
            }
        } else if (non_resident_header->lowest_vcn == 0ULL) {
            allocated_size = non_resident_header->allocated_size;
        }

        if (non_resident_header->lowest_vcn == 0ULL) {
            logical_size = non_resident_header->data_size;
        }
        mftscan_set_data_size_candidate(candidate, logical_size, allocated_size);
        return MFTSCAN_OK;
    }
}

static MftscanError mftscan_copy_resident_attribute_value(
    const MftscanNtfsResidentAttributeHeader *resident_header,
    size_t attribute_length,
    uint8_t **value_data,
    size_t *value_length) {
    uint8_t *copied_value = NULL;

    if (resident_header == NULL || value_data == NULL || value_length == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (sizeof(MftscanNtfsResidentAttributeHeader) > attribute_length ||
        resident_header->value_offset + resident_header->value_length > attribute_length) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    *value_data = NULL;
    *value_length = resident_header->value_length;
    if (resident_header->value_length == 0U) {
        return MFTSCAN_OK;
    }

    copied_value = (uint8_t *)malloc(resident_header->value_length);
    if (copied_value == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    memcpy(copied_value, (const uint8_t *)resident_header + resident_header->value_offset, resident_header->value_length);
    *value_data = copied_value;
    return MFTSCAN_OK;
}

static uint64_t mftscan_read_unsigned_le(const uint8_t *bytes, size_t byte_count) {
    uint64_t value = 0ULL;
    size_t index = 0;

    for (index = 0; index < byte_count; ++index) {
        value |= (uint64_t)bytes[index] << (index * 8U);
    }

    return value;
}

static int64_t mftscan_read_signed_le(const uint8_t *bytes, size_t byte_count) {
    uint64_t value = mftscan_read_unsigned_le(bytes, byte_count);

    if (byte_count > 0U && byte_count < sizeof(uint64_t) && (bytes[byte_count - 1U] & 0x80U) != 0U) {
        value |= (~0ULL) << (byte_count * 8U);
    }

    return (int64_t)value;
}

typedef struct MftscanCopyValueState {
    const MftscanVolumeHandle *volume_handle;
    uint8_t *output;
    size_t output_capacity;
    size_t output_offset;
    uint8_t *cluster_buffer;
} MftscanCopyValueState;

static MftscanError mftscan_copy_value_callback(
    void *user_data,
    uint64_t cluster_count,
    uint64_t starting_lcn,
    bool sparse) {
    MftscanCopyValueState *state = (MftscanCopyValueState *)user_data;
    uint64_t cluster_index = 0ULL;
    uint32_t bytes_per_cluster = state->volume_handle->bytes_per_cluster;

    for (cluster_index = 0ULL;
        cluster_index < cluster_count && state->output_offset < state->output_capacity;
        ++cluster_index) {
        size_t copy_size = bytes_per_cluster;
        if (state->output_capacity - state->output_offset < copy_size) {
            copy_size = state->output_capacity - state->output_offset;
        }

        if (sparse) {
            memset(state->output + state->output_offset, 0, copy_size);
        } else {
            uint64_t absolute_lcn = 0ULL;
            uint64_t byte_offset = 0ULL;
            MftscanError error_code = MFTSCAN_OK;

            if (starting_lcn > UINT64_MAX - cluster_index) {
                return MFTSCAN_ERROR_MFT_PARSE;
            }
            absolute_lcn = starting_lcn + cluster_index;
            if (absolute_lcn > UINT64_MAX / bytes_per_cluster) {
                return MFTSCAN_ERROR_MFT_PARSE;
            }
            byte_offset = absolute_lcn * bytes_per_cluster;

            error_code = mftscan_read_volume_bytes(
                state->volume_handle,
                byte_offset,
                state->cluster_buffer,
                bytes_per_cluster);
            if (error_code != MFTSCAN_OK) {
                return error_code;
            }
            memcpy(state->output + state->output_offset, state->cluster_buffer, copy_size);
        }

        state->output_offset += copy_size;
    }

    return MFTSCAN_OK;
}

static MftscanError mftscan_copy_nonresident_attribute_value(
    const MftscanVolumeHandle *volume_handle,
    const MftscanNtfsNonResidentAttributeHeader *non_resident_header,
    size_t attribute_length,
    uint8_t **value_data,
    size_t *value_length) {
    const uint8_t *mapping_pairs = NULL;
    size_t mapping_pairs_length = 0;
    MftscanCopyValueState state = { 0 };
    uint8_t *copied_value = NULL;
    uint8_t *cluster_buffer = NULL;
    MftscanError error_code = MFTSCAN_OK;

    if (volume_handle == NULL || value_data == NULL || value_length == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }
    if (volume_handle->bytes_per_cluster == 0U) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }
    if (non_resident_header->data_size > SIZE_MAX) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    error_code = mftscan_runlist_bounds_for_attribute(
        non_resident_header, attribute_length, &mapping_pairs, &mapping_pairs_length);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    *value_data = NULL;
    *value_length = (size_t)non_resident_header->data_size;
    if (non_resident_header->data_size == 0ULL) {
        return MFTSCAN_OK;
    }

    copied_value = (uint8_t *)calloc((size_t)non_resident_header->data_size, sizeof(uint8_t));
    if (copied_value == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }
    cluster_buffer = (uint8_t *)malloc(volume_handle->bytes_per_cluster);
    if (cluster_buffer == NULL) {
        free(copied_value);
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    state.volume_handle = volume_handle;
    state.output = copied_value;
    state.output_capacity = (size_t)non_resident_header->data_size;
    state.output_offset = 0;
    state.cluster_buffer = cluster_buffer;

    error_code = mftscan_iterate_mapping_pairs(
        mapping_pairs, mapping_pairs_length,
        mftscan_copy_value_callback, &state);
    if (error_code != MFTSCAN_OK) {
        goto cleanup;
    }
    if (state.output_offset != state.output_capacity) {
        error_code = MFTSCAN_ERROR_MFT_PARSE;
        goto cleanup;
    }

    *value_data = copied_value;
    copied_value = NULL;

cleanup:
    free(cluster_buffer);
    free(copied_value);
    return error_code;
}

static MftscanError mftscan_capture_attribute_list(
    const MftscanVolumeHandle *volume_handle,
    const MftscanNtfsAttributeHeader *attribute_header,
    size_t attribute_length,
    MftscanNtfsParseState *parse_state) {
    uint8_t *attribute_list_data = NULL;
    size_t attribute_list_length = 0;
    MftscanError error_code = MFTSCAN_OK;

    if (volume_handle == NULL || attribute_header == NULL || parse_state == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (parse_state->has_attribute_list) {
        mftscan_set_error_detail("同一记录出现多个 $ATTRIBUTE_LIST");
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    if (!mftscan_attribute_name_is_valid(attribute_header, attribute_length)) {
        mftscan_set_error_detail("$ATTRIBUTE_LIST 名称区域越界");
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    if (attribute_header->name_length != 0U) {
        return MFTSCAN_OK;
    }

    if (attribute_header->non_resident == 0U) {
        error_code = mftscan_copy_resident_attribute_value(
            (const MftscanNtfsResidentAttributeHeader *)attribute_header,
            attribute_length,
            &attribute_list_data,
            &attribute_list_length);
    } else {
        error_code = mftscan_copy_nonresident_attribute_value(
            volume_handle,
            (const MftscanNtfsNonResidentAttributeHeader *)attribute_header,
            attribute_length,
            &attribute_list_data,
            &attribute_list_length);
    }
    if (error_code != MFTSCAN_OK) {
        mftscan_set_error_detail("读取 $ATTRIBUTE_LIST 内容失败");
        return error_code;
    }

    parse_state->attribute_list_data = attribute_list_data;
    parse_state->attribute_list_length = attribute_list_length;
    parse_state->has_attribute_list = true;
    return MFTSCAN_OK;
}

static bool mftscan_attribute_list_entry_is_valid(
    const MftscanNtfsAttributeListEntry *entry,
    size_t remaining_length) {
    size_t minimum_length = offsetof(MftscanNtfsAttributeListEntry, attribute_name);
    size_t name_size_bytes = 0;

    if (entry == NULL || remaining_length < minimum_length) {
        return false;
    }

    if (entry->record_length < minimum_length || entry->record_length > remaining_length) {
        return false;
    }

    if (entry->name_length == 0U) {
        return true;
    }

    name_size_bytes = (size_t)entry->name_length * sizeof(WCHAR);
    return entry->name_offset >= minimum_length &&
        entry->name_offset + name_size_bytes <= entry->record_length;
}

static bool mftscan_bytes_are_zero(const uint8_t *bytes, size_t byte_count) {
    size_t index = 0;

    if (bytes == NULL) {
        return false;
    }

    for (index = 0; index < byte_count; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }

    return true;
}

static MftscanError mftscan_find_attribute_size_in_record(
    const MftscanVolumeHandle *volume_handle,
    uint64_t segment_frn,
    uint64_t base_record_frn,
    uint32_t attribute_type,
    const MftscanNtfsAttributeListEntry *target_entry,
    MftscanNtfsDataSizeCandidate *data_size) {
    NTFS_FILE_RECORD_OUTPUT_BUFFER *output_buffer = NULL;
    MftscanNtfsFileRecordHeader *header = NULL;
    size_t output_buffer_size = 0;
    uint64_t actual_record = 0ULL;
    bool reached_end = false;
    size_t attribute_offset = 0;
    MftscanError error_code = MFTSCAN_OK;

    if (volume_handle == NULL || data_size == NULL || segment_frn == 0ULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    output_buffer_size = offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) + volume_handle->bytes_per_file_record;
    output_buffer = (NTFS_FILE_RECORD_OUTPUT_BUFFER *)malloc(output_buffer_size);
    if (output_buffer == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    error_code = mftscan_read_file_record(
        volume_handle,
        segment_frn,
        output_buffer,
        output_buffer_size,
        &actual_record,
        &reached_end);
    if (error_code != MFTSCAN_OK) {
        mftscan_set_error_detail("读取 attribute list 指向的 FRN %llu 失败", (unsigned long long)segment_frn);
        goto cleanup;
    }
    if (reached_end || actual_record != segment_frn) {
        mftscan_set_error_detail(
            "attribute list 指向 FRN %llu，但实际读取到 FRN %llu",
            (unsigned long long)segment_frn,
            (unsigned long long)actual_record);
        error_code = MFTSCAN_ERROR_MFT_PARSE;
        goto cleanup;
    }

    /* IOCTL output already has USA applied by the kernel. Trust the buffer. */
    header = (MftscanNtfsFileRecordHeader *)output_buffer->FileRecordBuffer;
    if (header->signature != MFTSCAN_FILE_RECORD_SIGNATURE ||
        (header->flags & 0x0001U) == 0U ||
        !mftscan_record_layout_is_valid(header, volume_handle->bytes_per_file_record)) {
        mftscan_set_error_detail("attribute list 指向的 FRN %llu 不是有效文件记录", (unsigned long long)segment_frn);
        error_code = MFTSCAN_ERROR_MFT_PARSE;
        goto cleanup;
    }

    if (segment_frn == base_record_frn) {
        if (header->base_file_record != 0ULL) {
            mftscan_set_error_detail("FRN %llu 应为 base record 但带有 base 引用", (unsigned long long)segment_frn);
            error_code = MFTSCAN_ERROR_MFT_PARSE;
            goto cleanup;
        }
    } else if ((header->base_file_record & MFTSCAN_FRN_MASK) != base_record_frn) {
        mftscan_set_error_detail(
            "extension FRN %llu 不属于 base FRN %llu",
            (unsigned long long)segment_frn,
            (unsigned long long)base_record_frn);
        error_code = MFTSCAN_ERROR_MFT_PARSE;
        goto cleanup;
    }

    attribute_offset = header->first_attribute_offset;
    while (attribute_offset + sizeof(MftscanNtfsAttributeHeader) <= header->used_size &&
        attribute_offset + sizeof(MftscanNtfsAttributeHeader) <= volume_handle->bytes_per_file_record) {
        MftscanNtfsAttributeHeader *attribute_header =
            (MftscanNtfsAttributeHeader *)(output_buffer->FileRecordBuffer + attribute_offset);

        if (attribute_header->type == MFTSCAN_ATTRIBUTE_END) {
            break;
        }
        if (attribute_header->length == 0 ||
            attribute_offset + attribute_header->length > header->used_size ||
            attribute_offset + attribute_header->length > volume_handle->bytes_per_file_record) {
            mftscan_set_error_detail("extension FRN %llu 属性边界非法", (unsigned long long)segment_frn);
            error_code = MFTSCAN_ERROR_MFT_PARSE;
            goto cleanup;
        }

        if (attribute_header->type == attribute_type &&
            mftscan_attribute_name_matches_list_entry(attribute_header, attribute_header->length, target_entry)) {
            error_code = mftscan_capture_storage_size_fragment(
                volume_handle,
                attribute_header,
                attribute_header->length,
                target_entry->name_length == 0U,
                data_size);
            if (error_code != MFTSCAN_OK) {
                goto cleanup;
            }
            if (data_size->present) {
                break;
            }
        }

        attribute_offset += attribute_header->length;
    }

cleanup:
    free(output_buffer);
    return error_code;
}

static MftscanError mftscan_capture_file_name_in_record(
    const MftscanVolumeHandle *volume_handle,
    uint64_t segment_frn,
    uint64_t base_record_frn,
    const MftscanNtfsAttributeListEntry *target_entry,
    MftscanRecordInfo *record_info,
    MftscanNtfsDataSizeCandidate *file_name_size) {
    NTFS_FILE_RECORD_OUTPUT_BUFFER *output_buffer = NULL;
    MftscanNtfsFileRecordHeader *header = NULL;
    size_t output_buffer_size = 0;
    uint64_t actual_record = 0ULL;
    bool reached_end = false;
    bool found = false;
    size_t attribute_offset = 0;
    MftscanError error_code = MFTSCAN_OK;

    if (volume_handle == NULL || target_entry == NULL || record_info == NULL ||
        file_name_size == NULL || segment_frn == 0ULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    output_buffer_size = offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) + volume_handle->bytes_per_file_record;
    output_buffer = (NTFS_FILE_RECORD_OUTPUT_BUFFER *)malloc(output_buffer_size);
    if (output_buffer == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    error_code = mftscan_read_file_record(
        volume_handle,
        segment_frn,
        output_buffer,
        output_buffer_size,
        &actual_record,
        &reached_end);
    if (error_code != MFTSCAN_OK) {
        mftscan_set_error_detail("读取 attribute list 指向的 FILE_NAME FRN %llu 失败", (unsigned long long)segment_frn);
        goto cleanup;
    }
    if (reached_end || actual_record != segment_frn) {
        mftscan_set_error_detail(
            "attribute list 指向 FILE_NAME FRN %llu，但实际读取到 FRN %llu",
            (unsigned long long)segment_frn,
            (unsigned long long)actual_record);
        error_code = MFTSCAN_ERROR_MFT_PARSE;
        goto cleanup;
    }

    header = (MftscanNtfsFileRecordHeader *)output_buffer->FileRecordBuffer;
    if (header->signature != MFTSCAN_FILE_RECORD_SIGNATURE ||
        (header->flags & 0x0001U) == 0U ||
        !mftscan_record_layout_is_valid(header, volume_handle->bytes_per_file_record)) {
        mftscan_set_error_detail("attribute list 指向的 FILE_NAME FRN %llu 不是有效文件记录", (unsigned long long)segment_frn);
        error_code = MFTSCAN_ERROR_MFT_PARSE;
        goto cleanup;
    }

    if (segment_frn == base_record_frn) {
        if (header->base_file_record != 0ULL) {
            mftscan_set_error_detail("FRN %llu 应为 base record 但带有 base 引用", (unsigned long long)segment_frn);
            error_code = MFTSCAN_ERROR_MFT_PARSE;
            goto cleanup;
        }
    } else if ((header->base_file_record & MFTSCAN_FRN_MASK) != base_record_frn) {
        mftscan_set_error_detail(
            "FILE_NAME extension FRN %llu 不属于 base FRN %llu",
            (unsigned long long)segment_frn,
            (unsigned long long)base_record_frn);
        error_code = MFTSCAN_ERROR_MFT_PARSE;
        goto cleanup;
    }

    attribute_offset = header->first_attribute_offset;
    while (attribute_offset + sizeof(MftscanNtfsAttributeHeader) <= header->used_size &&
        attribute_offset + sizeof(MftscanNtfsAttributeHeader) <= volume_handle->bytes_per_file_record) {
        MftscanNtfsAttributeHeader *attribute_header =
            (MftscanNtfsAttributeHeader *)(output_buffer->FileRecordBuffer + attribute_offset);

        if (attribute_header->type == MFTSCAN_ATTRIBUTE_END) {
            break;
        }
        if (attribute_header->length == 0 ||
            attribute_offset + attribute_header->length > header->used_size ||
            attribute_offset + attribute_header->length > volume_handle->bytes_per_file_record) {
            mftscan_set_error_detail("FILE_NAME extension FRN %llu 属性边界非法", (unsigned long long)segment_frn);
            error_code = MFTSCAN_ERROR_MFT_PARSE;
            goto cleanup;
        }

        if (attribute_header->type == MFTSCAN_ATTRIBUTE_FILE_NAME &&
            attribute_header->attribute_number == target_entry->attribute_id) {
            if (attribute_header->non_resident != 0U) {
                mftscan_set_error_detail("FILE_NAME extension FRN %llu 包含非 resident FILE_NAME", (unsigned long long)segment_frn);
                error_code = MFTSCAN_ERROR_MFT_PARSE;
                goto cleanup;
            }

            error_code = mftscan_capture_file_name(
                (const MftscanNtfsResidentAttributeHeader *)attribute_header,
                attribute_header->length,
                record_info,
                file_name_size);
            if (error_code != MFTSCAN_OK) {
                goto cleanup;
            }
            found = true;
            break;
        }

        attribute_offset += attribute_header->length;
    }

    if (!found) {
        mftscan_set_error_detail(
            "extension FRN %llu 未找到属性号 %u 的 FILE_NAME",
            (unsigned long long)segment_frn,
            (unsigned int)target_entry->attribute_id);
        error_code = MFTSCAN_ERROR_MFT_PARSE;
    }

cleanup:
    free(output_buffer);
    return error_code;
}

static MftscanError mftscan_resolve_data_size_from_attribute_list(
    const MftscanVolumeHandle *volume_handle,
    uint64_t base_record_frn,
    MftscanRecordInfo *record_info,
    MftscanNtfsParseState *parse_state,
    MftscanNtfsDataSizeCandidate *resolved_size) {
    size_t offset = 0;
    bool saw_data_entry = false;
    bool saw_vcn0_entry = false;
    MftscanNtfsDataSizeCandidate combined_size = { 0 };

    if (volume_handle == NULL || record_info == NULL || parse_state == NULL || resolved_size == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    while (offset < parse_state->attribute_list_length) {
        size_t minimum_length = offsetof(MftscanNtfsAttributeListEntry, attribute_name);
        const MftscanNtfsAttributeListEntry *entry =
            (const MftscanNtfsAttributeListEntry *)(parse_state->attribute_list_data + offset);
        uint64_t segment_frn = 0ULL;
        size_t remaining_length = parse_state->attribute_list_length - offset;
        MftscanError error_code = MFTSCAN_OK;

        if (remaining_length < minimum_length) {
            if (mftscan_bytes_are_zero(parse_state->attribute_list_data + offset, remaining_length)) {
                break;
            }
            mftscan_set_error_detail("$ATTRIBUTE_LIST 尾部非零碎片，offset=%zu", offset);
            return MFTSCAN_ERROR_MFT_PARSE;
        }

        if (entry->type == 0UL &&
            entry->record_length == 0U &&
            mftscan_bytes_are_zero(parse_state->attribute_list_data + offset, remaining_length)) {
            break;
        }

        if (entry->type == MFTSCAN_ATTRIBUTE_END) {
            break;
        }

        if (!mftscan_attribute_list_entry_is_valid(entry, remaining_length)) {
            mftscan_set_error_detail("$ATTRIBUTE_LIST entry 非法，offset=%zu", offset);
            return MFTSCAN_ERROR_MFT_PARSE;
        }

        if (entry->type == MFTSCAN_ATTRIBUTE_FILE_NAME && entry->name_length == 0U) {
            segment_frn = entry->segment_reference & MFTSCAN_FRN_MASK;
            if (segment_frn == 0ULL) {
                mftscan_set_error_detail("$ATTRIBUTE_LIST FILE_NAME entry 的 FRN 为 0");
                return MFTSCAN_ERROR_MFT_PARSE;
            }
            if (segment_frn != base_record_frn) {
                error_code = mftscan_capture_file_name_in_record(
                    volume_handle,
                    segment_frn,
                    base_record_frn,
                    entry,
                    record_info,
                    &parse_state->file_name_size);
                if (error_code != MFTSCAN_OK) {
                    return error_code;
                }
            }
        }

        if ((entry->type == MFTSCAN_ATTRIBUTE_DATA && entry->name_length == 0U) ||
            (!record_info->is_directory &&
                (((entry->type == MFTSCAN_ATTRIBUTE_DATA && entry->name_length != 0U) ||
                    entry->type == MFTSCAN_ATTRIBUTE_INDEX_ALLOCATION ||
                    entry->type == MFTSCAN_ATTRIBUTE_BITMAP ||
                    entry->type == MFTSCAN_ATTRIBUTE_LOGGED_UTILITY_STREAM))) ||
            (record_info->is_directory &&
                (entry->type == MFTSCAN_ATTRIBUTE_INDEX_ALLOCATION ||
                    entry->type == MFTSCAN_ATTRIBUTE_BITMAP))) {
            MftscanNtfsDataSizeCandidate fragment_size = { 0 };
            bool is_primary_data = (entry->type == MFTSCAN_ATTRIBUTE_DATA && entry->name_length == 0U);
            bool is_wof_backing_stream = (entry->type == MFTSCAN_ATTRIBUTE_DATA && entry->name_length != 0U &&
                mftscan_attribute_list_entry_name_equals(entry, L"WofCompressedData"));

            segment_frn = entry->segment_reference & MFTSCAN_FRN_MASK;
            if (segment_frn == 0ULL) {
                mftscan_set_error_detail("$ATTRIBUTE_LIST 相关 entry 的 FRN 为 0");
                return MFTSCAN_ERROR_MFT_PARSE;
            }

            if (segment_frn == base_record_frn) {
                if (is_primary_data && entry->lowest_vcn == 0ULL && parse_state->direct_data_size.present) {
                    fragment_size = parse_state->direct_data_size;
                } else {
                    error_code = mftscan_find_attribute_size_in_record(
                        volume_handle,
                        segment_frn,
                        base_record_frn,
                        entry->type,
                        entry,
                        &fragment_size);
                    if (error_code != MFTSCAN_OK) {
                        return error_code;
                    }
                }
            } else {
                error_code = mftscan_find_attribute_size_in_record(
                    volume_handle,
                    segment_frn,
                    base_record_frn,
                    entry->type,
                    entry,
                    &fragment_size);
                if (error_code != MFTSCAN_OK) {
                    return error_code;
                }
            }

            if (!fragment_size.present) {
                mftscan_set_error_detail(
                    "extension FRN %llu 未找到属性号 %u 的目标属性",
                    (unsigned long long)segment_frn,
                    (unsigned int)entry->attribute_id);
                return MFTSCAN_ERROR_MFT_PARSE;
            }

            if (is_primary_data) {
                saw_data_entry = true;
                if (!mftscan_add_data_size_candidate(&combined_size, &fragment_size)) {
                    return MFTSCAN_ERROR_MFT_PARSE;
                }
                if (entry->lowest_vcn == 0ULL) {
                    saw_vcn0_entry = true;
                }
            } else if (record_info->is_directory) {
                if (!mftscan_add_data_size_candidate(&parse_state->directory_self_size, &fragment_size)) {
                    return MFTSCAN_ERROR_MFT_PARSE;
                }
            } else if (is_wof_backing_stream) {
                if (!mftscan_add_data_size_candidate(&parse_state->wof_backing_size, &fragment_size)) {
                    return MFTSCAN_ERROR_MFT_PARSE;
                }
            } else {
                if (!mftscan_add_data_size_candidate(&parse_state->fallback_stream_size, &fragment_size)) {
                    return MFTSCAN_ERROR_MFT_PARSE;
                }
            }
        }

        offset += entry->record_length;
    }

    if (combined_size.present) {
        if (!saw_vcn0_entry) {
            mftscan_set_error_detail("$ATTRIBUTE_LIST 有未命名 $DATA entry 但缺少 lowest_vcn 0");
            return MFTSCAN_ERROR_MFT_PARSE;
        }
        *resolved_size = combined_size;
        return MFTSCAN_OK;
    }

    if (saw_data_entry) {
        mftscan_set_error_detail("$ATTRIBUTE_LIST 有未命名 $DATA entry 但未解析出大小");
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    return MFTSCAN_OK;
}

MftscanError mftscan_parse_file_record(
    const MftscanVolumeHandle *volume_handle,
    uint8_t *record_buffer,
    size_t record_length,
    uint64_t record_number,
    MftscanRecordInfo *record_info) {
    MftscanNtfsFileRecordHeader *header = NULL;
    MftscanNtfsParseState parse_state;
    MftscanNtfsDataSizeCandidate final_size = { 0 };
    MftscanError error_code = MFTSCAN_OK;
    size_t attribute_offset = 0;

    if (volume_handle == NULL || record_buffer == NULL || record_info == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    memset(&parse_state, 0, sizeof(parse_state));
    memset(record_info, 0, sizeof(*record_info));
    record_info->frn = record_number & MFTSCAN_FRN_MASK;

    if (record_length < sizeof(MftscanNtfsFileRecordHeader)) {
        mftscan_set_error_detail("FRN %llu 记录长度过短", (unsigned long long)record_info->frn);
        error_code = MFTSCAN_ERROR_MFT_PARSE;
        goto cleanup;
    }

    /* Caller is responsible for delivering a USA-fixed-up buffer. Streaming
     * reads cover every MFT slot, so we will encounter unallocated slots (all
     * zero) and corruption markers ("BAAD"). Treat anything that is not a FILE
     * record as not-in-use and skip cleanly instead of failing the whole scan. */
    header = (MftscanNtfsFileRecordHeader *)record_buffer;
    if (header->signature != MFTSCAN_FILE_RECORD_SIGNATURE) {
        record_info->in_use = false;
        goto cleanup;
    }

    if (!mftscan_record_layout_is_valid(header, record_length)) {
        mftscan_set_error_detail("FRN %llu 记录头非法", (unsigned long long)record_info->frn);
        error_code = MFTSCAN_ERROR_MFT_PARSE;
        goto cleanup;
    }

    if ((header->flags & 0x0001U) == 0U) {
        goto cleanup;
    }

    if (header->base_file_record != 0ULL) {
        goto cleanup;
    }

    record_info->in_use = true;
    record_info->is_directory = (header->flags & 0x0002U) != 0U;

    attribute_offset = header->first_attribute_offset;
    while (attribute_offset + sizeof(MftscanNtfsAttributeHeader) <= header->used_size &&
        attribute_offset + sizeof(MftscanNtfsAttributeHeader) <= record_length) {
        MftscanNtfsAttributeHeader *attribute_header = (MftscanNtfsAttributeHeader *)(record_buffer + attribute_offset);

        if (attribute_header->type == MFTSCAN_ATTRIBUTE_END) {
            break;
        }
        if (attribute_header->length == 0 ||
            attribute_offset + attribute_header->length > header->used_size ||
            attribute_offset + attribute_header->length > record_length) {
            mftscan_set_error_detail(
                "FRN %llu 属性边界非法，offset=%zu",
                (unsigned long long)record_info->frn,
                attribute_offset);
            error_code = MFTSCAN_ERROR_MFT_PARSE;
            goto cleanup;
        }

        if (attribute_header->type == MFTSCAN_ATTRIBUTE_FILE_NAME && attribute_header->non_resident == 0U) {
            error_code = mftscan_capture_file_name(
                (const MftscanNtfsResidentAttributeHeader *)attribute_header,
                attribute_header->length,
                record_info,
                &parse_state.file_name_size);
            if (error_code != MFTSCAN_OK) {
                if (mftscan_error_detail()[0] == '\0') {
                    mftscan_set_error_detail("FRN %llu FILE_NAME 解析失败", (unsigned long long)record_info->frn);
                }
                goto cleanup;
            }
        } else if (attribute_header->type == MFTSCAN_ATTRIBUTE_DATA && !record_info->is_directory) {
            error_code = mftscan_capture_data_size_candidate(
                volume_handle,
                attribute_header,
                attribute_header->length,
                &parse_state.direct_data_size);
            if (error_code != MFTSCAN_OK) {
                if (mftscan_error_detail()[0] == '\0') {
                    mftscan_set_error_detail("FRN %llu 未命名 $DATA 解析失败", (unsigned long long)record_info->frn);
                }
                goto cleanup;
            }
            if (attribute_header->name_length != 0U &&
                mftscan_is_metadata_tree_file_fallback_attribute(attribute_header)) {
                MftscanNtfsDataSizeCandidate fallback_size = { 0 };
                bool is_wof_backing_stream = mftscan_attribute_name_equals(
                    attribute_header,
                    attribute_header->length,
                    L"WofCompressedData");
                error_code = mftscan_capture_storage_size_fragment(
                    volume_handle,
                    attribute_header,
                    attribute_header->length,
                    false,
                    &fallback_size);
                if (error_code != MFTSCAN_OK) {
                    if (mftscan_error_detail()[0] == '\0') {
                        mftscan_set_error_detail("FRN %llu 命名系统流解析失败", (unsigned long long)record_info->frn);
                    }
                    goto cleanup;
                }
                if (is_wof_backing_stream) {
                    if (!mftscan_add_data_size_candidate(&parse_state.wof_backing_size, &fallback_size)) {
                        error_code = MFTSCAN_ERROR_MFT_PARSE;
                        goto cleanup;
                    }
                } else if (!mftscan_add_data_size_candidate(&parse_state.fallback_stream_size, &fallback_size)) {
                    error_code = MFTSCAN_ERROR_MFT_PARSE;
                    goto cleanup;
                }
            }
        } else if (!record_info->is_directory &&
            mftscan_is_metadata_tree_file_fallback_attribute(attribute_header)) {
            MftscanNtfsDataSizeCandidate fallback_size = { 0 };
            error_code = mftscan_capture_storage_size_fragment(
                volume_handle,
                attribute_header,
                attribute_header->length,
                false,
                &fallback_size);
            if (error_code != MFTSCAN_OK) {
                if (mftscan_error_detail()[0] == '\0') {
                    mftscan_set_error_detail("FRN %llu 系统元文件属性解析失败", (unsigned long long)record_info->frn);
                }
                goto cleanup;
            }
            if (!mftscan_add_data_size_candidate(&parse_state.fallback_stream_size, &fallback_size)) {
                error_code = MFTSCAN_ERROR_MFT_PARSE;
                goto cleanup;
            }
        } else if (record_info->is_directory &&
            mftscan_is_metadata_tree_directory_self_attribute(attribute_header)) {
            MftscanNtfsDataSizeCandidate directory_size = { 0 };
            error_code = mftscan_capture_storage_size_fragment(
                volume_handle,
                attribute_header,
                attribute_header->length,
                false,
                &directory_size);
            if (error_code != MFTSCAN_OK) {
                if (mftscan_error_detail()[0] == '\0') {
                    mftscan_set_error_detail("FRN %llu 系统目录元数据解析失败", (unsigned long long)record_info->frn);
                }
                goto cleanup;
            }
            if (!mftscan_add_data_size_candidate(&parse_state.directory_self_size, &directory_size)) {
                error_code = MFTSCAN_ERROR_MFT_PARSE;
                goto cleanup;
            }
        } else if (attribute_header->type == MFTSCAN_ATTRIBUTE_ATTRIBUTE_LIST) {
            error_code = mftscan_capture_attribute_list(
                volume_handle,
                attribute_header,
                attribute_header->length,
                &parse_state);
            if (error_code != MFTSCAN_OK) {
                if (mftscan_error_detail()[0] == '\0') {
                    mftscan_set_error_detail("FRN %llu $ATTRIBUTE_LIST 捕获失败", (unsigned long long)record_info->frn);
                }
                goto cleanup;
            }
        }

        attribute_offset += attribute_header->length;
    }

    if (parse_state.has_attribute_list) {
        error_code = mftscan_resolve_data_size_from_attribute_list(
            volume_handle,
            record_info->frn,
            record_info,
            &parse_state,
            &final_size);
        if (error_code != MFTSCAN_OK) {
            if (mftscan_error_detail()[0] == '\0') {
                mftscan_set_error_detail("FRN %llu $ATTRIBUTE_LIST 解析失败", (unsigned long long)record_info->frn);
            }
            goto cleanup;
        }
        if (final_size.present) {
            record_info->has_primary_stream_size = true;
        }
    }

    if (!record_info->is_directory) {
        if (parse_state.direct_data_size.present && !final_size.present) {
            final_size = parse_state.direct_data_size;
            record_info->has_primary_stream_size = true;
        } else if (!final_size.present && parse_state.file_name_size.present) {
            final_size = parse_state.file_name_size;
        }

        if (final_size.present) {
            if (final_size.allocated_size == 0ULL && parse_state.wof_backing_size.present) {
                final_size.allocated_size = parse_state.wof_backing_size.allocated_size;
            }
            record_info->logical_size = final_size.logical_size;
            record_info->allocated_size = final_size.allocated_size;
            record_info->has_data_size = true;
        }
        if (parse_state.fallback_stream_size.present) {
            record_info->metadata_fallback_logical_size = parse_state.fallback_stream_size.logical_size;
            record_info->metadata_fallback_allocated_size = parse_state.fallback_stream_size.allocated_size;
            record_info->has_metadata_fallback_size = true;
        }
    } else if (parse_state.directory_self_size.present) {
        record_info->directory_metadata_allocated_size = parse_state.directory_self_size.allocated_size;
        record_info->has_directory_metadata_size = true;
    }

    if (record_info->frn != MFTSCAN_ROOT_FRN &&
        ((record_info->is_directory && record_info->name == NULL) ||
            (!record_info->is_directory &&
                record_info->name == NULL &&
                record_info->file_name_link_count == 0U))) {
        record_info->in_use = false;
    }

cleanup:
    mftscan_parse_state_free(&parse_state);
    if (error_code != MFTSCAN_OK) {
        mftscan_free_record_info(record_info);
    }
    return error_code;
}
