#include "files-async.h"

#include <stdio.h>

typedef struct {
    TsSnipper *snipper;
    char *filename;
} WriterFunctionData;

static gboolean files_async_write_stream_cb(guint8 *buffer, gsize bufsiz, FILE *f)
{
    gsize bytes_written;
    gsize retry_count = 0;
    while (bufsiz > 0) {
        bytes_written = fwrite(buffer, 1, bufsiz, f);
        if (bytes_written > 0) {
            buffer += bytes_written;
            bufsiz -= bytes_written;
            retry_count = 0;
        }
        else if (retry_count++ > 5) {
                return FALSE;
        }
    }
    return TRUE;
}

static void files_async_write_data_free(WriterFunctionData *data)
{
    g_free(data->filename);
    g_free(data);
}

static void files_async_write_thread_cb(GTask *task,
                                        gpointer source_object,
                                        gpointer task_data,
                                        GCancellable *cancellable)
{
    WriterFunctionData *data = task_data;
    gboolean retval = FALSE;

    if (g_task_return_error_if_cancelled(task))
        return;

    FILE *out;
    if ((out = fopen(data->filename, "wb")) != NULL) {
        retval = ts_snipper_write(data->snipper,
                                  (TsSnipperWriteFunc)files_async_write_stream_cb,
                                  out);
        fclose(out);
    }

    g_task_return_boolean(task, retval);
}

void file_write_async(TsSnipper *snipper,
                      char *filename,
                      GCancellable *cancellable,
                      GAsyncReadyCallback callback,
                      gpointer userdata)
{
    GTask *task = NULL;
    WriterFunctionData *data = NULL;

    g_return_if_fail(snipper != NULL);
    g_return_if_fail(filename != NULL && filename[0] != 0);
    g_return_if_fail(cancellable == NULL || G_IS_CANCELLABLE(cancellable));

    task = g_task_new(NULL, cancellable, callback, userdata);

    g_task_set_return_on_cancel(task, FALSE);

    data = g_new0(WriterFunctionData, 1);
    data->snipper = snipper;
    data->filename = g_strdup(filename);

    g_task_set_task_data(task, data, (GDestroyNotify)files_async_write_data_free);

    g_task_run_in_thread(task, files_async_write_thread_cb);

    g_object_unref(task);
}

gboolean file_write_finish(GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(G_IS_TASK(result), FALSE);
    g_return_val_if_fail(g_task_is_valid(result, NULL), FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}


