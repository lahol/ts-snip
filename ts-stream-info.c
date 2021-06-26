#include "ts-stream-info.h"

#include <ts-analyzer.h>

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <memory.h>

#include <bitstream/mpeg/ts.h>
#include <bitstream/mpeg/pes.h>

struct _TsStreamInfo {
    PidInfoManager *pmgr;
    uint32_t analyzer_client_id;
    uint32_t random_access_client_id;

    FILE *file;
    size_t file_size;

    GArray *frame_infos;
    guint32 iframe_count;
};

/* In own module? */
typedef struct _PESData {
    size_t packet_start;
    size_t packet_end;
    GByteArray *data;
    uint64_t pts;
    uint64_t dts;

    uint32_t have_start : 1;
    uint32_t complete : 1;
    uint32_t have_pts : 1;
    uint32_t have_dts : 1;
} PESData;

PESData *pes_data_new()
{
    PESData *pes = malloc(sizeof(PESData));
    memset(pes, 0, sizeof(PESData));
    pes->data = g_byte_array_sized_new(65536);
    return pes;
}

void pes_data_free(PESData *pes)
{
    if (pes) {
        g_byte_array_free(pes->data, TRUE);
        free(pes);
    }
}

void pes_data_clear(PESData *pes)
{
    if (pes) {
        pes->packet_start = 0;
        pes->packet_end = 0;
        pes->pts = 0;
        pes->dts = 0;

        pes->have_start = 0;
        pes->complete = 0;
        pes->have_pts = 0;
        pes->have_dts = 0;

        g_byte_array_remove_range(pes->data, 0, pes->data->len);
    }
}

void pes_data_analyze_video_13818(PESData *pes, TsStreamInfo *tsi)
{
    if (!pes->have_start) {
        return;
    }

    size_t bytes_left = pes->data->len;
    uint8_t *data = pes->data->data;

    while (bytes_left > 5) {
        if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01 && data[3] == 0x00) { /* pic start */
            uint8_t pictype = ((data[5] >> 3) & 0x7);

            if (pictype == 1) {
                /* Found start of I frame */
                PESFrameInfo frame_info = {
                    .frame_number = tsi->iframe_count++,
                    .stream_offset_start = pes->packet_start,
                    .stream_offset_end = pes->packet_end,
                    .pts = pes->pts,
                    .dts = pes->dts,
                    .pidtype = PID_TYPE_VIDEO_13818
                };
                g_array_append_val(tsi->frame_infos, frame_info);
                break;
            }
        }

        ++data;
        --bytes_left;
    }
}

void pes_data_analyze_video_14496(PESData *pes, TsStreamInfo *tsi)
{
    if (!pes->have_start)
        return;

    size_t bytes_left = pes->data->len;
    uint8_t *data = pes->data->data;

    while (bytes_left > 3) {
        if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01 && (data[3] & 0x1f) == 5) {
            /* IDR image start */
            PESFrameInfo frame_info = {
                .frame_number = tsi->iframe_count++,
                .stream_offset_start = pes->packet_start,
                .stream_offset_end = pes->packet_end,
                .pts = pes->pts,
                .dts = pes->dts,
                .pidtype = PID_TYPE_VIDEO_14496
            };
            g_array_append_val(tsi->frame_infos, frame_info);
            break;
        }

        ++data;
        --bytes_left;
    }
}

typedef void (*PESFinishedFunc)(PESData *, void *);

static void _tsi_handle_pes(PidInfo *pidinfo,
                            uint32_t client_id,
                            const uint8_t *packet,
                            const size_t offset,
                            PESFinishedFunc finished_cb,
                            void *cb_data)
{
    PESData *pes = NULL;

    pes = pid_info_get_private_data(pidinfo, client_id);
    if (!pes) {
        pes = pes_data_new();
        pid_info_set_private_data(pidinfo, client_id, pes, (PidInfoPrivateDataFree)pes_data_free);
    }

    size_t pes_offset = 4;
    if (ts_has_adaptation(packet)) {
        pes_offset += 1 + packet[4];
    }
    size_t pes_data_len = 0;
    uint8_t *pes_data = (uint8_t *)&packet[pes_offset];

    if (ts_get_unitstart(packet)) {
        /* Finish last packet. */
        pes->complete = 1;
        pes->packet_end = offset;
        if (finished_cb)
            finished_cb(pes, cb_data);

        /* Setup new packet. */
        pes_data_clear(pes);
        pes->packet_start = offset;
        pes->have_start = 1;
        if (pes_has_pts(pes_data)) {
            pes->pts = pes_get_pts(pes_data);
            pes->have_pts = 1;
        }
        if (pes_has_dts(pes_data)) {
            pes->dts = pes_get_dts(pes_data);
            pes->have_dts = 1;
        }

        pes_data_len = 188 - PES_HEADER_SIZE - PES_HEADER_OPTIONAL_SIZE - pes_get_headerlength(pes_data) - pes_offset;

        pes_data = pes_payload(pes_data);
    }
    else {
        pes_data_len = 188 - pes_offset;
    }

    g_byte_array_append(pes->data, pes_data, pes_data_len);
}

static bool tsi_handle_packet(PidInfo *pidinfo, const uint8_t *packet, const size_t offset, TsStreamInfo *tsi)
{
    if (!pidinfo || !tsi)
        return true;

    if (pidinfo->type == PID_TYPE_VIDEO_13818) {
        _tsi_handle_pes(pidinfo, tsi->analyzer_client_id, packet, offset,
                (PESFinishedFunc)pes_data_analyze_video_13818, tsi);
    }
    else if (pidinfo->type == PID_TYPE_VIDEO_14496) {
        _tsi_handle_pes(pidinfo, tsi->analyzer_client_id, packet, offset,
                (PESFinishedFunc)pes_data_analyze_video_14496, tsi);
    }

    return true;
}

bool tsi_open_file(TsStreamInfo *tsi, const char *filename)
{
    struct stat st;
    if (stat(filename, &st) != 0)
        return false;

    if ((tsi->file = fopen(filename, "r")) == NULL) {
        perror("Could not open file");
        return false;
    }

    tsi->file_size = st.st_size;

    return true;
}

void tsi_close_file(TsStreamInfo *tsi)
{
    if (tsi->file)
        fclose(tsi->file);
    tsi->file = NULL;
}

void tsi_analyze_file(TsStreamInfo *tsi)
{
    if (!tsi->file)
        return;

    static TsAnalyzerClass tscls = {
        .handle_packet = (TsHandlePacketFunc)tsi_handle_packet,
    };
    TsAnalyzer *ts_analyzer = ts_analyzer_new(&tscls, tsi);

    ts_analyzer_set_pid_info_manager(ts_analyzer, tsi->pmgr);

    uint8_t buffer[8*4096];
    size_t bytes_read;

    /* read from file */
    while (!feof(tsi->file)) {
        bytes_read = fread(buffer, 1, 8*4096, tsi->file);
        if (bytes_read == 0) {
            fprintf(stderr, "Error reading buffer.\n");
            break;
        }
        if (bytes_read > 0) {
            ts_analyzer_push_buffer(ts_analyzer, buffer, bytes_read);
        }
    }

    ts_analyzer_free(ts_analyzer);
}

TsStreamInfo *ts_stream_info_new(const gchar *filename)
{
    TsStreamInfo *tsi = g_malloc0(sizeof(TsStreamInfo));
    if (!tsi_open_file(tsi, filename))
        goto err;

    tsi->pmgr = pid_info_manager_new();
    tsi->analyzer_client_id = pid_info_manager_register_client(tsi->pmgr);
    tsi->random_access_client_id = pid_info_manager_register_client(tsi->pmgr);

    tsi->frame_infos = g_array_sized_new(FALSE, /* zero-terminated? */
                                   TRUE,  /* Clear when allocated? */
                                   sizeof(PESFrameInfo), /* Size of single element */
                                   1024 /* preallocated number of elements */);

    tsi_analyze_file(tsi);

    return tsi;

err:
    ts_stream_info_destroy(tsi);
    return NULL;
}

void ts_stream_info_destroy(TsStreamInfo *tsi)
{
    if (tsi) {
        tsi_close_file(tsi);
    }
}

guint32 ts_stream_info_get_iframe_count(TsStreamInfo *tsi)
{
    return tsi->iframe_count;
}

bool ts_stream_info_get_iframe_info(TsStreamInfo *tsi, PESFrameInfo *frame_info, guint32 frame_id)
{
    if (!tsi || tsi->iframe_count <= frame_id)
        return false;
    if (frame_info)
        *frame_info = g_array_index(tsi->frame_infos, PESFrameInfo, frame_id);
    return true;
}

struct FindIFrameInfo {
    TsStreamInfo *tsi;
    gboolean package_found;

    uint8_t *pes_data;
    size_t pes_size;
};

void _ts_get_iframe_handle_pes(PESData *pes, struct FindIFrameInfo *fifi)
{
    if (!fifi->package_found && pes->data->len > 0 && pes->have_start && pes->complete) {
        fifi->package_found = true;
        fifi->pes_data = g_byte_array_steal(pes->data, &fifi->pes_size);
    }
}

static bool _ts_get_iframe_handle_packet(PidInfo *pidinfo, const uint8_t *packet, const size_t offset, struct FindIFrameInfo *fifi)
{
    if (!pidinfo || !fifi)
        return false;

    if (pidinfo->type == PID_TYPE_VIDEO_13818 || pidinfo->type == PID_TYPE_VIDEO_14496) {
        _tsi_handle_pes(pidinfo, fifi->tsi->random_access_client_id, packet, offset,
                (PESFinishedFunc)_ts_get_iframe_handle_pes, fifi);
        if (fifi->package_found) {
            pid_info_clear_private_data(pidinfo, fifi->tsi->random_access_client_id);
            return false;
        }
    }
    return true;
}

void ts_stream_info_get_iframe(TsStreamInfo *tsi, guint8 **data, gsize *length, guint32 frame_id)
{
    if (!data)
        return;
    if (!tsi || tsi->iframe_count <= frame_id || !tsi->file) {
        *data = NULL;
        if (length) *length = 0;
    }

    PESFrameInfo *frame_info = &g_array_index(tsi->frame_infos, PESFrameInfo, frame_id);

    struct FindIFrameInfo fifi;
    memset(&fifi, 0, sizeof(struct FindIFrameInfo));
    fifi.tsi = tsi;

    static TsAnalyzerClass tscls = {
        .handle_packet = (TsHandlePacketFunc)_ts_get_iframe_handle_packet
    };
    TsAnalyzer *ts_analyzer = ts_analyzer_new(&tscls, &fifi);
    ts_analyzer_set_pid_info_manager(ts_analyzer, tsi->pmgr);

    uint8_t buffer[8*4096];
    size_t bytes_read;

    fseek(tsi->file, frame_info->stream_offset_start, SEEK_SET);

    while (!feof(tsi->file) && !fifi.package_found) {
        bytes_read = fread(buffer, 1, 8*4096, tsi->file);
        if (bytes_read == 0) {
            break;
        }
        if (bytes_read > 0) {
            ts_analyzer_push_buffer(ts_analyzer, buffer, bytes_read);
        }
    }

    ts_analyzer_free(ts_analyzer);

    if (fifi.package_found) {
        *data = fifi.pes_data;
        if (length) *length = fifi.pes_size;
    }
    else {
        /* TODO Reset private data. */
    }
}
