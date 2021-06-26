#include <glib.h>
#include <stdio.h>
#include <libavcodec/avcodec.h>
#include "ts-snipper.h"

void _dump_pes(const char *fname, guint8 *data, gsize length)
{
    FILE *out = fopen(fname, "wb");
    if (out == NULL)
        return;
    fwrite(data, 1, length, out);
    fclose(out);
}

void write_frame(AVFrame *frame, const char *filename)
{
    FILE *out;
    if ((out = fopen(filename, "w")) == NULL)
        return;

    int x,y;
    fprintf(out, "P2 %d %d 255\n", frame->width, frame->height);
    for (y = 0; y < frame->height; ++y) {
        for (x = 0; x < frame->width; ++x) {
            fprintf(out, "%3u ", frame->data[0][y*frame->linesize[0] + x]);
        }
        fprintf(out, "\n");
    }

    fclose(out);
}

gboolean decode_image(const char *fname, PESFrameInfo *frame_info, guint8 *buffer, gsize length)
{
    if (!fname || !frame_info || !buffer)
        return FALSE;

    AVCodec *codec = NULL;
    switch (frame_info->pidtype) {
        case PID_TYPE_VIDEO_13818:
            codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
            break;
        case PID_TYPE_VIDEO_14496:
            codec = avcodec_find_decoder(AV_CODEC_ID_H264);
            break;
        default:
            break;
    }
    if (!codec)
        return FALSE;
    AVCodecContext *context = avcodec_alloc_context3(codec);
    AVDictionary *opts = NULL;
    if (avcodec_open2(context, codec, &opts) < 0) {
        return FALSE;
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    int rc = -1;

    packet->data = buffer;
    packet->size = length;
    if ((rc = avcodec_send_packet(context, packet)) < 0)
        goto done;

    /* Flush */
    packet->data = NULL;
    packet->size = 0;
    if ((rc = avcodec_send_packet(context, packet)) < 0)
        goto done;

    if ((rc = avcodec_receive_frame(context, frame)) < 0)
        goto done;

    write_frame(frame, fname);

done:
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);

    return (rc == 0);

}

static gboolean write_stream_cb(guint8 *buffer, gsize bufsiz, FILE *f)
{
    fprintf(stderr, "Write %zu bytes\n", bufsiz);
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

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "You must specify a file name.\n");
        exit(1);
    }

    TsSnipper *tsn = ts_snipper_new(argv[1]);
    if (!tsn) {
        fprintf(stderr, "Error opening file.\n");
        exit(1);
    }

    guint32 iframe_count = ts_snipper_get_iframe_count(tsn);
    fprintf(stderr, "Found %u I frames\n", iframe_count);

#if 0
    if (argc >= 3) {
        guint32 id = strtoul(argv[2], NULL, 0);
        fprintf(stderr, "Get I frame no. %u\n", id);
        guint8 *data = NULL;
        gsize length = 0;
        PESFrameInfo frame_info;
        if (ts_snipper_get_iframe_info(tsn, &frame_info, id))
            fprintf(stderr, " codec: %u\n", frame_info.pidtype);
        ts_snipper_get_iframe(tsn, &data, &length, id);
        fprintf(stderr, " %p, len %zu\n", data, length);
        if (argc >= 4)
            decode_image(argv[3], &frame_info, data, length);
        g_free(data);
    }
#else
    /* Copy file */
    if (argc >= 3) {
        FILE *out = fopen(argv[2], "wb");
        if (out == NULL)
            goto done;

        ts_snipper_write(tsn, (TsSnipperWriteFunc)write_stream_cb, out);
        fclose(out);
    }
#endif
done:

    ts_snipper_destroy(tsn);

    return 0;
}

