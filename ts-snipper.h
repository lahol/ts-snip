#pragma once

#include <glib.h>
#include "pes-frame-info.h"

typedef struct _TsSnipper TsSnipper;

/* Create a new stream info for the given file, read and analyze. */
TsSnipper *ts_snipper_new(const gchar *filename);

const gchar *ts_snipper_get_filename(TsSnipper *tsn);
/* After analyze */
const gchar *ts_snipper_get_sha1(TsSnipper *tsn);

void ts_snipper_destroy(TsSnipper *tsn);

guint32 ts_snipper_get_iframe_count(TsSnipper *tsn);

bool ts_snipper_get_iframe_info(TsSnipper *tsn, PESFrameInfo *frame_info, guint32 frame_id);

void ts_snipper_get_iframe(TsSnipper *tsn, guint8 **data, gsize *length, guint32 frame_id);

#define TS_SLICE_ID_INVALID ((guint32)(-1))
/** A slice in the stream, i.e., a section that is to be cut out. */
typedef struct {
    guint32 id; /**< Identifier of the slices. */
    gsize begin; /**< The offset of the start of the slice. */
    gsize end; /**< The offset of the data following the slice. */

    guint32 begin_frame;
    guint32 end_frame;

    guint64 pts_begin;
    guint64 pts_end;

    guint64 pcr_begin;
    guint64 pcr_end;
} TsSlice;

#define TS_SLICE(ptr) ((TsSlice *)(ptr))

/** Only cut on I frames.
 *  @param[in] frame_begin The id of the begin or -1 to cut from start.
 *  @param[in] frame_end The id of the end or -1 to cut until the end.
 */
guint32 ts_snipper_add_slice(TsSnipper *tsn, guint32 frame_begin, guint32 frame_end);

/** Find a slice containing the given frame id.
 */
guint32 ts_snipper_find_slice_for_frame(TsSnipper *tsn, TsSlice *slice, guint32 frame_id, gboolean include_end);

void ts_snipper_delete_slice(TsSnipper *tsn, guint64 id);

typedef gboolean (*TsSnipperEnumSlicesFunc)(TsSlice *, gpointer);
void ts_snipper_enum_slices(TsSnipper *tsn, TsSnipperEnumSlicesFunc callback, gpointer userdata);

/* enum slices, merge slices (interal), remove slice */

/** Callback to write currently buffered data until false is returned. */
typedef gboolean (*TsSnipperWriteFunc)(guint8 *, gsize, gpointer);

/** Write filtered output to buffer and call writer.*/
gboolean ts_snipper_write(TsSnipper *tsn, TsSnipperWriteFunc writer, gpointer userdata);

gboolean ts_snipper_get_analyze_status(TsSnipper *tsn, gsize *bytes_read, gsize *bytes_total);
gboolean ts_snipper_get_write_status(TsSnipper *tsn, gsize *bytes_read, gsize *bytes_total);

void ts_snipper_analyze(TsSnipper *tsn);

typedef enum {
    TsSnipperStateUnknown = 0,
    TsSnipperStateInitialized = 1,
    TsSnipperStateAnalyzing = 2,
    TsSnipperStateWriting = 3,
    TsSnipperStateReady = 4
} TsSnipperState;

TsSnipperState ts_snipper_get_state(TsSnipper *tsn);
