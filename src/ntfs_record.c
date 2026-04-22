#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "model.h"

#define MFTSCAN_FILE_RECORD_SIGNATURE 0x454c4946UL
#define MFTSCAN_ATTRIBUTE_END 0xffffffffUL
#define MFTSCAN_ATTRIBUTE_ATTRIBUTE_LIST 0x20UL
#define MFTSCAN_ATTRIBUTE_FILE_NAME 0x30UL
#define MFTSCAN_ATTRIBUTE_DATA 0x80UL

#define MFTSCAN_MAX_ATTRIBUTE_LIST_DEPTH 64U

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

static MftscanError mftscan_apply_update_sequence_array(uint8_t *record_buffer, size_t record_length, uint32_t bytes_per_sector) {
    MftscanNtfsFileRecordHeader *header = (MftscanNtfsFileRecordHeader *)record_buffer;
    WORD *update_sequence_words = NULL;
    WORD update_sequence_number = 0;
    WORD sector_index = 0;

    if (record_length < sizeof(MftscanNtfsFileRecordHeader) || bytes_per_sector < sizeof(WORD) * 2U) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    if (header->update_sequence_offset + header->update_sequence_count * sizeof(WORD) > record_length) {
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
            return MFTSCAN_OK;
        }

        *sector_trailer = update_sequence_words[sector_index];
    }

    return MFTSCAN_OK;
}

static bool mftscan_record_layout_is_valid(const MftscanNtfsFileRecordHeader *header, size_t record_length) {
    if (header == NULL) {
        return false;
    }

    return header->first_attribute_offset >= sizeof(MftscanNtfsFileRecordHeader) &&
        header->first_attribute_offset <= header->used_size &&
        header->used_size <= record_length;
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

static MftscanError mftscan_capture_data_size_candidate(
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

        if (sizeof(MftscanNtfsNonResidentAttributeHeader) > attribute_length) {
            return MFTSCAN_ERROR_MFT_PARSE;
        }

        if (non_resident_header->lowest_vcn != 0ULL) {
            return MFTSCAN_OK;
        }

        mftscan_set_data_size_candidate(candidate, non_resident_header->data_size, non_resident_header->allocated_size);
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

static MftscanError mftscan_copy_nonresident_attribute_value(
    const MftscanVolumeHandle *volume_handle,
    const MftscanNtfsNonResidentAttributeHeader *non_resident_header,
    size_t attribute_length,
    uint8_t **value_data,
    size_t *value_length) {
    const uint8_t *mapping_pair = NULL;
    const uint8_t *mapping_end = NULL;
    uint8_t *copied_value = NULL;
    uint8_t *cluster_buffer = NULL;
    size_t output_offset = 0;
    int64_t current_lcn = 0;
    MftscanError error_code = MFTSCAN_OK;

    if (volume_handle == NULL ||
        non_resident_header == NULL ||
        value_data == NULL ||
        value_length == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (volume_handle->bytes_per_cluster == 0U ||
        sizeof(MftscanNtfsNonResidentAttributeHeader) > attribute_length ||
        non_resident_header->mapping_pairs_offset >= attribute_length) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    if (non_resident_header->data_size > SIZE_MAX) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
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

    mapping_pair = (const uint8_t *)non_resident_header + non_resident_header->mapping_pairs_offset;
    mapping_end = (const uint8_t *)non_resident_header + attribute_length;
    while (mapping_pair < mapping_end && output_offset < (size_t)non_resident_header->data_size) {
        BYTE run_header = *mapping_pair++;
        size_t length_size = 0;
        size_t offset_size = 0;
        uint64_t cluster_count = 0ULL;
        int64_t lcn_delta = 0;
        uint64_t cluster_index = 0ULL;

        if (run_header == 0U) {
            break;
        }

        length_size = run_header & 0x0FU;
        offset_size = run_header >> 4U;
        if (length_size == 0U || mapping_pair + length_size + offset_size > mapping_end) {
            error_code = MFTSCAN_ERROR_MFT_PARSE;
            goto cleanup;
        }

        cluster_count = mftscan_read_unsigned_le(mapping_pair, length_size);
        mapping_pair += length_size;
        lcn_delta = mftscan_read_signed_le(mapping_pair, offset_size);
        mapping_pair += offset_size;
        if (cluster_count == 0ULL) {
            error_code = MFTSCAN_ERROR_MFT_PARSE;
            goto cleanup;
        }

        if (offset_size != 0U) {
            current_lcn += lcn_delta;
            if (current_lcn < 0) {
                error_code = MFTSCAN_ERROR_MFT_PARSE;
                goto cleanup;
            }
        }

        for (cluster_index = 0ULL;
            cluster_index < cluster_count && output_offset < (size_t)non_resident_header->data_size;
            ++cluster_index) {
            size_t copy_size = volume_handle->bytes_per_cluster;

            if ((size_t)non_resident_header->data_size - output_offset < copy_size) {
                copy_size = (size_t)non_resident_header->data_size - output_offset;
            }

            if (offset_size == 0U) {
                memset(copied_value + output_offset, 0, copy_size);
            } else {
                uint64_t absolute_lcn = 0ULL;
                uint64_t byte_offset = 0ULL;

                if ((uint64_t)current_lcn > UINT64_MAX - cluster_index) {
                    error_code = MFTSCAN_ERROR_MFT_PARSE;
                    goto cleanup;
                }

                absolute_lcn = (uint64_t)current_lcn + cluster_index;
                if (absolute_lcn > UINT64_MAX / volume_handle->bytes_per_cluster) {
                    error_code = MFTSCAN_ERROR_MFT_PARSE;
                    goto cleanup;
                }

                byte_offset = absolute_lcn * volume_handle->bytes_per_cluster;
                error_code = mftscan_read_volume_bytes(
                    volume_handle,
                    byte_offset,
                    cluster_buffer,
                    volume_handle->bytes_per_cluster);
                if (error_code != MFTSCAN_OK) {
                    goto cleanup;
                }

                memcpy(copied_value + output_offset, cluster_buffer, copy_size);
            }

            output_offset += copy_size;
        }
    }

    if (output_offset != (size_t)non_resident_header->data_size) {
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

static MftscanError mftscan_find_data_size_in_record(
    const MftscanVolumeHandle *volume_handle,
    uint64_t segment_frn,
    uint64_t base_record_frn,
    uint16_t attribute_number,
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

    error_code = mftscan_apply_update_sequence_array(
        output_buffer->FileRecordBuffer,
        volume_handle->bytes_per_file_record,
        volume_handle->bytes_per_sector);
    if (error_code != MFTSCAN_OK) {
        mftscan_set_error_detail("attribute list 指向的 FRN %llu USA 修复失败", (unsigned long long)segment_frn);
        goto cleanup;
    }

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

        if (attribute_header->type == MFTSCAN_ATTRIBUTE_DATA &&
            attribute_header->attribute_number == attribute_number) {
            error_code = mftscan_capture_data_size_candidate(attribute_header, attribute_header->length, data_size);
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

static MftscanError mftscan_resolve_data_size_from_attribute_list(
    const MftscanVolumeHandle *volume_handle,
    uint64_t base_record_frn,
    const MftscanNtfsParseState *parse_state,
    MftscanNtfsDataSizeCandidate *resolved_size) {
    size_t offset = 0;
    bool saw_vcn0_entry = false;
    size_t follow_count = 0;

    if (volume_handle == NULL || parse_state == NULL || resolved_size == NULL) {
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

        if (entry->type == MFTSCAN_ATTRIBUTE_DATA &&
            entry->name_length == 0U &&
            entry->lowest_vcn == 0ULL) {
            saw_vcn0_entry = true;
            if (++follow_count > MFTSCAN_MAX_ATTRIBUTE_LIST_DEPTH) {
                mftscan_set_error_detail("$ATTRIBUTE_LIST 跟随次数超过上限");
                return MFTSCAN_ERROR_MFT_PARSE;
            }

            segment_frn = entry->segment_reference & MFTSCAN_FRN_MASK;
            if (segment_frn == 0ULL) {
                mftscan_set_error_detail("$ATTRIBUTE_LIST 未命名 $DATA entry 的 FRN 为 0");
                return MFTSCAN_ERROR_MFT_PARSE;
            }

            if (segment_frn == base_record_frn) {
                if (!parse_state->direct_data_size.present) {
                    mftscan_set_error_detail("$ATTRIBUTE_LIST 指向 base record，但 base 中没有未命名 $DATA");
                    return MFTSCAN_ERROR_MFT_PARSE;
                }

                *resolved_size = parse_state->direct_data_size;
                return MFTSCAN_OK;
            }

            error_code = mftscan_find_data_size_in_record(
                volume_handle,
                segment_frn,
                base_record_frn,
                entry->attribute_id,
                resolved_size);
            if (error_code != MFTSCAN_OK) {
                return error_code;
            }
            if (!resolved_size->present) {
                mftscan_set_error_detail(
                    "extension FRN %llu 未找到属性号 %u 的未命名 $DATA",
                    (unsigned long long)segment_frn,
                    (unsigned int)entry->attribute_id);
                return MFTSCAN_ERROR_MFT_PARSE;
            }

            return MFTSCAN_OK;
        }

        offset += entry->record_length;
    }

    if (parse_state->direct_data_size.present) {
        *resolved_size = parse_state->direct_data_size;
        return MFTSCAN_OK;
    }

    if (saw_vcn0_entry) {
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

    error_code = mftscan_apply_update_sequence_array(record_buffer, record_length, volume_handle->bytes_per_sector);
    if (error_code != MFTSCAN_OK) {
        mftscan_set_error_detail("FRN %llu USA 修复失败", (unsigned long long)record_info->frn);
        goto cleanup;
    }

    header = (MftscanNtfsFileRecordHeader *)record_buffer;
    if (header->signature != MFTSCAN_FILE_RECORD_SIGNATURE || !mftscan_record_layout_is_valid(header, record_length)) {
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
                attribute_header,
                attribute_header->length,
                &parse_state.direct_data_size);
            if (error_code != MFTSCAN_OK) {
                if (mftscan_error_detail()[0] == '\0') {
                    mftscan_set_error_detail("FRN %llu 未命名 $DATA 解析失败", (unsigned long long)record_info->frn);
                }
                goto cleanup;
            }
        } else if (attribute_header->type == MFTSCAN_ATTRIBUTE_ATTRIBUTE_LIST && !record_info->is_directory) {
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

    if (!record_info->is_directory) {
        if (parse_state.has_attribute_list) {
            error_code = mftscan_resolve_data_size_from_attribute_list(
                volume_handle,
                record_info->frn,
                &parse_state,
                &final_size);
            if (error_code != MFTSCAN_OK) {
                if (mftscan_error_detail()[0] == '\0') {
                    mftscan_set_error_detail("FRN %llu $ATTRIBUTE_LIST 解析失败", (unsigned long long)record_info->frn);
                }
                goto cleanup;
            }
            if (!final_size.present) {
                if (parse_state.file_name_size.present) {
                    final_size = parse_state.file_name_size;
                } else {
                    mftscan_set_error_detail("FRN %llu 有 $ATTRIBUTE_LIST 但未得到未命名 $DATA 大小", (unsigned long long)record_info->frn);
                    error_code = MFTSCAN_ERROR_MFT_PARSE;
                    goto cleanup;
                }
            }
        } else if (parse_state.direct_data_size.present) {
            final_size = parse_state.direct_data_size;
        } else if (parse_state.file_name_size.present) {
            final_size = parse_state.file_name_size;
        }

        if (final_size.present) {
            record_info->logical_size = final_size.logical_size;
            record_info->allocated_size = final_size.allocated_size;
            record_info->has_data_size = true;
        }
    }

    if (record_info->name == NULL && record_info->frn != MFTSCAN_ROOT_FRN) {
        record_info->in_use = false;
    }

cleanup:
    mftscan_parse_state_free(&parse_state);
    if (error_code != MFTSCAN_OK) {
        mftscan_free_record_info(record_info);
    }
    return error_code;
}
