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

/** A slice in the stream, i.e., a section that is to be cut out. */
typedef struct {
    gsize begin; /** The offset of the start of the slice. */
    gsize end; /** The offset of the data following the slice. */
} TsSlice;

void ts_snipper_add_slice(TsSnipper *tsn, gsize begin, gsize end);

/* enum slices, merge slices (interal), remove slice */

/** Callback to write currently buffered data until false is returned. */
typedef gboolean (*TsSnipperWriteFunc)(guint8 *, gsize, gpointer);

/** Write filtered output to buffer and call writer.*/
gboolean ts_snipper_write(TsSnipper *tsn, TsSnipperWriteFunc writer, gpointer userdata);
