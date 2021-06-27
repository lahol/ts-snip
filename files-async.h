#pragma once

#include <glib.h>
#include <gio/gio.h>

#include "ts-snipper.h"

void file_write_async(TsSnipper *snipper,
                      const char *filename,
                      GCancellable *cancellable,
                      GAsyncReadyCallback callback,
                      gpointer userdata);

gboolean file_write_finish(GAsyncResult *result, GError **error);

