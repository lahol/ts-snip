#include <glib.h>
#include <stdio.h>
#include <libavcodec/avcodec.h>

#include <gdk/gdkx.h>
#include <gtk/gtk.h>

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

gboolean output_slice(TsSlice *slice, gpointer nil)
{
    fprintf(stderr, "Slice %" PRIu64 " %zu -> %zu; %" PRIu64 " -> %" PRIu64 "\n",
            slice->id,
            slice->begin, slice->end,
            slice->pts_begin, slice->pts_end);
    return TRUE;
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

struct TsSnipApp {
    TsSnipper *tsn;
    GtkWidget *main_window;
    GtkWidget *drawing_area;
    GtkWidget *slider;
    GtkAdjustment *adjust_stream_pos;
} app;

static void main_quit(GtkWidget *widget, gpointer data)
{
    gtk_main_quit();
}

static void main_drawing_area_size_allocate(GtkWidget *widget, GtkAllocation *alloc, gpointer nil)
{
}

static void main_drawing_area_realize(GtkWidget *widget, gpointer nil)
{
}

static void main_drawing_area_draw(GtkWidget *widget, cairo_t *cr, gpointer nil)
{
}

static void main_adjustment_value_changed(GtkAdjustment *adjustment, gpointer nil)
{
    fprintf(stderr, "value changed to %f\n", gtk_adjustment_get_value(adjustment));
}

void main_init_window(void)
{
    app.main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_signal_connect(G_OBJECT(app.main_window), "destroy",
            G_CALLBACK(main_quit), NULL);
/*    g_signal_connect(G_OBJECT(app.main_window), "configure-event",
            G_CALLBACK(main_window_configure_event), &app);*/

    app.drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(app.drawing_area, 120, 80);
    g_signal_connect(G_OBJECT(app.drawing_area), "size-allocate",
            G_CALLBACK(main_drawing_area_size_allocate), NULL);
    g_signal_connect(G_OBJECT(app.drawing_area), "realize",
            G_CALLBACK(main_drawing_area_realize), NULL);
    g_signal_connect(G_OBJECT(app.drawing_area), "draw",
            G_CALLBACK(main_drawing_area_draw), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_box_pack_start(GTK_BOX(vbox), app.drawing_area, TRUE, TRUE, 0);

    app.adjust_stream_pos = gtk_adjustment_new(0.0, 0.0, 0.0, 1.0, 100.0, 0.0);
    g_signal_connect(G_OBJECT(app.adjust_stream_pos), "value-changed",
            G_CALLBACK(main_adjustment_value_changed), NULL);

    app.slider = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, app.adjust_stream_pos);
    gtk_scale_set_has_origin(GTK_SCALE(app.slider), FALSE);
    gtk_scale_set_draw_value(GTK_SCALE(app.slider), FALSE);
    gtk_range_set_round_digits(GTK_RANGE(app.slider), 0);
    gtk_box_pack_end(GTK_BOX(vbox), app.slider, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(app.main_window), vbox);

    gtk_widget_show_all(app.main_window);
}

int main(int argc, char **argv)
{
    if (!XInitThreads()) {
        fprintf(stderr, "XInitThreads() failed.\n");
        exit(1);
    }

    gtk_init(&argc, &argv);

    if (argc < 2) {
        fprintf(stderr, "You must specify a file name.\n");
        exit(1);
    }

    memset(&app, 0, sizeof(struct TsSnipApp));

    app.tsn = ts_snipper_new(argv[1]);
    if (!app.tsn) {
        fprintf(stderr, "Error opening file.\n");
        exit(1);
    }

    guint32 iframe_count = ts_snipper_get_iframe_count(app.tsn);
    fprintf(stderr, "Found %u I frames\n", iframe_count);

    main_init_window();
    gtk_adjustment_configure(GTK_ADJUSTMENT(app.adjust_stream_pos),
                             0.0, /* value */
                             0.0, /* lower */
                             (gdouble)iframe_count, /* upper */
                             1.0, /* step increment */
                             100.0, /* page_increment */
                             0.0); /* page_size */

    gtk_window_present(GTK_WINDOW(app.main_window));

    gtk_scale_add_mark(GTK_SCALE(app.slider), 50, GTK_POS_BOTTOM, /*"&#x21e4;"*/"[");
    gtk_scale_add_mark(GTK_SCALE(app.slider), 100, GTK_POS_BOTTOM, /*"&#x21e5;"*/"]");

    gtk_main();

    fprintf(stderr, "Quit\n");

#if 0
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

        /* Cleanup start */
        ts_snipper_add_slice(tsn, -1, 0);
        ts_snipper_add_slice(tsn, 50, 1400);

        ts_snipper_enum_slices(tsn, (TsSnipperEnumSlicesFunc)output_slice, NULL);

        FILE *out = fopen(argv[2], "wb");
        if (out == NULL)
            goto done;

        ts_snipper_write(tsn, (TsSnipperWriteFunc)write_stream_cb, out);
        fclose(out);
    }
#endif
done:
#endif

    ts_snipper_destroy(app.tsn);

    return 0;
}

