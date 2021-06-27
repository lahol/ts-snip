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

/* Actions for data stretched over multiple packets. */
typedef enum {
    WPAIgnoreUntilUnitStart = 0,
    WPAWrite = 1,
    WPAWriteUntilUnitStart = 2,
    WPAIgnore = 3
} WriterPidAction;

typedef struct {
    GList *slices; /**< [TsSlice *] */
    GList *active_slice; /**< Pointer to next/current slice in slices. */
    guint32 next_slice_id;

    guint32 writer_client_id;

    TsSnipperWriteFunc writer;
    gpointer writer_data;
    gboolean writer_result;

    gsize bytes_read;

    guint8 *buffer;
    gsize buffer_size;
    gsize buffer_filled;

    guint32 have_pat : 1; /* To not accidentally ignore pat/pmt */
    guint32 have_pmt : 1;
    guint32 in_slice : 1; /* Whether we are inside a slice or not. */

    WriterPidAction *pid_actions; /* When writing starts, set the actions for all pids. */
    gsize pids_seen;
    gsize pid_count;

    guint64 pts_delta;
    guint64 pcr_delta;
/*    guint64 dts_delta;*/
} TsSnipperOutput;

struct _TsSnipper {
    PidInfoManager *pmgr;
    uint32_t analyzer_client_id;
    uint32_t random_access_client_id;
    TsSnipperState state;

    FILE *file;
    gsize file_size;
    gsize bytes_read;

    GArray *frame_infos;
    guint32 iframe_count;
    guint16 video_pid;

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
    uint64_t pcr;

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
    pes->pts = PES_FRAME_TS_INVALID;
    pes->dts = PES_FRAME_TS_INVALID;
    pes->pcr = PES_FRAME_TS_INVALID;
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
        pes->pts = PES_FRAME_TS_INVALID;
        pes->dts = PES_FRAME_TS_INVALID;
        pes->pcr = PES_FRAME_TS_INVALID;

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
                    .pcr = pes->pcr,
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
                .pcr = pes->pcr,
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
    guint64 pcr = PES_FRAME_TS_INVALID;
    if (ts_has_adaptation(packet)) {
        pes_offset += 1 + packet[4];
        if (tsaf_has_pcr(packet)) {
            pcr = tsaf_get_pcr(packet) * 300 + tsaf_get_pcrext(packet);
        }
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
        pes->pcr = pcr;
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
        if (!tsn->video_pid)
            tsn->video_pid = pidinfo->pid;
        _tsn_handle_pes(pidinfo, tsn->analyzer_client_id, packet, offset,
                (PESFinishedFunc)pes_data_analyze_video_13818, tsn);
    }
    else if (pidinfo->type == PID_TYPE_VIDEO_14496) {
        if (!tsn->video_pid)
            tsn->video_pid = pidinfo->pid;
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

    tsn->state = TsSnipperStateAnalyzing;

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

    tsn->state = TsSnipperStateReady;
}

TsSnipper *ts_snipper_new(const gchar *filename)
{
    TsSnipper *tsn = g_malloc0(sizeof(TsSnipper));
    if (!tsn_open_file(tsn, filename))
        goto err;

    tsn->pmgr = pid_info_manager_new();
    tsn->analyzer_client_id = pid_info_manager_register_client(tsn->pmgr);
    tsn->random_access_client_id = pid_info_manager_register_client(tsn->pmgr);
    tsn->out.writer_client_id = pid_info_manager_register_client(tsn->pmgr);

    tsn->frame_infos = g_array_sized_new(FALSE, /* zero-terminated? */
                                   TRUE,  /* Clear when allocated? */
                                   sizeof(PESFrameInfo), /* Size of single element */
                                   1024 /* preallocated number of elements */);

    g_mutex_init(&tsn->file_lock);
    g_mutex_init(&tsn->data_lock);

    tsn->state = TsSnipperStateInitialized;

    return tsn;

err:
    ts_snipper_destroy(tsn);
    return NULL;
}

void ts_snipper_destroy(TsSnipper *tsn)
{
    if (tsn) {
        tsn_close_file(tsn);

        g_free(tsn);
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
    if (tsn && (tsn->state == TsSnipperStateInitialized || tsn->state == TsSnipperStateReady)) {
        tsn_analyze_file(tsn);
    }
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
        /* Reset private data. */
        pid_info_manager_clear_private_data(tsn->pmgr, tsn->random_access_client_id);
    }
}

static gint ts_slice_compare(TsSlice *a, TsSlice *b)
{
    if (b->begin > a->begin)
        return -1;
    return (gint)(a->begin - b->begin);
}

/* Merge overlapping slices. */
void ts_snipper_merge_slices(TsSnipper *tsn)
{
    g_mutex_lock(&tsn->data_lock);

    GList *linkA, *linkB;
    TsSlice *A, *B;
    /* Slices are always sorted such that A.begin <= B.begin
     * If B.begin > A.end => nothing to do, proceed with next pair.
     * If B.begin <= A.end => merge slices (min(A.begin,B.begin)=A.begin, max(A.end,B.end))*/
    linkA = tsn->out.slices;
    while (linkA != NULL && (linkB = g_list_next(linkA)) != NULL) {
        A = (TsSlice *)linkA->data;
        B = (TsSlice *)linkB->data;

        if (B->begin > A->end) {
            linkA = linkB;
        }
        else {
            if (A->end < B->end) {
                /* copy B end information to A end. */
                A->end = B->end;
                A->end_frame = B->end_frame;
                A->pts_end = B->pts_end;
                A->pcr_end = B->pcr_end;
            }
            g_free(B);
            tsn->out.slices = g_list_delete_link(tsn->out.slices, linkB);
        }
    }

    g_mutex_unlock(&tsn->data_lock);
}

guint32 ts_snipper_add_slice(TsSnipper *tsn, guint32 frame_begin, guint32 frame_end)
{
    /* FIXME Handle overlapping slices. */
    PESFrameInfo fi_begin;
    PESFrameInfo fi_end;
    if (frame_begin == PES_FRAME_ID_INVALID) {
        fi_begin.stream_offset_start = 0;
        fi_begin.pts = PES_FRAME_TS_INVALID;
        fi_begin.pcr = PES_FRAME_TS_INVALID;
    }
    else if (!ts_snipper_get_iframe_info(tsn, &fi_begin, frame_begin) && frame_begin != tsn->iframe_count) {
        return -1;
    }
    else if (frame_begin == tsn->iframe_count) {
        if (!ts_snipper_get_iframe_info(tsn, &fi_begin, frame_begin - 1))
            return -1;
        fprintf(stderr, "set slice for end %zu -> %zu\n", fi_begin.stream_offset_start, fi_begin.stream_offset_end);
        fi_begin.stream_offset_start = fi_begin.stream_offset_end;
        fi_begin.pts = PES_FRAME_ID_INVALID;
        fi_begin.pcr = PES_FRAME_TS_INVALID;
    }
    if (frame_end == PES_FRAME_ID_INVALID) {
        fi_end.stream_offset_start = tsn->file_size;
        fi_end.pts = PES_FRAME_TS_INVALID;
        fi_end.pcr = PES_FRAME_TS_INVALID;
    }
    else if (!ts_snipper_get_iframe_info(tsn, &fi_end, frame_end)) {
        return -1;
    }

    TsSlice *slice = g_new(TsSlice, 1);
    slice->begin = fi_begin.stream_offset_start;
    slice->begin_frame = frame_begin;
    slice->pts_begin = fi_begin.pts;
    slice->pcr_begin = fi_begin.pcr;

    slice->end = fi_end.stream_offset_start;
    slice->end_frame = frame_end;
    slice->pts_end = fi_end.pts;
    slice->pcr_end = fi_end.pcr;

    g_mutex_lock(&tsn->data_lock);
    guint32 slice_id = tsn->out.next_slice_id++;
    slice->id = slice_id; /* slice might become invalid after merging. */

    tsn->out.slices = g_list_insert_sorted(tsn->out.slices, slice, (GCompareFunc)ts_slice_compare);
    g_mutex_unlock(&tsn->data_lock);

    ts_snipper_merge_slices(tsn);

    return slice_id;
}

static gint _ts_snipper_slice_compare_frame_in_range(TsSlice *slice, guint32 frame_id)
{
    return (slice->begin_frame <= frame_id && frame_id < slice->end_frame) ? 0 : 1;
}

static gint _ts_snipper_slice_compare_frame_in_range_inclusive(TsSlice *slice, guint32 frame_id)
{
    return (slice->begin_frame <= frame_id && frame_id <= slice->end_frame) ? 0 : 1;
}

guint32 ts_snipper_find_slice_for_frame(TsSnipper *tsn, TsSlice *slice, guint32 frame_id, gboolean include_end)
{
    guint32 slice_id = TS_SLICE_ID_INVALID;
    g_return_val_if_fail(tsn != NULL, slice_id);
    g_mutex_lock(&tsn->data_lock);
    GList *slice_link = g_list_find_custom(
                             tsn->out.slices,
                             GUINT_TO_POINTER(frame_id),
                             include_end
                                ? (GCompareFunc)_ts_snipper_slice_compare_frame_in_range_inclusive
                                : (GCompareFunc)_ts_snipper_slice_compare_frame_in_range);
    if (slice_link) {
        if (slice) *slice = *((TsSlice *)slice_link->data);
        slice_id = ((TsSlice *)slice_link->data)->id;
    }
    g_mutex_unlock(&tsn->data_lock);

    return slice_id;
}

static gint _ts_snipper_slice_compare_id(TsSlice *slice, guint32 id)
{
    return (slice->id == id) ? 0 : 1;
}

void ts_snipper_delete_slice(TsSnipper *tsn, guint64 id)
{
    g_return_if_fail(tsn != NULL);
    g_mutex_lock(&tsn->data_lock);
    GList *rmlink = g_list_find_custom(tsn->out.slices,
                                       GUINT_TO_POINTER(id),
                                       (GCompareFunc)_ts_snipper_slice_compare_id);
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

static void tso_pid_actions_init(TsSnipperOutput *tso, PidInfoManager *pmgr)
{
    tso->pid_count = pid_info_manager_get_pid_count(pmgr);
    tso->pid_actions = g_new0(WriterPidAction, tso->pid_count);
    tso->pids_seen = 0;
}

static void tso_pid_actions_cleanup(TsSnipperOutput *tso, PidInfoManager *pmgr)
{
    pid_info_manager_clear_private_data(pmgr, tso->writer_client_id);
    g_free(tso->pid_actions);
    tso->pid_actions = NULL;
    tso->pid_count = 0;
}

static void tso_pid_actions_reset(TsSnipperOutput *tso, WriterPidAction action)
{
    gsize j;
    for (j = 0; j < tso->pid_count; ++j) {
        tso->pid_actions[j] = action;
    }
}

static WriterPidAction *tso_pid_actions_get_for_pid(TsSnipperOutput *tso, PidInfo *pidinfo)
{
    /* We could handle this without private data, but this way we gain faster access (no searching). */
    WriterPidAction *action = pid_info_get_private_data(pidinfo, tso->writer_client_id);
    if (action == NULL) {
        /* pids_seen cannot be larger than pid_count */
        /* First occurrence of this pid */
        action = &tso->pid_actions[tso->pids_seen++];
        pid_info_set_private_data(pidinfo, tso->writer_client_id, action, NULL);
    }

    return action;
}

static gboolean tso_packet_is_pes(PidInfo *pidinfo)
{
    return (pidinfo &&
            (pidinfo->type == PID_TYPE_VIDEO_14496 ||
             pidinfo->type == PID_TYPE_VIDEO_13818 ||
             pidinfo->type == PID_TYPE_VIDEO_11172 ||
             pidinfo->type == PID_TYPE_AUDIO_13818 ||
             pidinfo->type == PID_TYPE_AUDIO_11172));
}

static void tso_update_pts_delta(TsSnipperOutput *tso, gsize offset)
{
    if (tso->active_slice && ((TsSlice *)tso->active_slice->data)->begin == offset) {
        guint64 pts_delta = 0;
        guint64 pcr_delta = 0;
/*        guint64 dts_delta = 0;*/
        TsSlice *slice = (TsSlice *)tso->active_slice->data;
        if (slice->pts_begin != PES_FRAME_TS_INVALID &&
            slice->pts_end != PES_FRAME_TS_INVALID &&
            slice->pts_end > slice->pts_begin) {
            pts_delta = slice->pts_end - slice->pts_begin;
        }
        if (slice->pcr_begin != PES_FRAME_TS_INVALID &&
            slice->pcr_end != PES_FRAME_TS_INVALID &&
            slice->pcr_end > slice->pcr_begin) {
            pcr_delta = slice->pcr_end - slice->pcr_begin;
        }
/*        if (slice->dts_begin != PES_FRAME_TS_INVALID &&
            slice->dts_end != PES_FRAME_TS_INVALID &&
            slice->dts_end > slice->dts_begin) {
            dts_delta = slice->dts_end - slice->dts_begin;
        }*/

        if (pts_delta && pcr_delta) {
            tso->pts_delta += pts_delta;
            tso->pcr_delta += pcr_delta;
        }
        fprintf(stderr, "Update PTS delta @%zu: %" G_GUINT64_FORMAT " -> %" G_GUINT64_FORMAT "\n",
                offset, pts_delta, tso->pts_delta);
    }
}

static void tso_rewrite_pts(TsSnipperOutput *tso, PidInfo *pidinfo, uint8_t *packet)
{
#if 0
    /* edit pcr? */
    if (!tso_packet_is_pes(pidinfo))
        return;
    if (ts_get_unitstart(packet)) {
        size_t pes_offset = 4;
        if (ts_has_adaptation(packet)) {
            pes_offset += 1 + packet[4];
            if (tsaf_has_pcr(packet)) {
                guint64 pcr = tsaf_get_pcr(packet) * 300 + tsaf_get_pcrext(packet);
                pcr -= tso->pcr_delta;
                tsaf_set_pcr(packet, (pcr / 300) & 0x1ffffffff);
                tsaf_set_pcrext(packet, pcr % 300);
            }
        }
        if (pes_offset > 180)
            return;
        uint8_t *pes_data = &packet[pes_offset];
        if (pes_has_pts(pes_data)) {
            pes_set_pts(pes_data, pes_get_pts(pes_data) - tso->pts_delta);
        }
        if (pes_has_dts(pes_data)) {
            pes_set_dts(pes_data, pes_get_dts(pes_data) - tso->pts_delta);
        }
    }
#endif
}

static gboolean tso_should_write_packet(PidInfo *pidinfo, const uint8_t *packet, TsSnipperOutput *tso)
{
    if (!pidinfo)
        return TRUE;
    WriterPidAction *action = tso_pid_actions_get_for_pid(tso, pidinfo);

    if (!tso->in_slice) {
        if (*action == WPAIgnoreUntilUnitStart && ts_get_unitstart(packet)) {
            *action = WPAWrite;
        }
        /* Already set to write or NULL packet. */
        return (*action == WPAWrite || pidinfo->pid == 0x1FFF);
    }
    else {
        if (*action == WPAWriteUntilUnitStart && ts_get_unitstart(packet)) {
            *action = WPAIgnore;
        }
        return !(*action == WPAIgnore || pidinfo->pid == 0x1FFF);
    }
}

/* Check whether the given offset is inside the active slice. Move active slice forward, if necessary. */
static gboolean tsn_check_offset_in_slice(TsSnipperOutput *tso, const size_t offset)
{
    /* Advance active slice, if necessary.
     * End of active slice has to be after offset.
     * Begin can be before or after, depending whether we are in the slice or not. */
    while (tso->active_slice &&
            ((TsSlice *)tso->active_slice->data)->end <= offset) {
        tso->active_slice = g_list_next(tso->active_slice);
    }
    if (!tso->active_slice)
        return FALSE;

    return (offset >= ((TsSlice *)tso->active_slice->data)->begin);
}

static bool tsn_output_handle_packet(PidInfo *pidinfo, const uint8_t *packet, const size_t offset, TsSnipperOutput *tso)
{
    /* if not in slice, or first PAT/PMT push to buffer. */
    gboolean in_slice = tsn_check_offset_in_slice(tso, offset);
    if (tso->in_slice && !in_slice) {
        /* We were in a slice and are now out of it. */
        tso_pid_actions_reset(tso, WPAIgnoreUntilUnitStart);
        tso->in_slice = 0;
    }
    else if (!tso->in_slice && in_slice) {
        /* We changed from not in a slice to a slice. */
        tso_pid_actions_reset(tso, WPAWriteUntilUnitStart);
        tso->in_slice = 1;
    }
    gboolean write_packet = tso_should_write_packet(pidinfo, packet, tso);
    tso->bytes_read = offset;

    tso_update_pts_delta(tso, offset);

    /* Check that we do not ignore pat/pmt */
    if (pidinfo) {
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
    tso_rewrite_pts(tso, pidinfo, tso->buffer + tso->buffer_filled);

    tso->buffer_filled += TS_SIZE;

    /* Flush buffer to writer if there is no more space for another packet available. */
    if (tso->buffer_filled + TS_SIZE > tso->buffer_size) {
        tso->writer_result = tso->writer(tso->buffer, tso->buffer_filled, tso->writer_data);
        tso->buffer_filled = 0;
    }

    return tso->writer_result;
}

#if 0
void push_term_packet(TsSnipper *tsn)
{
    uint8_t packet[TS_SIZE];
    ts_init(packet);
    ts_set_unitstart(packet);
    ts_set_pid(packet, tsn->video_pid);
    ts_set_adaptation(packet, 183);

    memcpy(tsn->out.buffer + tsn->out.buffer_filled, packet, TS_SIZE);
    tsn->out.buffer_filled += TS_SIZE;

    if (tsn->out.buffer_filled + TS_SIZE > tsn->out.buffer_size) {
        tsn->out.writer_result = tsn->out.writer(tsn->out.buffer, tsn->out.buffer_filled, tsn->out.writer_data);
        tsn->out.buffer_filled = 0;
    }

}
#endif

gboolean ts_snipper_write(TsSnipper *tsn, TsSnipperWriteFunc writer, gpointer userdata)
{
    if (!tsn || !writer || !tsn->file)
        return FALSE;

    if (tsn->state != TsSnipperStateReady)
        return FALSE;

    tsn->state = TsSnipperStateWriting;

    /* read input, handle with tsn_output, write last bytes in buffer. */
    tsn->out.writer = writer;
    tsn->out.writer_data = userdata;
    tsn->out.writer_result = TRUE;
    tsn->out.buffer_size = TSN_READ_BUFFER_SIZE;
    tsn->out.buffer_filled = 0;
    tsn->out.buffer = g_malloc(tsn->out.buffer_size);
    tsn->out.have_pat = 0;
    tsn->out.have_pmt = 0;
    tsn->out.in_slice = 0;
    tsn->out.pts_delta = 0;

    guint32 tmp_start_slice = ts_snipper_add_slice(tsn, -1, 0);
    guint32 tmp_end_slice = ts_snipper_add_slice(tsn, tsn->iframe_count, -1);

    tsn->out.active_slice = tsn->out.slices;

    tso_pid_actions_init(&tsn->out, tsn->pmgr);

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
#if 0
    push_term_packet(tsn);
#endif

    /* Write rest of buffer. */
    if (tsn->out.writer_result && tsn->out.buffer_filled > 0) {
        tsn->out.writer_result = writer(tsn->out.buffer, tsn->out.buffer_filled, userdata);
    }

    tso_pid_actions_cleanup(&tsn->out, tsn->pmgr);
    ts_analyzer_free(ts_analyzer);
    tsn->out.buffer_size = 0;
    g_free(tsn->out.buffer);
    tsn->out.buffer = NULL;

    ts_snipper_delete_slice(tsn, tmp_start_slice);
    ts_snipper_delete_slice(tsn, tmp_end_slice);

    tsn->state = TsSnipperStateReady;

    fflush(stderr);

    return tsn->out.writer_result;
}

TsSnipperState ts_snipper_get_state(TsSnipper *tsn)
{
    return tsn != NULL ? tsn->state : TsSnipperStateUnknown;
}
