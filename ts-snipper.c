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
    WriterPidAction action;
    guint8 continuity;
    gint64 pts_last; /* last pts of this pid */
} WriterPidInfo;

typedef struct {
    GList *slices; /**< [TsSlice *] */
    GList *active_slice; /**< Pointer to next/current slice in slices. */
    GArray *disabled_pids; /* [guint16] */
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
    guint32 pcr_present : 1;

    WriterPidInfo *pid_writer_infos; /* When writing starts, set the actions for all pids. */
    gsize pids_seen;
    gsize pid_count;

    gint64 pcr_delta;
    gint64 pcr_delta_accumulator; /* During an active slice, accumulate the deltas. Necessary
                                     for samples with a timestamp before the slice, which occur
                                     later. */
    gint64 pts_delta_tolerance; /* Tolerance for non-video streams, required because of drift
                                   between pcr and pts. */
    gint64 pcr_stream_first; /**< First pcr/pts pair */
    gint64 pts_stream_first;

    gint64 pcr_last;
    /* Next pts of the frame after the active slice */
    gint64 pts_cut;
} TsSnipperOutput;

struct _TsSnipper {
    PidInfoManager *pmgr;
    uint32_t analyzer_client_id;
    uint32_t random_access_client_id;
    TsSnipperState state;

    gint ref_count;

    gchar *filename;
    FILE *file;
    gsize file_size;
    gsize bytes_read;
    GChecksum *checksum;

    GArray *frame_infos;
    guint32 iframe_count;
    guint16 video_pid;

    /* first B frame after an I or P frame without another one yet */
    gsize dangling_bframe_start;
    gboolean dangling_bframe_present;

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
    pes->data = g_byte_array_new();
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
                    .stream_offset_dangling_bframe =
                        tsn->dangling_bframe_present ? tsn->dangling_bframe_start : pes->packet_start,
                    .pts = pes->pts,
                    .dts = pes->dts,
                    .pcr = pes->pcr,
                    .pidtype = PID_TYPE_VIDEO_13818
                };
                g_mutex_lock(&tsn->data_lock);
                g_array_append_val(tsn->frame_infos, frame_info);
                g_mutex_unlock(&tsn->data_lock);
                tsn->dangling_bframe_present = FALSE;
#if DEBUG
                fprintf(stderr, "I frame %" G_GINT64_FORMAT " (%u) delta to pcr %" G_GINT64_FORMAT "\n",
                        pes->pts, frame_info.frame_number,
                        pes->pts - (pes->pcr / 300));
#endif
                break;
            }
            else if (pictype == 2) {
                /* P frames reset the dangling B frame also. */
                tsn->dangling_bframe_present = FALSE;
#if DEBUG
                fprintf(stderr, "P frame %" G_GINT64_FORMAT "\n", pes->pts);
#endif
            }
            else if (pictype == 3) {
                /* Only remember the first dangling B frame. */
#if DEBUG
                fprintf(stderr, "B frame %" G_GINT64_FORMAT "\n", pes->pts);
#endif
                if (!tsn->dangling_bframe_present) {
                    tsn->dangling_bframe_start = pes->packet_start;
                    tsn->dangling_bframe_present = TRUE;
                }
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
            if ((data[3] & 0x1f) == 5) {
                /* IDR image start */
                PESFrameInfo frame_info = {
                    .frame_number = tsn->iframe_count++,
                    .stream_offset_start = pes->packet_start,
                    .stream_offset_end = pes->packet_end,
                    .stream_offset_dangling_bframe =
                        tsn->dangling_bframe_present ? tsn->dangling_bframe_start : pes->packet_start,
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
            else {
                /* FIXME: Is there a similar concept to P frames in 14496-10? */
                if (!tsn->dangling_bframe_present)
                    tsn->dangling_bframe_start = pes->packet_start;
            }
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

static void tso_check_first_pcr_pts(TsSnipperOutput *tso,
                                    gint64 pcr,
                                    gint64 pts)
{
    /* If both are set, do noting. */
    if (tso->pts_stream_first != PES_FRAME_TS_INVALID
            && tso->pcr_stream_first != PES_FRAME_TS_INVALID)
        return;
    /* Update the timestamps until both are set, in order to get a pair close to each other. */
    if (pcr != PES_FRAME_TS_INVALID)
        tso->pcr_stream_first = pcr;
    if (pts != PES_FRAME_TS_INVALID)
        tso->pts_stream_first = pts;
#if DEBUG
    fprintf(stderr, "First pcr/pts: %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT "\n",
            tso->pcr_stream_first, tso->pts_stream_first);
#endif
}

typedef void (*PESFinishedFunc)(PESData *, gpointer);

static void _tsn_handle_pes(TsSnipper *tsn,
                            PidInfo *pidinfo,
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
            tso_check_first_pcr_pts(&tsn->out, pcr, PES_FRAME_TS_INVALID);
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
            tso_check_first_pcr_pts(&tsn->out, pcr, pes->pts);
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
    g_checksum_update(tsn->checksum, packet, TS_SIZE);
    if (!pidinfo)
        return true;

    if (pidinfo->type == PID_TYPE_VIDEO_13818) {
        if (!tsn->video_pid)
            tsn->video_pid = pidinfo->pid;
        _tsn_handle_pes(tsn, pidinfo, tsn->analyzer_client_id, packet, offset,
                (PESFinishedFunc)pes_data_analyze_video_13818, tsn);
    }
    else if (pidinfo->type == PID_TYPE_VIDEO_14496) {
        if (!tsn->video_pid)
            tsn->video_pid = pidinfo->pid;
        _tsn_handle_pes(tsn, pidinfo, tsn->analyzer_client_id, packet, offset,
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

    tsn->out.pts_stream_first = PES_FRAME_TS_INVALID;
    tsn->out.pcr_stream_first = PES_FRAME_TS_INVALID;

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
    tsn->filename = g_canonicalize_filename(filename, NULL);
    tsn->checksum = g_checksum_new(G_CHECKSUM_SHA1);
    if (!tsn_open_file(tsn, tsn->filename))
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

    ts_snipper_ref(tsn);

    return tsn;

err:
    ts_snipper_destroy(tsn);
    return NULL;
}

void ts_snipper_destroy(TsSnipper *tsn)
{
    if (tsn) {
        tsn_close_file(tsn);
        g_free(tsn->filename);
        g_checksum_free(tsn->checksum);

        g_list_free_full(tsn->out.slices, g_free);
        if (tsn->out.disabled_pids)
            g_array_free(tsn->out.disabled_pids, TRUE);

        g_free(tsn);
    }
}

void ts_snipper_ref(TsSnipper *snipper)
{
    if (G_LIKELY(snipper != NULL))
        g_atomic_int_inc(&snipper->ref_count);
}

void ts_snipper_unref(TsSnipper *snipper)
{
    if (G_LIKELY(snipper != NULL)) {
        if (!g_atomic_int_dec_and_test(&snipper->ref_count))
            ts_snipper_destroy(snipper);
    }
}

const gchar *ts_snipper_get_filename(TsSnipper *tsn)
{
    return tsn ? tsn->filename : NULL;
}

const gchar *ts_snipper_get_sha1sum(TsSnipper *tsn)
{
    if (!tsn || tsn->state != TsSnipperStateReady)
        return NULL;
    return g_checksum_get_string(tsn->checksum);
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
#if GLIB_CHECK_VERSION(2,64,0)
        fifi->pes_data = g_byte_array_steal(pes->data, &fifi->pes_size);
#else
        fifi->pes_size = pes->data->len;
        fifi->pes_data = g_byte_array_free(pes->data, FALSE);
        pes->data = g_byte_array_new();
#endif
    }
}

static bool _ts_get_iframe_handle_packet(PidInfo *pidinfo, const uint8_t *packet, const size_t offset, struct FindIFrameInfo *fifi)
{
    if (!pidinfo || !fifi)
        return false;

    if (pidinfo->type == PID_TYPE_VIDEO_13818 || pidinfo->type == PID_TYPE_VIDEO_14496) {
        _tsn_handle_pes(fifi->tsn, pidinfo, fifi->tsn->random_access_client_id, packet, offset,
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
    if (!tsn)
        return TS_SLICE_ID_INVALID;
    /* FIXME Handle overlapping slices. */
    PESFrameInfo fi_begin;
    PESFrameInfo fi_end;
    if (frame_begin == PES_FRAME_ID_INVALID) {
        fi_begin.stream_offset_dangling_bframe = 0;
        fi_begin.stream_offset_start = 0;
        fi_begin.pts = PES_FRAME_TS_INVALID;
        fi_begin.pcr = PES_FRAME_TS_INVALID;
    }
    else if (!ts_snipper_get_iframe_info(tsn, &fi_begin, frame_begin) && frame_begin != tsn->iframe_count) {
        return TS_SLICE_ID_INVALID;
    }
    else if (frame_begin == tsn->iframe_count) {
        if (!ts_snipper_get_iframe_info(tsn, &fi_begin, frame_begin - 1))
            return TS_SLICE_ID_INVALID;
        fi_begin.stream_offset_start = fi_begin.stream_offset_end;
        fi_begin.pts = PES_FRAME_TS_INVALID;
        fi_begin.pcr = PES_FRAME_TS_INVALID;
    }
    if (frame_end == PES_FRAME_ID_INVALID || frame_end + 1 == tsn->iframe_count) {
        fi_end.stream_offset_start = tsn->file_size;
        fi_end.pts = PES_FRAME_TS_INVALID;
        fi_end.pcr = PES_FRAME_TS_INVALID;
    }
    else if (!ts_snipper_get_iframe_info(tsn, &fi_end, frame_end)) {
        return TS_SLICE_ID_INVALID;
    }

    TsSlice *slice = g_new(TsSlice, 1);
    /* Also ignore dangling, B frames, which relate to this I frame, i.e., B frames immediately before
     * the I frame */
    slice->begin = fi_begin.stream_offset_dangling_bframe/*fi_begin.stream_offset_start*/;
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

static void tso_pid_writer_infos_init(TsSnipperOutput *tso, PidInfoManager *pmgr)
{
    tso->pid_count = pid_info_manager_get_pid_count(pmgr);
    tso->pid_writer_infos = g_new0(WriterPidInfo, tso->pid_count);
    tso->pids_seen = 0;
    gsize j;
    for (j = 0; j < tso->pid_count; ++j)
        tso->pid_writer_infos[j].pts_last = PES_FRAME_TS_INVALID;
}

static void tso_pid_writer_infos_cleanup(TsSnipperOutput *tso, PidInfoManager *pmgr)
{
    pid_info_manager_clear_private_data(pmgr, tso->writer_client_id);
    g_free(tso->pid_writer_infos);
    tso->pid_writer_infos = NULL;
    tso->pid_count = 0;
}

static void tso_pid_writer_infos_reset(TsSnipperOutput *tso, WriterPidAction action)
{
    gsize j;
    for (j = 0; j < tso->pid_count; ++j) {
        tso->pid_writer_infos[j].action = action;
    }
}

static WriterPidInfo *tso_pid_writer_infos_get_for_pid(TsSnipperOutput *tso, PidInfo *pidinfo)
{
    /* We could handle this without private data, but this way we gain faster access (no searching). */
    WriterPidInfo *info = pid_info_get_private_data(pidinfo, tso->writer_client_id);
    if (info == NULL) {
        /* pids_seen cannot be larger than pid_count */
        /* First occurrence of this pid */
        info = &tso->pid_writer_infos[tso->pids_seen++];
        pid_info_set_private_data(pidinfo, tso->writer_client_id, info, NULL);
    }

    return info;
}

static gboolean tso_packet_is_pes(PidInfo *pidinfo)
{
    return (pidinfo &&
            (pidinfo->type == PID_TYPE_VIDEO_14496 ||
             pidinfo->type == PID_TYPE_VIDEO_13818 ||
             pidinfo->type == PID_TYPE_VIDEO_11172 ||
             pidinfo->type == PID_TYPE_AUDIO_13818 ||
             pidinfo->type == PID_TYPE_AUDIO_11172 ||
             pidinfo->type == PID_TYPE_TELETEXT));
}

static gboolean tso_packet_is_video(PidInfo *pidinfo)
{
    return (pidinfo &&
            (pidinfo->type == PID_TYPE_VIDEO_14496 ||
             pidinfo->type == PID_TYPE_VIDEO_13818 ||
             pidinfo->type == PID_TYPE_VIDEO_11172));
}

static inline gint64 tso_get_pes_pts(PidInfo *pidinfo, const uint8_t *packet)
{
    if (!tso_packet_is_pes(pidinfo) || !ts_get_unitstart(packet))
        return PES_FRAME_TS_INVALID;
    size_t pes_offset = ts_has_adaptation(packet) ? 5 + packet[4] : 4;
    const uint8_t *pes = packet + pes_offset;
    if (!pes_has_pts(pes))
        return PES_FRAME_TS_INVALID;
    return pes_get_pts(pes);
}

#if 0
static void tso_update_timestamps_delta(TsSnipperOutput *tso,
                                        const uint8_t *packet)
{
    gint64 tstmp;
    if (ts_has_adaptation(packet) && tsaf_has_pcr(packet)) {
        tstmp = tsaf_get_pcr(packet) * 300 + tsaf_get_pcrext(packet);
        /* Only accumulate deltas of packets that are not written. */
        if (tso->pcr_present && tso->in_slice) {
            tso->pcr_delta_accumulator += tstmp - tso->pcr_last;
        }
        tso->pcr_last = tstmp;
        tso->pcr_present = 1;
    }
}
#endif

static void tso_update_pes_pts(PidInfo *pidinfo, const uint8_t *packet, TsSnipperOutput *tso)
{
    gint64 pts = tso_get_pes_pts(pidinfo, packet);
    if (pts == PES_FRAME_TS_INVALID)
        return;
    WriterPidInfo *info = pid_info_get_private_data(pidinfo, tso->writer_client_id);
    if (!info)
        return;
    info->pts_last = pts;
}

/* Rewrite on basis of pcr differences. */
static void tso_rewrite_timestamps(TsSnipperOutput *tso, PidInfo *pidinfo, uint8_t *packet)
{
    gint64 tstmp;
    size_t pes_offset = 4;
    if (ts_has_adaptation(packet)) {
        pes_offset += 1 + packet[4];
        if (tsaf_has_pcr(packet) && tso->pcr_present) {
            tstmp = tsaf_get_pcr(packet) * 300 + tsaf_get_pcrext(packet) - tso->pcr_delta;
            tsaf_set_pcr(packet, tstmp / 300);
            tsaf_set_pcrext(packet, tstmp % 300);
        }
    }

    if (!ts_get_unitstart(packet) || !tso_packet_is_pes(pidinfo)) {
        return;
    }
    uint8_t *pes = packet + pes_offset;
    /* pcr has a frequency of 300 higher than pts/dts */
    gint64 delta = tso->pcr_delta / 300;
    if (pes_has_pts(pes)) {
#if DEBUG 
        fprintf(stderr, "[%3u] rewrite %" G_GINT64_FORMAT " -> %" G_GINT64_FORMAT "\n",
                ts_get_pid(packet), pes_get_pts(pes), pes_get_pts(pes) - delta);
#endif
        pes_set_pts(pes, pes_get_pts(pes) - delta);
    }
    if (pes_has_dts(pes)) {
        pes_set_dts(pes, pes_get_dts(pes) - delta);
    }
}

static void tso_rewrite_continuity(TsSnipperOutput *tso, PidInfo *pidinfo, uint8_t *packet)
{
    WriterPidInfo *info = pid_info_get_private_data(pidinfo, tso->writer_client_id);
    if (!info)
        return;
    /* Only increment when payload present. */
    /* FIXME original duplicates should stay this way. */
    if (ts_has_payload(packet)) {
        info->continuity = (info->continuity + 1) & 0x0f;
    }
    packet[3] = (packet[3] & 0xf0) | (info->continuity);
}

static gboolean tso_check_pes_timestamp(PidInfo *pidinfo, const uint8_t *packet, TsSnipperOutput *tso)
{
    /* Not enough data to check timestamp. So we are fine. */
    if (tso->pts_cut == PES_FRAME_TS_INVALID /*|| !tso_packet_is_video(pidinfo)*/)
        return TRUE;
    gint64 pts = tso_get_pes_pts(pidinfo, packet);
    if (pts == PES_FRAME_TS_INVALID)
        return TRUE;
    /* Adapt to tolerance between pcr and pts in video */
    gint64 pts_pcr_tolerance = tso_packet_is_video(pidinfo) ? 0 : tso->pts_delta_tolerance;

    return (pts + pts_pcr_tolerance >= tso->pts_cut);
}

static gboolean tso_check_pes_timestamp_in_active_slice(PidInfo *pidinfo, const uint8_t *packet, TsSnipperOutput *tso)
{
    /* If we are not in a slice, or this is video, simply ignore it. */
    if (!tso->in_slice || tso->active_slice == NULL
            || TS_SLICE(tso->active_slice->data)->pts_begin == PES_FRAME_TS_INVALID
            || TS_SLICE(tso->active_slice->data)->pts_end == PES_FRAME_TS_INVALID
            || tso_packet_is_video(pidinfo))
        return TRUE;
    gint64 pts = tso_get_pes_pts(pidinfo, packet);
    if (pts == PES_FRAME_TS_INVALID)
        return TRUE;

   return (pts >= TS_SLICE(tso->active_slice->data)->pts_begin
            && pts < TS_SLICE(tso->active_slice->data)->pts_end);
}

static gboolean tso_is_pid_disabled(TsSnipperOutput *tso, guint16 pid)
{
    if (tso->disabled_pids == NULL)
        return FALSE;
    guint i;
    for (i = 0; i < tso->disabled_pids->len; ++i) {
        if (g_array_index(tso->disabled_pids, guint16, i) == pid)
            return TRUE;
    }
    return FALSE;
}

static gboolean tso_should_write_packet(PidInfo *pidinfo, const uint8_t *packet, TsSnipperOutput *tso)
{
    if (tso_is_pid_disabled(tso, ts_get_pid(packet))) {
        return FALSE;
    }
    if (!pidinfo) {
        return TRUE;
    }
    WriterPidInfo *info = tso_pid_writer_infos_get_for_pid(tso, pidinfo);
#if DEBUG
    gint64 debug_pts;
#endif

    if (!tso->in_slice) {
        if (info->action == WPAIgnoreUntilUnitStart && ts_get_unitstart(packet)) {
            info->action = WPAWrite;
        }
        /* Check whether the timestamp is after the last cut, if not, wait until the next unit. */
        if (!tso_check_pes_timestamp(pidinfo, packet, tso)) {
            info->action = WPAIgnoreUntilUnitStart;
        }
#if DEBUG
        if ((debug_pts = tso_get_pes_pts(pidinfo, packet)) != PES_FRAME_TS_INVALID)
            fprintf(stderr, "[%3u] %c packet out of slice: %" G_GINT64_FORMAT "\n",
                    ts_get_pid(packet), info->action == WPAWrite ? 'W' : 'I', debug_pts);
#endif
         /* Already set to write or NULL packet. */
        return (info->action == WPAWrite || pidinfo->pid == 0x1FFF);
    }
    else {
        if (info->action == WPAWriteUntilUnitStart && ts_get_unitstart(packet)) {
            info->action = WPAIgnore;
        }
        if (!tso_check_pes_timestamp_in_active_slice(pidinfo, packet, tso)) {
            info->action = WPAWriteUntilUnitStart;
        }
#if DEBUG
        if ((debug_pts = tso_get_pes_pts(pidinfo, packet)) != PES_FRAME_TS_INVALID)
            fprintf(stderr, "[%3u] %c packet in slice    : %" G_GINT64_FORMAT "\n",
                    ts_get_pid(packet), info->action == WPAIgnore ? 'I' : 'W', debug_pts);
#endif
        return !(info->action == WPAIgnore || pidinfo->pid == 0x1FFF);
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
        tso_pid_writer_infos_reset(tso, WPAIgnoreUntilUnitStart);
        tso->in_slice = 0;
        tso->pcr_delta += tso->pcr_delta_accumulator;
#if DEBUG
        fprintf(stderr, "updated pcr delta: %" G_GINT64_FORMAT " accum %" G_GINT64_FORMAT "\n",
                tso->pcr_delta, tso->pcr_delta_accumulator);
#endif
    }
    else if (!tso->in_slice && in_slice) {
        /* We changed from not in a slice to a slice. */
        tso_pid_writer_infos_reset(tso, WPAWriteUntilUnitStart);
        tso->in_slice = 1;
        tso->pts_cut = ((TsSlice *)tso->active_slice->data)->pts_end;
        /* FIXME: Use the accumulator, but take the correct pcr, i.e., not already the last
         * before each slice. */
        tso->pcr_delta_accumulator = TS_SLICE(tso->active_slice->data)->pcr_begin != PES_FRAME_TS_INVALID
            ? TS_SLICE(tso->active_slice->data)->pcr_end - TS_SLICE(tso->active_slice->data)->pcr_begin
            : TS_SLICE(tso->active_slice->data)->pcr_end - tso->pcr_stream_first;
        tso->pts_delta_tolerance = TS_SLICE(tso->active_slice->data)->pcr_begin != PES_FRAME_TS_INVALID
            ? TS_SLICE(tso->active_slice->data)->pts_end - TS_SLICE(tso->active_slice->data)->pts_begin
              - tso->pcr_delta_accumulator / 300
            : TS_SLICE(tso->active_slice->data)->pts_end - tso->pts_stream_first
              - tso->pcr_delta_accumulator / 300;

        fprintf(stderr, "[0x%08zx] pts_delta_tolerance: %" G_GINT64_FORMAT "\n",
                offset, tso->pts_delta_tolerance);
#if DEBUG
        fprintf(stderr, "active slice: %" G_GINT64_FORMAT " -> %" G_GINT64_FORMAT " delta pts: %"
                G_GINT64_FORMAT ", delta pcr/300 %" G_GINT64_FORMAT "\n",
                TS_SLICE(tso->active_slice->data)->pts_begin,
                TS_SLICE(tso->active_slice->data)->pts_end,
                TS_SLICE(tso->active_slice->data)->pts_end - TS_SLICE(tso->active_slice->data)->pts_begin,
                tso->pcr_delta_accumulator / 300);
#endif
    }
    gboolean write_packet = tso_should_write_packet(pidinfo, packet, tso);
    tso->bytes_read = offset;

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

#if 0
    tso_update_timestamps_delta(tso, packet);
#endif
    tso_update_pes_pts(pidinfo, packet, tso);

    if (!write_packet)
        return tso->writer_result;

    /* push packet to buffer */
    memcpy(tso->buffer + tso->buffer_filled, packet, TS_SIZE);
    tso_rewrite_timestamps(tso, pidinfo, tso->buffer + tso->buffer_filled);
    tso_rewrite_continuity(tso, pidinfo, tso->buffer + tso->buffer_filled);

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
    tsn->out.pcr_present = 0;
    tsn->out.pcr_delta = 0;

    guint32 tmp_start_slice = ts_snipper_add_slice(tsn, -1, 0);
    guint32 tmp_end_slice = ts_snipper_add_slice(tsn, tsn->iframe_count, -1);

    tsn->out.active_slice = tsn->out.slices;

    tso_pid_writer_infos_init(&tsn->out, tsn->pmgr);
    tso_pid_writer_infos_reset(&tsn->out, WPAIgnore);

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

    /* Write rest of buffer. */
    if (tsn->out.writer_result && tsn->out.buffer_filled > 0) {
        tsn->out.writer_result = writer(tsn->out.buffer, tsn->out.buffer_filled, userdata);
    }

    tso_pid_writer_infos_cleanup(&tsn->out, tsn->pmgr);
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

void ts_snipper_disable_pid(TsSnipper *snipper, guint16 pid)
{
    if (snipper == NULL)
        return;
    if (snipper->out.disabled_pids == NULL) {
        snipper->out.disabled_pids = g_array_new(FALSE, FALSE, sizeof(guint16));
    }

    /* Check that pid is not already set as disabled. */
    if (tso_is_pid_disabled(&snipper->out, pid))
        return;
    g_array_append_val(snipper->out.disabled_pids, pid);
}

void ts_snipper_enable_pid(TsSnipper *snipper, guint16 pid)
{
    if (snipper == NULL || snipper->out.disabled_pids == NULL)
        return;
    guint i;
    for (i = 0; i < snipper->out.disabled_pids->len; ++i) {
        if (g_array_index(snipper->out.disabled_pids, guint16, i) == pid) {
            g_array_remove_index_fast(snipper->out.disabled_pids, i);
            return;
        }
    }
}
