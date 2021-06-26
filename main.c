#include <glib.h>
#include <stdio.h>
#include <libavcodec/avcodec.h>

#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#include "ts-snipper.h"

#if 0
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
#endif

struct TsSnipApp {
    TsSnipper *tsn;
    GtkWidget *main_window;
    GtkWidget *drawing_area;
    GtkWidget *slider;
    GtkAdjustment *adjust_stream_pos;

    guint32 frame_id;
    cairo_surface_t *current_iframe_surf;
    AVFrame *current_iframe;
    gdouble aspect_scale;

    GMutex frame_lock;
    GMutex slider_lock;
    GMutex snipper_lock;

    gboolean analyze_in_progress;
} app;

static void rebuild_surface(void);

void main_app_init(void)
{
    memset(&app, 0, sizeof(struct TsSnipApp));

    g_mutex_init(&app.frame_lock);
    g_mutex_init(&app.slider_lock);
    g_mutex_init(&app.snipper_lock);
}

void main_app_set_file(const char *filename)
{
    g_mutex_lock(&app.snipper_lock);
    ts_snipper_destroy(app.tsn);
    app.tsn = ts_snipper_new(filename);
    g_mutex_unlock(&app.snipper_lock);
}

void main_app_cleanup(void)
{
    g_mutex_clear(&app.frame_lock);
    g_mutex_clear(&app.slider_lock);
    g_mutex_clear(&app.snipper_lock);

    if (app.current_iframe_surf)
        cairo_surface_destroy(app.current_iframe_surf);
    if (app.current_iframe)
        av_frame_free(&app.current_iframe);
    ts_snipper_destroy(app.tsn);
}

void main_adjust_slider(void)
{
    g_mutex_lock(&app.slider_lock);
    gdouble value = gtk_adjustment_get_value(GTK_ADJUSTMENT(app.adjust_stream_pos));
    guint32 iframe_count = ts_snipper_get_iframe_count(app.tsn);
    gtk_adjustment_configure(GTK_ADJUSTMENT(app.adjust_stream_pos),
                             value, /* value */
                             0.0, /* lower */
                             (gdouble)iframe_count, /* upper */
                             1.0, /* step increment */
                             100.0, /* page_increment */
                             0.0); /* page_size */
    g_mutex_unlock(&app.slider_lock);

}

static gboolean update_drawing_area(void)
{
    main_adjust_slider();
    rebuild_surface();
    return FALSE;
}

static gpointer main_analyze_file_thread(gpointer nil)
{
    /* lock? */
    app.analyze_in_progress = TRUE;
    ts_snipper_analyze(app.tsn);
    app.analyze_in_progress = FALSE;

    g_idle_add((GSourceFunc)update_drawing_area, NULL);

    g_thread_unref(g_thread_self());
    return NULL;
}

static gboolean main_display_progress(gpointer nil)
{
    gsize done, full;

    if (ts_snipper_get_analyze_status(app.tsn, &done, &full))
        fprintf(stderr, "\rProgress: %6.2f%%",
                ((gdouble)done)/((gdouble)full)*100.0f);
    return app.analyze_in_progress;
}

static void main_analyze_file_async(void)
{
    g_thread_new("AnalyzeTS",
                 (GThreadFunc)main_analyze_file_thread,
                  NULL);
    g_timeout_add(200, (GSourceFunc)main_display_progress, NULL);
}

void cairo_render_current_frame(cairo_surface_t **surf, gdouble *aspect, AVFrame *frame)
{
    if (surf == NULL || frame == NULL)
        return;
    if (*surf != NULL && (cairo_image_surface_get_width(*surf) != frame->width ||
                          cairo_image_surface_get_height(*surf) != frame->height)) {
        cairo_surface_destroy(*surf);
        *surf = NULL;
    }
    if (*surf == NULL) {
        *surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, frame->width, frame->height);
        if (cairo_surface_status(*surf) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(*surf);
            *surf = NULL;
            return;
        }
    }
    if (aspect) *aspect = ((gdouble)frame->sample_aspect_ratio.num) / ((gdouble)frame->sample_aspect_ratio.den);

    /* flush all pending drawing actions. */
    cairo_surface_flush(*surf);
    int stride = cairo_image_surface_get_stride(*surf);
    guchar *surf_data = cairo_image_surface_get_data(*surf);

    int y, x;
    for (y = 0; y < frame->height; ++y) {
        for (x = 0; x < frame->width; ++x) {
            *((guint32 *)(surf_data + y * stride + x * sizeof(guint32)))
                = frame->data[0][y * frame->linesize[0] + x] * 0x00010101;
        }
    }

    cairo_surface_mark_dirty(*surf);
}

/* TODO move to own file. */
gboolean decode_image(PESFrameInfo *frame_info, guint8 *buffer, gsize length)
{
    if (!frame_info || !buffer)
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
    if (app.current_iframe == NULL)
        app.current_iframe = av_frame_alloc();

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

    if ((rc = avcodec_receive_frame(context, app.current_iframe)) < 0)
        goto done;

done:
    av_packet_free(&packet);
    avcodec_free_context(&context);

    return (rc == 0);

}

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

static gboolean main_drawing_area_draw(GtkWidget *widget, cairo_t *cr, gpointer nil)
{
    guint width, height;
    GtkStyleContext *context;

    context = gtk_widget_get_style_context(widget);

    width = gtk_widget_get_allocated_width(widget);
    height = gtk_widget_get_allocated_height(widget);

    gtk_render_background(context, cr, 0, 0, width, height);

    if (app.current_iframe_surf == NULL)
        return FALSE;

    gdouble surf_width = app.aspect_scale * cairo_image_surface_get_width(app.current_iframe_surf);
    gdouble surf_height = (gdouble)cairo_image_surface_get_height(app.current_iframe_surf);

    gdouble ratio_width = width / surf_width;
    gdouble ratio_height = height / surf_height;

    if (ratio_width < ratio_height) {
        /* full fit for width */
        cairo_translate(cr, 0.0, 0.5 * (height - surf_height * ratio_width));
        cairo_scale(cr, app.aspect_scale * ratio_width, ratio_width);
    }
    else {
        /* full fit for height */
        cairo_translate(cr, 0.5 * (width - surf_width * ratio_height), 0.0);
        cairo_scale(cr, app.aspect_scale * ratio_height, ratio_height);
    }

    cairo_set_source_surface(cr, app.current_iframe_surf, 0, 0);
    cairo_paint(cr);

    return FALSE;
}

static void rebuild_surface(void)
{
    PESFrameInfo frame_info;
    guint8 *data = NULL;
    gsize length = 0;
    if (ts_snipper_get_iframe_info(app.tsn, &frame_info, app.frame_id)) {
        ts_snipper_get_iframe(app.tsn, &data, &length, app.frame_id);
    }

    g_mutex_lock(&app.frame_lock);
    if (decode_image(&frame_info, data, length)) {
        cairo_render_current_frame(&app.current_iframe_surf, &app.aspect_scale, app.current_iframe);
    }
    g_free(data);
    g_mutex_unlock(&app.frame_lock);

    gtk_widget_queue_draw(app.drawing_area);
}

static void main_adjustment_value_changed(GtkAdjustment *adjustment, gpointer nil)
{
    g_mutex_lock(&app.slider_lock);
    app.frame_id = (guint32)gtk_adjustment_get_value(adjustment);
    g_mutex_unlock(&app.slider_lock);
    rebuild_surface();
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

    main_app_init();
    main_app_set_file(argv[1]);

    if (!app.tsn) {
        fprintf(stderr, "Error opening file.\n");
        exit(1);
    }

    main_init_window();

    main_analyze_file_async();

    gtk_window_present(GTK_WINDOW(app.main_window));
    rebuild_surface();

    /* Just to test marking for cutting. */
    gtk_scale_add_mark(GTK_SCALE(app.slider), 50, GTK_POS_BOTTOM, /*"&#x21e4;"*/"[");
    gtk_scale_add_mark(GTK_SCALE(app.slider), 100, GTK_POS_BOTTOM, /*"&#x21e5;"*/"]");

    gtk_main();

    fprintf(stderr, "Quit\n");
    main_app_cleanup();

    return 0;
}

