#include <glib.h>
#include <stdio.h>
#include "ts-stream-info.h"

void _dump_pes(const char *fname, guint8 *data, gsize length)
{
    FILE *out = fopen(fname, "wb");
    if (out == NULL)
        return;
    fwrite(data, 1, length, out);
    fclose(out);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "You must specify a file name.\n");
        exit(1);
    }

    TsStreamInfo *tsi = ts_stream_info_new(argv[1]);
    if (!tsi) {
        fprintf(stderr, "Error opening file.\n");
        exit(1);
    }

    guint32 iframe_count = ts_stream_info_get_iframe_count(tsi);
    fprintf(stderr, "Found %u I frames\n", iframe_count);

    if (argc >= 3) {
        guint32 id = strtoul(argv[2], NULL, 0);
        fprintf(stderr, "Get I frame no. %u\n", id);
        guint8 *data = NULL;
        gsize length = 0;
        ts_stream_info_get_iframe(tsi, &data, &length, id);
        fprintf(stderr, " %p, len %zu\n", data, length);
        if (argc >= 4)
            _dump_pes(argv[3], data, length);
        g_free(data);
    }

    ts_stream_info_destroy(tsi);

    return 0;
}

