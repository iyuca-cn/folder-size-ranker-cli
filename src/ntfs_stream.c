#include <windows.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "model.h"

#define MFTSCAN_FILE_RECORD_SIGNATURE 0x454c4946UL
#define MFTSCAN_ATTRIBUTE_END 0xffffffffUL
#define MFTSCAN_ATTRIBUTE_DATA 0x80UL
#define MFTSCAN_STREAM_CHUNK_BYTES (4U * 1024U * 1024U)

#pragma pack(push, 1)
typedef struct MftscanStreamFileRecordHeader {
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
} MftscanStreamFileRecordHeader;

typedef struct MftscanStreamAttributeHeader {
    DWORD type;
    DWORD length;
    BYTE non_resident;
    BYTE name_length;
    WORD name_offset;
    WORD flags;
    WORD attribute_number;
} MftscanStreamAttributeHeader;

typedef struct MftscanStreamNonResidentHeader {
    MftscanStreamAttributeHeader header;
    ULONGLONG lowest_vcn;
    ULONGLONG highest_vcn;
    WORD mapping_pairs_offset;
    BYTE compression_unit;
    BYTE reserved[5];
    ULONGLONG allocated_size;
    ULONGLONG data_size;
    ULONGLONG initialized_size;
} MftscanStreamNonResidentHeader;
#pragma pack(pop)

void mftscan_runlist_init(MftscanRunlist *runlist) {
    if (runlist != NULL) {
        memset(runlist, 0, sizeof(*runlist));
    }
}

void mftscan_runlist_free(MftscanRunlist *runlist) {
    if (runlist == NULL) {
        return;
    }
    free(runlist->extents);
    runlist->extents = NULL;
    runlist->count = 0;
    runlist->capacity = 0;
    runlist->total_clusters = 0ULL;
}

static MftscanError mftscan_runlist_append_callback(
    void *user_data,
    uint64_t cluster_count,
    uint64_t starting_lcn,
    bool sparse) {
    MftscanRunlist *runlist = (MftscanRunlist *)user_data;
    MftscanRunlistExtent *grown_items = NULL;

    grown_items = (MftscanRunlistExtent *)mftscan_realloc_array(
        runlist->extents,
        sizeof(MftscanRunlistExtent),
        &runlist->capacity,
        runlist->count + 1U);
    if (grown_items == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }
    runlist->extents = grown_items;
    runlist->extents[runlist->count].starting_lcn = starting_lcn;
    runlist->extents[runlist->count].cluster_count = cluster_count;
    runlist->extents[runlist->count].sparse = sparse;
    runlist->count += 1U;

    if (runlist->total_clusters > UINT64_MAX - cluster_count) {
        return MFTSCAN_ERROR_MFT_PARSE;
    }
    runlist->total_clusters += cluster_count;
    return MFTSCAN_OK;
}

MftscanError mftscan_runlist_decode(
    const uint8_t *mapping_pairs,
    size_t mapping_pairs_length,
    MftscanRunlist *runlist) {
    if (runlist == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }
    return mftscan_iterate_mapping_pairs(
        mapping_pairs, mapping_pairs_length,
        mftscan_runlist_append_callback, runlist);
}

/* Read FRN 0 ($MFT) and extract its unnamed $DATA runlist. */
static MftscanError mftscan_stream_load_mft_runlist(
    const MftscanVolumeHandle *volume_handle,
    MftscanRunlist *runlist) {
    NTFS_FILE_RECORD_OUTPUT_BUFFER *output_buffer = NULL;
    size_t output_buffer_size = 0;
    uint64_t actual_record = 0ULL;
    bool reached_end = false;
    MftscanStreamFileRecordHeader *header = NULL;
    size_t attribute_offset = 0;
    MftscanError error_code = MFTSCAN_OK;
    bool found = false;

    if (volume_handle == NULL || runlist == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    output_buffer_size = offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer)
        + volume_handle->bytes_per_file_record;
    output_buffer = (NTFS_FILE_RECORD_OUTPUT_BUFFER *)malloc(output_buffer_size);
    if (output_buffer == NULL) {
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    error_code = mftscan_read_file_record(
        volume_handle, 0ULL, output_buffer, output_buffer_size,
        &actual_record, &reached_end);
    if (error_code != MFTSCAN_OK) {
        mftscan_set_error_detail("读取 $MFT FRN 0 失败");
        goto cleanup;
    }
    if (reached_end || actual_record != 0ULL) {
        mftscan_set_error_detail("$MFT FRN 0 不存在");
        error_code = MFTSCAN_ERROR_MFT_PARSE;
        goto cleanup;
    }

    /* IOCTL output is already USA-fixed-up by the kernel. Trust the buffer. */
    header = (MftscanStreamFileRecordHeader *)output_buffer->FileRecordBuffer;
    if (header->signature != MFTSCAN_FILE_RECORD_SIGNATURE ||
        (header->flags & 0x0001U) == 0U) {
        mftscan_set_error_detail("$MFT FRN 0 记录头非法");
        error_code = MFTSCAN_ERROR_MFT_PARSE;
        goto cleanup;
    }

    attribute_offset = header->first_attribute_offset;
    while (attribute_offset + sizeof(MftscanStreamAttributeHeader) <= header->used_size &&
        attribute_offset + sizeof(MftscanStreamAttributeHeader) <= volume_handle->bytes_per_file_record) {
        MftscanStreamAttributeHeader *attribute_header =
            (MftscanStreamAttributeHeader *)(output_buffer->FileRecordBuffer + attribute_offset);

        if (attribute_header->type == MFTSCAN_ATTRIBUTE_END) {
            break;
        }
        if (attribute_header->length == 0U ||
            attribute_offset + attribute_header->length > header->used_size) {
            error_code = MFTSCAN_ERROR_MFT_PARSE;
            goto cleanup;
        }

        if (attribute_header->type == MFTSCAN_ATTRIBUTE_DATA &&
            attribute_header->name_length == 0U &&
            attribute_header->non_resident != 0U) {
            MftscanStreamNonResidentHeader *nrh =
                (MftscanStreamNonResidentHeader *)attribute_header;
            const uint8_t *mapping_pairs = NULL;
            size_t mapping_pairs_length = 0;

            if (sizeof(MftscanStreamNonResidentHeader) > attribute_header->length ||
                nrh->mapping_pairs_offset < sizeof(MftscanStreamNonResidentHeader) ||
                nrh->mapping_pairs_offset >= attribute_header->length) {
                error_code = MFTSCAN_ERROR_MFT_PARSE;
                goto cleanup;
            }
            mapping_pairs = (const uint8_t *)nrh + nrh->mapping_pairs_offset;
            mapping_pairs_length = attribute_header->length - nrh->mapping_pairs_offset;
            error_code = mftscan_runlist_decode(mapping_pairs, mapping_pairs_length, runlist);
            if (error_code != MFTSCAN_OK) {
                mftscan_set_error_detail("$MFT $DATA runlist 解码失败");
                goto cleanup;
            }
            found = true;
            break;
        }

        attribute_offset += attribute_header->length;
    }

    if (!found) {
        mftscan_set_error_detail(
            "$MFT FRN 0 中未找到非 resident 的未命名 $DATA (可能 runlist 在 $ATTRIBUTE_LIST 中，需走 IOCTL 路径)");
        error_code = MFTSCAN_ERROR_MFT_PARSE;
        goto cleanup;
    }

cleanup:
    free(output_buffer);
    return error_code;
}

/* Locate the LCN holding the cluster at virtual offset vcn within the MFT.
 * Returns false if vcn is beyond the runlist (caller should treat as EOF). */
static bool mftscan_stream_vcn_to_lcn(
    const MftscanRunlist *runlist,
    uint64_t vcn,
    uint64_t *lcn_out,
    bool *sparse_out) {
    size_t index = 0;
    uint64_t accumulated = 0ULL;

    if (runlist == NULL || lcn_out == NULL || sparse_out == NULL) {
        return false;
    }

    for (index = 0; index < runlist->count; ++index) {
        const MftscanRunlistExtent *extent = &runlist->extents[index];
        if (vcn < accumulated + extent->cluster_count) {
            uint64_t offset_within_run = vcn - accumulated;
            *sparse_out = extent->sparse;
            if (extent->sparse) {
                *lcn_out = 0ULL;
            } else {
                *lcn_out = extent->starting_lcn + offset_within_run;
            }
            return true;
        }
        accumulated += extent->cluster_count;
    }
    return false;
}

MftscanError mftscan_record_stream_open(
    const MftscanVolumeHandle *volume_handle,
    MftscanRecordStream *stream) {
    MftscanError error_code = MFTSCAN_OK;
    uint32_t bytes_per_record = 0;
    size_t records_per_chunk = 0;

    if (volume_handle == NULL || stream == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    memset(stream, 0, sizeof(*stream));
    stream->volume_handle = volume_handle;
    mftscan_runlist_init(&stream->runlist);

    bytes_per_record = volume_handle->bytes_per_file_record;
    if (bytes_per_record == 0U || volume_handle->bytes_per_cluster == 0U) {
        return MFTSCAN_ERROR_VOLUME_QUERY;
    }

    if (volume_handle->mft_valid_data_length > 0ULL) {
        stream->total_records =
            volume_handle->mft_valid_data_length / bytes_per_record;
    } else {
        stream->total_records = volume_handle->highest_record_number + 1ULL;
    }

    /* Try the streaming fast path. If anything fails we fall back to IOCTL. */
    error_code = mftscan_stream_load_mft_runlist(volume_handle, &stream->runlist);
    if (error_code == MFTSCAN_OK && stream->runlist.count > 0U) {
        records_per_chunk = MFTSCAN_STREAM_CHUNK_BYTES / bytes_per_record;
        if (records_per_chunk == 0U) {
            records_per_chunk = 1U;
        }
        stream->chunk_records = records_per_chunk;
        stream->chunk_capacity_bytes = records_per_chunk * bytes_per_record;
        stream->chunk_buffer = (uint8_t *)malloc(stream->chunk_capacity_bytes);
        if (stream->chunk_buffer == NULL) {
            mftscan_runlist_free(&stream->runlist);
            return MFTSCAN_ERROR_OUT_OF_MEMORY;
        }
        stream->chunk_first_record = UINT64_MAX;
        stream->chunk_valid_records = 0;
        stream->runlist_initialized = true;
    } else {
        /* Streaming unavailable. Clear any partial detail and fall back. */
        mftscan_runlist_free(&stream->runlist);
        mftscan_set_error_detail("");
        stream->runlist_initialized = false;
    }

    stream->fallback_buffer_size =
        offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) + bytes_per_record;
    stream->fallback_buffer =
        (NTFS_FILE_RECORD_OUTPUT_BUFFER *)malloc(stream->fallback_buffer_size);
    if (stream->fallback_buffer == NULL) {
        mftscan_record_stream_close(stream);
        return MFTSCAN_ERROR_OUT_OF_MEMORY;
    }

    return MFTSCAN_OK;
}

void mftscan_record_stream_close(MftscanRecordStream *stream) {
    if (stream == NULL) {
        return;
    }
    free(stream->chunk_buffer);
    stream->chunk_buffer = NULL;
    free(stream->fallback_buffer);
    stream->fallback_buffer = NULL;
    mftscan_runlist_free(&stream->runlist);
    memset(stream, 0, sizeof(*stream));
}

/* Load chunk_records consecutive records starting at chunk_first_record into
 * stream->chunk_buffer by walking the $MFT runlist and issuing bulk reads.
 * Sparse runs translate to zero-filled regions. */
static MftscanError mftscan_stream_load_chunk(
    MftscanRecordStream *stream,
    uint64_t first_record) {
    const MftscanVolumeHandle *volume_handle = stream->volume_handle;
    uint32_t bytes_per_record = volume_handle->bytes_per_file_record;
    uint32_t bytes_per_cluster = volume_handle->bytes_per_cluster;
    uint64_t mft_byte_offset = first_record * bytes_per_record;
    uint64_t requested_bytes = (uint64_t)stream->chunk_records * bytes_per_record;
    uint64_t mft_size_bytes = stream->total_records * bytes_per_record;
    uint64_t available_bytes = 0ULL;
    uint64_t bytes_filled = 0ULL;
    uint64_t records_filled = 0ULL;

    if (mft_byte_offset >= mft_size_bytes) {
        stream->chunk_first_record = first_record;
        stream->chunk_valid_records = 0;
        return MFTSCAN_OK;
    }
    available_bytes = mft_size_bytes - mft_byte_offset;
    if (requested_bytes > available_bytes) {
        requested_bytes = available_bytes;
    }

    while (bytes_filled < requested_bytes) {
        uint64_t cur_mft_offset = mft_byte_offset + bytes_filled;
        uint64_t vcn = cur_mft_offset / bytes_per_cluster;
        uint64_t intra_cluster = cur_mft_offset % bytes_per_cluster;
        uint64_t lcn = 0ULL;
        bool sparse = false;
        uint64_t this_run_bytes = 0ULL;

        if (!mftscan_stream_vcn_to_lcn(&stream->runlist, vcn, &lcn, &sparse)) {
            /* Past the end of the runlist before MftValidDataLength: treat
             * remainder as zero (defensive — shouldn't normally happen). */
            memset(stream->chunk_buffer + bytes_filled, 0,
                (size_t)(requested_bytes - bytes_filled));
            bytes_filled = requested_bytes;
            break;
        }

        /* How many consecutive bytes are contiguous starting at vcn? */
        {
            size_t extent_index = 0;
            uint64_t accumulated = 0ULL;
            uint64_t clusters_left_in_run = 0ULL;

            for (extent_index = 0; extent_index < stream->runlist.count; ++extent_index) {
                const MftscanRunlistExtent *extent =
                    &stream->runlist.extents[extent_index];
                if (vcn < accumulated + extent->cluster_count) {
                    clusters_left_in_run =
                        (accumulated + extent->cluster_count) - vcn;
                    break;
                }
                accumulated += extent->cluster_count;
            }
            this_run_bytes = clusters_left_in_run * bytes_per_cluster - intra_cluster;
        }

        if (this_run_bytes > requested_bytes - bytes_filled) {
            this_run_bytes = requested_bytes - bytes_filled;
        }

        if (sparse) {
            memset(stream->chunk_buffer + bytes_filled, 0, (size_t)this_run_bytes);
        } else {
            uint64_t volume_byte_offset = lcn * bytes_per_cluster + intra_cluster;
            MftscanError error_code = mftscan_read_volume_bytes(
                volume_handle,
                volume_byte_offset,
                stream->chunk_buffer + bytes_filled,
                (size_t)this_run_bytes);
            if (error_code != MFTSCAN_OK) {
                return error_code;
            }
        }

        bytes_filled += this_run_bytes;
    }

    records_filled = bytes_filled / bytes_per_record;
    stream->chunk_first_record = first_record;
    stream->chunk_valid_records = (size_t)records_filled;

    /* Apply USA fix-up to each record once, while the chunk is fresh. Records
     * that fail USA (torn write / corruption) get their signature cleared so
     * that mftscan_parse_file_record skips them as not-in-use. */
    {
        size_t record_index = 0;
        for (record_index = 0; record_index < (size_t)records_filled; ++record_index) {
            uint8_t *rec = stream->chunk_buffer + record_index * bytes_per_record;
            MftscanStreamFileRecordHeader *rec_header =
                (MftscanStreamFileRecordHeader *)rec;
            bool torn_write = false;

            if (rec_header->signature != MFTSCAN_FILE_RECORD_SIGNATURE) {
                continue;
            }

            if (mftscan_apply_update_sequence_array(
                    rec,
                    bytes_per_record,
                    volume_handle->bytes_per_sector,
                    &torn_write) != MFTSCAN_OK) {
                rec_header->signature = 0U;
                mftscan_set_error_detail("");
            }
            (void)torn_write;
        }
    }
    return MFTSCAN_OK;
}

static MftscanError mftscan_stream_get_via_ioctl(
    MftscanRecordStream *stream,
    uint64_t record_number,
    uint8_t **record_buffer,
    size_t *record_length,
    bool *available) {
    uint64_t actual_record = 0ULL;
    bool reached_end = false;
    MftscanError error_code = MFTSCAN_OK;

    *available = false;
    *record_buffer = NULL;
    *record_length = 0;

    error_code = mftscan_read_file_record(
        stream->volume_handle,
        record_number,
        stream->fallback_buffer,
        stream->fallback_buffer_size,
        &actual_record,
        &reached_end);
    if (error_code != MFTSCAN_OK) {
        return error_code;
    }
    if (reached_end || actual_record != record_number) {
        return MFTSCAN_OK;
    }

    *record_buffer = stream->fallback_buffer->FileRecordBuffer;
    *record_length = stream->volume_handle->bytes_per_file_record;
    *available = true;
    return MFTSCAN_OK;
}

MftscanError mftscan_record_stream_get(
    MftscanRecordStream *stream,
    uint64_t record_number,
    uint8_t **record_buffer,
    size_t *record_length,
    bool *available) {
    uint32_t bytes_per_record = 0;
    uint64_t chunk_first = 0ULL;
    size_t offset_in_chunk = 0;
    MftscanError error_code = MFTSCAN_OK;

    if (stream == NULL || record_buffer == NULL ||
        record_length == NULL || available == NULL) {
        return MFTSCAN_ERROR_INVALID_ARGUMENT;
    }

    *record_buffer = NULL;
    *record_length = 0;
    *available = false;

    if (!stream->runlist_initialized) {
        return mftscan_stream_get_via_ioctl(
            stream, record_number, record_buffer, record_length, available);
    }

    if (record_number >= stream->total_records) {
        return MFTSCAN_OK;
    }

    bytes_per_record = stream->volume_handle->bytes_per_file_record;
    chunk_first = (record_number / stream->chunk_records) * stream->chunk_records;

    if (stream->chunk_first_record != chunk_first) {
        error_code = mftscan_stream_load_chunk(stream, chunk_first);
        if (error_code != MFTSCAN_OK) {
            /* On a streaming I/O failure, fall back to IOCTL for this record
             * rather than aborting the whole scan. */
            mftscan_set_error_detail("");
            return mftscan_stream_get_via_ioctl(
                stream, record_number, record_buffer, record_length, available);
        }
    }

    offset_in_chunk = (size_t)(record_number - stream->chunk_first_record);
    if (offset_in_chunk >= stream->chunk_valid_records) {
        return MFTSCAN_OK;
    }

    *record_buffer = stream->chunk_buffer + offset_in_chunk * bytes_per_record;
    *record_length = bytes_per_record;
    *available = true;
    return MFTSCAN_OK;
}
