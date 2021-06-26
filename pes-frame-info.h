#pragma once

#include <glib.h>
#include <pidinfo.h>

typedef struct {
    guint32 frame_number;
    guint64 pts;
    guint64 dts;
    gsize stream_offset_start;
    gsize stream_offset_end;
    PidType pidtype;
} PESFrameInfo;


