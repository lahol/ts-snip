#include "ts-snipper.h"

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

typedef struct {
    GList *slices; /**< [TsSlice *] */
    GList *active_slice; /**< Pointer to next/current slice in slices. */
    guint64 next_slice_id;

    TsSnipperWriteFunc writer;
    gpointer writer_data;
    gboolean writer_result;

    gsize bytes_read;

    guint8 *buffer;
    gsize buffer_size;
    gsize buffer_filled;

    guint32 have_pat : 1; /* To not accidentally ignore pat/pmt */
    guint32 have_pmt : 1;
} TsSnipperOutput;

struct _TsSnipper {
    PidInfoManager *pmgr;
    uint32_t analyzer_client_id;
    uint32_t random_access_client_id;

    FILE *file;
    gsize file_size;
    gsize bytes_read;

    GArray *frame_infos;
    guint32 iframe_count;

    TsSnipperOutput out;

    GMutex file_lock;
    GMutex data_lock;
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

void pes_data_analyze_video_13818(PESData *pes, TsSnipper *tsn)
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
                    .frame_number = tsn->iframe_count++,
                    .stream_offset_start = pes->packet_start,
                    .stream_offset_end = pes->packet_end,
                    .pts = pes->pts,
                    .dts = pes->dts,
                    .pidtype = PID_TYPE_VIDEO_13818
                };
                g_mutex_lock(&tsn->data_lock);
                g_array_append_val(tsn->frame_infos, frame_info);
                g_mutex_unlock(&tsn->data_lock);
                break;
            }
        }

        ++data;
        --bytes_left;
    }
}

void pes_data_analyze_video_14496(PESData *pes, TsSnipper *tsn)
{
    if (!pes->have_start)
        return;

    size_t bytes_left = pes->data->len;
    uint8_t *data = pes->data->data;

    while (bytes_left > 3) {
        if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01 && (data[3] & 0x1f) == 5) {
            /* IDR image start */
            PESFrameInfo frame_info = {
                .frame_number = tsn->iframe_count++,
                .stream_offset_start = pes->packet_start,
                .stream_offset_end = pes->packet_end,
                .pts = pes->pts,
                .dts = pes->dts,
                .pidtype = PID_TYPE_VIDEO_14496
            };
            g_mutex_lock(&tsn->data_lock);
            g_array_append_val(tsn->frame_infos, frame_info);
            g_mutex_unlock(&tsn->data_lock);
            break;
        }

        ++data;
        --bytes_left;
    }
}

typedef gboolean (*TsnResumeCallback)(gpointer);

/* Dummy to always resume (and not check every time for pointer). */
static gboolean _tsn_resume_true(gpointer nil)
{
    return TRUE;
}

#define TSN_READ_BUFFER_SIZE (32768)
static void tsn_read_buffered(TsSnipper *snipper,
                              TsAnalyzer *analyzer,
                              gsize start_offset,
                              TsnResumeCallback resume,
                              gpointer resume_data)
{
    g_return_if_fail(snipper != NULL);
    g_return_if_fail(analyzer != NULL);
    g_return_if_fail(snipper->file != NULL);
    g_return_if_fail(start_offset < snipper->file_size);

    if (resume == NULL)
        resume = _tsn_resume_true;

    g_mutex_lock(&snipper->file_lock);
    fseek(snipper->file, start_offset, SEEK_SET);

    guint8 buffer[TSN_READ_BUFFER_SIZE];
    gsize bytes_read;

    while (!feof(snipper->file) && resume(resume_data)) {
        bytes_read = fread(buffer, 1, TSN_READ_BUFFER_SIZE, snipper->file);
        if (bytes_read == 0)
            break;
        ts_analyzer_push_buffer(analyzer, buffer, bytes_read);
    }
    g_mutex_unlock(&snipper->file_lock);
}

typedef void (*PESFinishedFunc)(PESData *, gpointer);

static void _tsn_handle_pes(PidInfo *pidinfo,
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

static bool tsn_handle_packet(PidInfo *pidinfo, const uint8_t *packet, const size_t offset, TsSnipper *tsn)
{
    if (!tsn)
        return true;
    tsn->bytes_read = offset;
    if (!pidinfo)
        return true;

    if (pidinfo->type == PID_TYPE_VIDEO_13818) {
        _tsn_handle_pes(pidinfo, tsn->analyzer_client_id, packet, offset,
                (PESFinishedFunc)pes_data_analyze_video_13818, tsn);
    }
    else if (pidinfo->type == PID_TYPE_VIDEO_14496) {
        _tsn_handle_pes(pidinfo, tsn->analyzer_client_id, packet, offset,
                (PESFinishedFunc)pes_data_analyze_video_14496, tsn);
    }

    return true;
}

bool tsn_open_file(TsSnipper *tsn, const char *filename)
{
    struct stat st;
    if (stat(filename, &st) != 0)
        return false;

    if ((tsn->file = fopen(filename, "r")) == NULL) {
        perror("Could not open file");
        return false;
    }

    tsn->file_size = st.st_size;

    return true;
}

void tsn_close_file(TsSnipper *tsn)
{
    g_mutex_lock(&tsn->file_lock);
    if (tsn->file)
        fclose(tsn->file);
    tsn->file = NULL;
    g_mutex_unlock(&tsn->file_lock);
}

void tsn_analyze_file(TsSnipper *tsn)
{
    if (!tsn->file)
        return;

    static TsAnalyzerClass tscls = {
        .handle_packet = (TsHandlePacketFunc)tsn_handle_packet,
    };
    TsAnalyzer *ts_analyzer = ts_analyzer_new(&tscls, tsn);

    ts_analyzer_set_pid_info_manager(ts_analyzer, tsn->pmgr);

    tsn_read_buffered(tsn,
                      ts_analyzer,
                      0,
                      NULL,
                      NULL);

    ts_analyzer_free(ts_analyzer);
}

TsSnipper *ts_snipper_new(const gchar *filename)
{
    TsSnipper *tsn = g_malloc0(sizeof(TsSnipper));
    if (!tsn_open_file(tsn, filename))
        goto err;

    tsn->pmgr = pid_info_manager_new();
    tsn->analyzer_client_id = pid_info_manager_register_client(tsn->pmgr);
    tsn->random_access_client_id = pid_info_manager_register_client(tsn->pmgr);

    tsn->frame_infos = g_array_sized_new(FALSE, /* zero-terminated? */
                                   TRUE,  /* Clear when allocated? */
                                   sizeof(PESFrameInfo), /* Size of single element */
                                   1024 /* preallocated number of elements */);

    g_mutex_init(&tsn->file_lock);
    g_mutex_init(&tsn->data_lock);

    return tsn;

err:
    ts_snipper_destroy(tsn);
    return NULL;
}

void ts_snipper_destroy(TsSnipper *tsn)
{
    if (tsn) {
        tsn_close_file(tsn);
    }
}

gboolean ts_snipper_get_analyze_status(TsSnipper *tsn, gsize *bytes_read, gsize *bytes_total)
{
    g_return_val_if_fail(tsn != NULL, FALSE);
    if (bytes_read) *bytes_read = tsn->bytes_read;
    if (bytes_total) *bytes_total = tsn->file_size;
    return TRUE;
}

gboolean ts_snipper_get_write_status(TsSnipper *tsn, gsize *bytes_read, gsize *bytes_total)
{
    g_return_val_if_fail(tsn != NULL, FALSE);
    if (bytes_read) *bytes_read = tsn->out.bytes_read;
    if (bytes_total) *bytes_total = tsn->file_size;
    return TRUE;
}

void ts_snipper_analyze(TsSnipper *tsn)
{
    tsn_analyze_file(tsn);
}

guint32 ts_snipper_get_iframe_count(TsSnipper *tsn)
{
    return tsn->iframe_count;
}

bool ts_snipper_get_iframe_info(TsSnipper *tsn, PESFrameInfo *frame_info, guint32 frame_id)
{
    if (!tsn || tsn->iframe_count <= frame_id)
        return false;
    if (frame_info) {
        g_mutex_lock(&tsn->data_lock);
        *frame_info = g_array_index(tsn->frame_infos, PESFrameInfo, frame_id);
        g_mutex_unlock(&tsn->data_lock);
    }
    return true;
}

struct FindIFrameInfo {
    TsSnipper *tsn;
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
        _tsn_handle_pes(pidinfo, fifi->tsn->random_access_client_id, packet, offset,
                (PESFinishedFunc)_ts_get_iframe_handle_pes, fifi);
        if (fifi->package_found) {
            pid_info_clear_private_data(pidinfo, fifi->tsn->random_access_client_id);
            return false;
        }
    }
    return true;
}

/* Check if an I frame was found or if the search continues. */
static gboolean _ts_fifi_resume(struct FindIFrameInfo *fifi)
{
    return !fifi->package_found;
}

void ts_snipper_get_iframe(TsSnipper *tsn, guint8 **data, gsize *length, guint32 frame_id)
{
    if (!data)
        return;
    if (!tsn || tsn->iframe_count <= frame_id || !tsn->file) {
        *data = NULL;
        if (length) *length = 0;
    }

    g_mutex_lock(&tsn->data_lock);
    PESFrameInfo *frame_info = &g_array_index(tsn->frame_infos, PESFrameInfo, frame_id);
    g_mutex_unlock(&tsn->data_lock);

    struct FindIFrameInfo fifi;
    memset(&fifi, 0, sizeof(struct FindIFrameInfo));
    fifi.tsn = tsn;

    static TsAnalyzerClass tscls = {
        .handle_packet = (TsHandlePacketFunc)_ts_get_iframe_handle_packet
    };
    TsAnalyzer *ts_analyzer = ts_analyzer_new(&tscls, &fifi);
    ts_analyzer_set_pid_info_manager(ts_analyzer, tsn->pmgr);

    tsn_read_buffered(tsn,
                      ts_analyzer,
                      frame_info->stream_offset_start,
                      (TsnResumeCallback)_ts_fifi_resume,
                      &fifi);

    ts_analyzer_free(ts_analyzer);

    if (fifi.package_found) {
        *data = fifi.pes_data;
        if (length) *length = fifi.pes_size;
    }
    else {
        /* TODO Reset private data. */
    }
}

static gint ts_slice_compare(TsSlice *a, TsSlice *b)
{
    if (b->begin > a->begin)
        return -1;
    return (gint)(a->begin - b->begin);
}

guint64 ts_snipper_add_slice(TsSnipper *tsn, guint32 frame_begin, guint32 frame_end)
{
    /* FIXME Handle overlapping slices. */
    PESFrameInfo fi_begin;
    PESFrameInfo fi_end;
    if (frame_begin == (guint32)(-1)) {
        fi_begin.stream_offset_start = 0;
        fi_begin.pts = (guint64)(-1);
    }
    else if (!ts_snipper_get_iframe_info(tsn, &fi_begin, frame_begin)) {
        return -1;
    }
    if (frame_end == (guint32)(-1)) {
        fi_end.stream_offset_start = tsn->file_size;
        fi_end.pts = (guint64)(-1);
    }
    else if (!ts_snipper_get_iframe_info(tsn, &fi_end, frame_end)) {
        return -1;
    }

    TsSlice *slice = g_new(TsSlice, 1);
    slice->begin = fi_begin.stream_offset_start;
    slice->begin_frame = frame_begin;
    slice->pts_begin = fi_begin.pts;

    slice->end = fi_end.stream_offset_start;
    slice->end_frame = frame_end;
    slice->pts_end = fi_end.pts;

    g_mutex_lock(&tsn->data_lock);
    slice->id = tsn->out.next_slice_id++;

    tsn->out.slices = g_list_insert_sorted(tsn->out.slices, slice, (GCompareFunc)ts_slice_compare);
    g_mutex_unlock(&tsn->data_lock);

    return slice->id;
}

static gint _ts_snipper_slice_compare_id(TsSlice *slice, guint64* id)
{
    return (slice->id == *id) ? 0 : 1;
}

void ts_snipper_delete_slice(TsSnipper *tsn, guint64 id)
{
    g_return_if_fail(tsn != NULL);
    g_mutex_lock(&tsn->data_lock);
    GList *rmlink = g_list_find_custom(tsn->out.slices, (GCompareFunc)_ts_snipper_slice_compare_id, (gpointer)&id);
    if (rmlink) {
        g_free(rmlink->data);
        tsn->out.slices = g_list_delete_link(tsn->out.slices, rmlink);
    }
    g_mutex_unlock(&tsn->data_lock);
}

void ts_snipper_enum_slices(TsSnipper *tsn, TsSnipperEnumSlicesFunc callback, gpointer userdata)
{
    if (!callback)
        return;
    GList *tmp;
    gboolean cbres;
    TsSlice slice;
    g_mutex_lock(&tsn->data_lock);
    for (tmp = tsn->out.slices; tmp; tmp = g_list_next(tmp)) {
        slice = *((TsSlice *)tmp->data);
        g_mutex_unlock(&tsn->data_lock);
        cbres = callback(&slice, userdata);
        g_mutex_lock(&tsn->data_lock);
        if (!cbres)
            break;
    }
    g_mutex_unlock(&tsn->data_lock);
}

/* Check whether the given offset is inside the active slice. Move active slice forward, if necessary. */
static gboolean tsn_check_offset_in_slice(TsSnipperOutput *tso, const size_t offset)
{
    /* Advance active slice, if necessary.
     * End of active slice has to be after offset.
     * Begin can be before or after, depending whether we are in the slice or not. */
    while (tso->active_slice &&
            ((TsSlice *)tso->active_slice->data)->end < offset) {
        tso->active_slice = g_list_next(tso->active_slice);
    }
    if (!tso->active_slice)
        return FALSE;

    return (offset >= ((TsSlice *)tso->active_slice->data)->begin);
}

static bool tsn_output_handle_packet(PidInfo *pidinfo, const uint8_t *packet, const size_t offset, TsSnipperOutput *tso)
{
    /* if not in slice, or first PAT/PMT push to buffer. */
    gboolean write_packet = !tsn_check_offset_in_slice(tso, offset);
    tso->bytes_read = offset;

    /* Check that we do not ignore pat/pmt */
    if (!write_packet && pidinfo) {
        if (!tso->have_pat && pidinfo->type == PID_TYPE_PAT) {
            write_packet = TRUE;
            tso->have_pat = 1;
        }
        else if (!tso->have_pmt && pidinfo->type == PID_TYPE_PMT) {
            write_packet = TRUE;
            tso->have_pmt = 1;
        }
    }

    if (!write_packet)
        return tso->writer_result;

    /* push packet to buffer */
    memcpy(tso->buffer + tso->buffer_filled, packet, TS_SIZE);
    tso->buffer_filled += TS_SIZE;

    /* Flush buffer to writer if there is no more space for another packet available. */
    if (tso->buffer_filled + TS_SIZE > tso->buffer_size) {
        tso->writer_result = tso->writer(tso->buffer, tso->buffer_filled, tso->writer_data);
        tso->buffer_filled = 0;
    }

    return tso->writer_result;
}

gboolean ts_snipper_write(TsSnipper *tsn, TsSnipperWriteFunc writer, gpointer userdata)
{
    if (!tsn || !writer || !tsn->file)
        return FALSE;

    /* read input, handle with tsn_output, write last bytes in buffer. */
    tsn->out.writer = writer;
    tsn->out.writer_data = userdata;
    tsn->out.writer_result = TRUE;
    tsn->out.buffer_size = TSN_READ_BUFFER_SIZE;
    tsn->out.buffer_filled = 0;
    tsn->out.buffer = g_malloc(tsn->out.buffer_size);
    tsn->out.have_pat = 0;
    tsn->out.have_pmt = 0;
    tsn->out.active_slice = tsn->out.slices;

    static TsAnalyzerClass tscls = {
        .handle_packet = (TsHandlePacketFunc)tsn_output_handle_packet
    };
    TsAnalyzer *ts_analyzer = ts_analyzer_new(&tscls, &tsn->out);

    ts_analyzer_set_pid_info_manager(ts_analyzer, tsn->pmgr);

    tsn_read_buffered(tsn,
                      ts_analyzer,
                      0,
                      NULL,
                      NULL);

    ts_analyzer_free(ts_analyzer);

    /* Write rest of buffer. */
    if (tsn->out.writer_result && tsn->out.buffer_filled > 0) {
        tsn->out.writer_result = writer(tsn->out.buffer, tsn->out.buffer_filled, userdata);
    }

    tsn->out.buffer_size = 0;
    g_free(tsn->out.buffer);
    tsn->out.buffer = NULL;

    return tsn->out.writer_result;
}
