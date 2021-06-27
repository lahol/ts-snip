#pragma once

#include <glib.h>
#include <pidinfo.h>

#define PES_FRAME_ID_INVALID ((guint32)(-1))
#define PES_FRAME_TS_INVALID ((guint64)(-1))

typedef struct {
    guint32 frame_number;
    guint64 pts;
    guint64 dts;
    guint64 pcr;
    gsize stream_offset_start;
    gsize stream_offset_end;
    PidType pidtype;
} PESFrameInfo;


