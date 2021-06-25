#pragma once

#include <glib.h>

typedef struct {
    guint32 frame_number;
    guint64 pts;
    guint64 dts;
    gsize stream_offset;
} PESFrameInfo;

typedef struct _TsStreamInfo TsStreamInfo;

/* Create a new stream info for the given file, read and analyze. */
TsStreamInfo *ts_stream_info_new(const gchar *filename);

void ts_stream_info_destroy(TsStreamInfo *tsi);

guint32 ts_stream_info_get_iframe_count(TsStreamInfo *tsi);

void ts_stream_info_get_iframe(TsStreamInfo *tsi, guint8 **data, gsize *length, guint32 frame_id);
