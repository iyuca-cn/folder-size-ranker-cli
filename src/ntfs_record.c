#include <windows.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "model.h"

#define MFTSCAN_FILE_RECORD_SIGNATURE 0x454c4946UL
#define MFTSCAN_ATTRIBUTE_END 0xffffffffUL
#define MFTSCAN_ATTRIBUTE_FILE_NAME 0x30UL
#define MFTSCAN_ATTRIBUTE_DATA 0x80UL

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
#pragma pack(pop)

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

static MftscanError mftscan_capture_file_name(
    const MftscanNtfsResidentAttributeHeader *resident_header,
    size_t attribute_length,
    bool is_directory,
    MftscanRecordInfo *record_info) {
    const MftscanNtfsFileNameAttribute *file_name_attribute = NULL;
    uint8_t candidate_priority = 0;
    size_t name_size_bytes = 0;
    wchar_t *candidate_name = NULL;

    if (resident_header->value_offset + resident_header->value_length > attribute_length ||
        resident_header->value_length < offsetof(MftscanNtfsFileNameAttribute, file_name)) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    file_name_attribute = (const MftscanNtfsFileNameAttribute *)((const uint8_t *)resident_header + resident_header->value_offset);
    name_size_bytes = (size_t)file_name_attribute->file_name_length * sizeof(WCHAR);
    if (resident_header->value_length < offsetof(MftscanNtfsFileNameAttribute, file_name) + name_size_bytes) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    if (!is_directory && !record_info->has_data_size) {
        if (file_name_attribute->real_size > record_info->logical_size) {
            record_info->logical_size = file_name_attribute->real_size;
        }
        if (file_name_attribute->allocated_size > record_info->allocated_size) {
            record_info->allocated_size = file_name_attribute->allocated_size;
        }
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

static MftscanError mftscan_capture_data_size(
    const MftscanNtfsAttributeHeader *attribute_header,
    size_t attribute_length,
    MftscanRecordInfo *record_info) {
    if (attribute_header == NULL || record_info == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    if (record_info->is_directory) {
        return MFTSCAN_OK;
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

        record_info->logical_size = resident_header->value_length;
        record_info->allocated_size = resident_header->value_length;
        record_info->has_data_size = true;
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

        record_info->logical_size = non_resident_header->data_size;
        record_info->allocated_size = non_resident_header->allocated_size;
        record_info->has_data_size = true;
        return MFTSCAN_OK;
    }
}

MftscanError mftscan_parse_file_record(
    uint8_t *record_buffer,
    size_t record_length,
    uint32_t bytes_per_sector,
    uint64_t record_number,
    MftscanRecordInfo *record_info) {
    MftscanNtfsFileRecordHeader *header = NULL;
    MftscanError error_code = MFTSCAN_OK;
    size_t attribute_offset = 0;

    if (record_buffer == NULL || record_info == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    memset(record_info, 0, sizeof(*record_info));
    record_info->frn = record_number & MFTSCAN_FRN_MASK;

    error_code = mftscan_apply_update_sequence_array(record_buffer, record_length, bytes_per_sector);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }

    header = (MftscanNtfsFileRecordHeader *)record_buffer;
    if (header->signature != MFTSCAN_FILE_RECORD_SIGNATURE) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }

    if ((header->flags & 0x0001U) == 0U) {
        return MFTSCAN_OK;
    }

    if (header->base_file_record != 0ULL) {
        return MFTSCAN_OK;
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
        if (attribute_header->length == 0 || attribute_offset + attribute_header->length > header->used_size ||
            attribute_offset + attribute_header->length > record_length) {
            return MFTSCAN_ERROR_MFT_PARSE;
        }

        if (attribute_header->type == MFTSCAN_ATTRIBUTE_FILE_NAME && attribute_header->non_resident == 0U) {
            error_code = mftscan_capture_file_name(
                (const MftscanNtfsResidentAttributeHeader *)attribute_header,
                attribute_header->length,
                record_info->is_directory,
                record_info);
            if (error_code != MFTSCAN_OK) {
                return error_code;
            }
        } else if (attribute_header->type == MFTSCAN_ATTRIBUTE_DATA) {
            error_code = mftscan_capture_data_size(
                attribute_header,
                attribute_header->length,
                record_info);
            if (error_code != MFTSCAN_OK) {
                return error_code;
            }
        }

        attribute_offset += attribute_header->length;
    }

    if (record_info->name == NULL && record_info->frn != MFTSCAN_ROOT_FRN) {
        record_info->in_use = false;
    }

    return MFTSCAN_OK;
}
