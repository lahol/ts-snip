#pragma once

#include <glib.h>
#include "pes-frame-info.h"

typedef struct _TsSnipper TsSnipper;

/* Create a new stream info for the given file, read and analyze. */
TsSnipper *ts_snipper_new(const gchar *filename);

void ts_snipper_destroy(TsSnipper *tsn);

guint32 ts_snipper_get_iframe_count(TsSnipper *tsn);

bool ts_snipper_get_iframe_info(TsSnipper *tsn, PESFrameInfo *frame_info, guint32 frame_id);

void ts_snipper_get_iframe(TsSnipper *tsn, guint8 **data, gsize *length, guint32 frame_id);
