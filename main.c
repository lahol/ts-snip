#include <glib.h>
#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>

#include "ts-snipper.h"
#include "files-async.h"
#include "project.h"
#include "filetype.h"

#define SNIPPER_ACTIVE_SLICE_BEGIN (1 << 0)
#define SNIPPER_ACTIVE_SLICE_END (1 << 1)
#define SNIPPER_MOTION_SLICE_VALID (1 << 2)
typedef struct {
    guint32 flags;
    guint32 frame_begin;
    guint32 frame_end;
} SnipperSlice;

struct TsSnipApp {
    TsSnipper *tsn;
    GtkWidget *main_window;
    GtkWidget *drawing_area;
    GtkWidget *slider;
    GtkAdjustment *adjust_stream_pos;
    GtkWidget *progress_bar;
    GtkAccelGroup *accelerator_group;

    guint32 frame_id;
    cairo_surface_t *current_iframe_surf;
    AVFrame *current_iframe;
    gdouble aspect_scale;

    GMutex frame_lock;
    GMutex snipper_lock;

    SnipperSlice active_slice;
    SnipperSlice motion_slice;

    TsSnipperProject *project;
} app;

static void rebuild_surface(void);

void main_app_init(void)
{
    memset(&app, 0, sizeof(struct TsSnipApp));

    g_mutex_init(&app.frame_lock);
    g_mutex_init(&app.snipper_lock);
}

void main_app_set_file(const char *filename)
{
    g_mutex_lock(&app.snipper_lock);
    ts_snipper_unref(app.tsn);
    app.tsn = ts_snipper_new(filename);
    if (app.project)
        ts_snipper_project_set_snipper(app.project, app.tsn);
    g_mutex_unlock(&app.snipper_lock);
}

void main_app_set_project_file(const char *filename)
{
    g_mutex_lock(&app.snipper_lock);
    ts_snipper_unref(app.tsn);
    ts_snipper_project_destroy(app.project);
    app.project = ts_snipper_project_new_from_file(filename);
    app.tsn = ts_snipper_project_get_snipper(app.project);
    ts_snipper_ref(app.tsn);
    g_mutex_unlock(&app.snipper_lock);
}

void main_app_cleanup(void)
{
    g_mutex_clear(&app.frame_lock);
    g_mutex_clear(&app.snipper_lock);

    if (app.current_iframe_surf)
        cairo_surface_destroy(app.current_iframe_surf);
    if (app.current_iframe)
        av_frame_free(&app.current_iframe);
    ts_snipper_unref(app.tsn);
}

void main_adjust_slider(void)
{
    gdouble value = gtk_adjustment_get_value(GTK_ADJUSTMENT(app.adjust_stream_pos));
    guint32 iframe_count = ts_snipper_get_iframe_count(app.tsn);
    gtk_adjustment_configure(GTK_ADJUSTMENT(app.adjust_stream_pos),
                             value, /* value */
                             0.0, /* lower */
                             (gdouble)(iframe_count - 1), /* upper */
                             1.0, /* step increment */
                             100.0, /* page_increment */
                             0.0); /* page_size */

}

static void main_slider_set_slice_markers(guint32 frame_begin, guint32 frame_end)
{
    if (frame_begin != PES_FRAME_ID_INVALID)
        gtk_scale_add_mark(GTK_SCALE(app.slider), frame_begin, GTK_POS_BOTTOM, /*"&#x21e4;"*/"[");
    if (frame_end != PES_FRAME_ID_INVALID)
        gtk_scale_add_mark(GTK_SCALE(app.slider), frame_end, GTK_POS_BOTTOM, /*"&#x21e5;"*/"]");
}

static gboolean refresh_slice_markers_cb(TsSlice *slice, gpointer nil)
{
    main_slider_set_slice_markers(slice->begin_frame, slice->end_frame);
    return TRUE;
}

static void main_slider_refresh_slice_markers(void)
{
    gtk_scale_clear_marks(GTK_SCALE(app.slider));
    ts_snipper_enum_slices(app.tsn, (TsSnipperEnumSlicesFunc)refresh_slice_markers_cb, NULL);
}

static void main_update_active_slice(guint32 frame_id, guint32 type)
{
    if (type & SNIPPER_ACTIVE_SLICE_BEGIN) {
        app.active_slice.frame_begin = frame_id;
    }
    if (type & SNIPPER_ACTIVE_SLICE_END) {
        app.active_slice.frame_end = frame_id;
    }
    app.active_slice.flags |= type;

    if ((app.active_slice.flags & SNIPPER_ACTIVE_SLICE_BEGIN) &&
            (app.active_slice.flags & SNIPPER_ACTIVE_SLICE_END) &&
            (app.active_slice.frame_begin < app.active_slice.frame_end ||
             app.active_slice.frame_begin == PES_FRAME_ID_INVALID ||
             app.active_slice.frame_end == PES_FRAME_ID_INVALID)) {
        ts_snipper_add_slice(app.tsn, app.active_slice.frame_begin, app.active_slice.frame_end);
        main_slider_refresh_slice_markers();
        app.active_slice.flags = 0;
    }
}

static void main_menu_edit_slice_begin(void)
{
    main_update_active_slice(app.frame_id, SNIPPER_ACTIVE_SLICE_BEGIN);
}

static void main_menu_edit_slice_end(void)
{
    main_update_active_slice(app.frame_id, SNIPPER_ACTIVE_SLICE_END);
}

static void main_menu_edit_slice_remove(void)
{
    guint32 slice_id = ts_snipper_find_slice_for_frame(app.tsn, NULL, app.frame_id, FALSE);
    if (slice_id != TS_SLICE_ID_INVALID) {
        ts_snipper_delete_slice(app.tsn, slice_id);
        main_slider_refresh_slice_markers();
    }
}

static void main_menu_edit_slice_select_begin(void)
{
    if (!(app.motion_slice.flags & SNIPPER_MOTION_SLICE_VALID)) {
        TsSlice slice;
        if (ts_snipper_find_slice_for_frame(app.tsn, &slice, app.frame_id, TRUE) != TS_SLICE_ID_INVALID) {
            app.motion_slice.frame_begin = slice.begin_frame;
            app.motion_slice.frame_end = slice.end_frame;
            app.motion_slice.flags |= SNIPPER_MOTION_SLICE_VALID;
        }
    }

    if (app.motion_slice.flags & SNIPPER_MOTION_SLICE_VALID) {
        gtk_adjustment_set_value(GTK_ADJUSTMENT(app.adjust_stream_pos), (gdouble)app.motion_slice.frame_begin);
    }
}

static void main_menu_edit_slice_select_end(void)
{
    if (!(app.motion_slice.flags & SNIPPER_MOTION_SLICE_VALID)) {
        TsSlice slice;
        if (ts_snipper_find_slice_for_frame(app.tsn, &slice, app.frame_id, TRUE) != TS_SLICE_ID_INVALID) {
            app.motion_slice.frame_begin = slice.begin_frame;
            app.motion_slice.frame_end = slice.end_frame;
            app.motion_slice.flags |= SNIPPER_MOTION_SLICE_VALID;
        }
    }

    if (app.motion_slice.flags & SNIPPER_MOTION_SLICE_VALID) {
        gtk_adjustment_set_value(GTK_ADJUSTMENT(app.adjust_stream_pos), (gdouble)app.motion_slice.frame_end);
    }
}

static void update_drawing_area(void)
{
    main_adjust_slider();
    rebuild_surface();
}

/* just before combining progress for analyze/write; analyze with GTask */
static gboolean main_display_progress(gpointer nil)
{
    gsize done, full;
    TsSnipperState state = ts_snipper_get_state(app.tsn);
    if ((state == TsSnipperStateAnalyzing && ts_snipper_get_analyze_status(app.tsn, &done, &full)) ||
        (state == TsSnipperStateWriting && ts_snipper_get_write_status(app.tsn, &done, &full))) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app.progress_bar),
                ((gdouble)done) / ((gdouble)full));
    }
    return (state == TsSnipperStateAnalyzing || state == TsSnipperStateWriting);
}

static void main_file_analyze_result_func(GObject *source_object,
                                          GAsyncResult *res,
                                          gpointer userdata)
{
    gboolean success = FALSE;
    success = file_read_finish(res, NULL);

    if (success)
        fprintf(stderr, "Analyze: SUCCESS\n");

    gtk_widget_hide(app.progress_bar);
    gtk_adjustment_set_value(GTK_ADJUSTMENT(app.adjust_stream_pos), 0.0);

    if (app.project) {
        if (!ts_snipper_project_validate(app.project))
            fprintf(stderr, "WARNING: Input has changed.\n");
        ts_snipper_project_apply_slices(app.project);
        main_slider_refresh_slice_markers();
    }

    update_drawing_area();
}

static void main_analyze_file_async(void)
{
    file_read_async(app.tsn,
                    NULL,
                    main_file_analyze_result_func,
                    NULL);
    gtk_widget_show(app.progress_bar);
    g_timeout_add(200, (GSourceFunc)main_display_progress, NULL);
}

static void main_file_write_result_func(GObject *source_object,
                                        GAsyncResult *res,
                                        gpointer userdata)
{
    gboolean success = FALSE;

    success = file_write_finish(res, NULL);

    if (success)
        fprintf(stderr, "write file: SUCCESS\n");
    else
        fprintf(stderr, "write file: FAILED\n");

    gtk_widget_hide(app.progress_bar);
}

static void main_write_file_async(const char *filename)
{
    file_write_async(app.tsn, filename,
                     NULL,
                     main_file_write_result_func,
                     NULL);
    gtk_widget_show(app.progress_bar);
    g_timeout_add(200, (GSourceFunc)main_display_progress, NULL);
}

static void frame_to_gray(guchar *surf_data, int stride, AVFrame *frame)
{
    int y, x;
    for (y = 0; y < frame->height; ++y) {
        for (x = 0; x < frame->width; ++x) {
            *((guint32 *)(surf_data + y * stride + x * sizeof(guint32)))
                = frame->data[0][y * frame->linesize[0] + x] * 0x00010101;
        }
    }

}

static void frame_to_rgb_av(guchar *surf_data, int stride, AVFrame *frame)
{
    struct SwsContext *sws_context = sws_getContext(
            frame->width, frame->height, frame->format, /* src */
            frame->width, frame->height, AV_PIX_FMT_RGB32, /* dst */
            0, NULL, NULL, NULL);
    if (sws_context == NULL) {
        /* todo: keep sws_context, check external, but also check format */
        frame_to_gray(surf_data, stride, frame);
        return;
    }
    uint8_t *dst[] = { surf_data };
    int dstStride[] = { stride };
    sws_scale(sws_context,
              (const uint8_t *const *)frame->data, /* srcSlice[] */
              frame->linesize, /* srcStride[] */
              0, frame->height, /* start and end */
              dst, /* dst data */
              dstStride);
    sws_freeContext(sws_context);
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

    frame_to_rgb_av(surf_data, stride, frame);

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
    app.frame_id = (guint32)gtk_adjustment_get_value(adjustment);
    if (app.motion_slice.flags & SNIPPER_MOTION_SLICE_VALID) {
        if (app.frame_id >= app.motion_slice.frame_end ||
            app.frame_id < app.motion_slice.frame_begin) {
            app.motion_slice.flags &= ~SNIPPER_MOTION_SLICE_VALID;
        }
    }
    rebuild_surface();
}

void main_menu_file_open_project(void)
{
    GtkWidget *dialog;
    gint res;
    dialog = gtk_file_chooser_dialog_new(_("Open project"),
            GTK_WINDOW(app.main_window),
            GTK_FILE_CHOOSER_ACTION_OPEN,
            _("_Cancel"),
            GTK_RESPONSE_CANCEL,
            _("_Open"),
            GTK_RESPONSE_ACCEPT,
            NULL);

    res = gtk_dialog_run(GTK_DIALOG(dialog));
    if (res == GTK_RESPONSE_ACCEPT) {
        char *filename;

        filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (app.project)
            ts_snipper_project_destroy(app.project);
        ts_snipper_unref(app.tsn);
        app.project = ts_snipper_project_new_from_file(filename);

        if (app.project) {
            app.tsn = ts_snipper_project_get_snipper(app.project);
            ts_snipper_ref(app.tsn);
            main_analyze_file_async();
        }

        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}

void main_menu_file_save_project(void)
{
    GtkWidget *dialog;
    gint res;
    dialog = gtk_file_chooser_dialog_new(_("Save project"),
            GTK_WINDOW(app.main_window),
            GTK_FILE_CHOOSER_ACTION_SAVE,
            _("_Cancel"),
            GTK_RESPONSE_CANCEL,
            _("_Save"),
            GTK_RESPONSE_ACCEPT,
            NULL);

    gtk_file_chooser_set_do_overwrite_confirmation(
            GTK_FILE_CHOOSER(dialog), TRUE);
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    if (res == GTK_RESPONSE_ACCEPT) {
        char *filename;

        filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));

        if (app.project == NULL) {
            /* Set as active project */
            app.project = ts_snipper_project_new();
            ts_snipper_project_set_snipper(app.project, app.tsn);
        }
        ts_snipper_project_write(app.project, filename);

        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}

void main_menu_file_import(void)
{
    GtkWidget *dialog;
    gint res;
    dialog = gtk_file_chooser_dialog_new(_("Import"),
            GTK_WINDOW(app.main_window),
            GTK_FILE_CHOOSER_ACTION_OPEN,
            _("_Cancel"),
            GTK_RESPONSE_CANCEL,
            _("_Import"),
            GTK_RESPONSE_ACCEPT,
            NULL);

    res = gtk_dialog_run(GTK_DIALOG(dialog));
    if (res == GTK_RESPONSE_ACCEPT) {
        char *filename;

        filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));

        main_app_set_file(filename);
        main_analyze_file_async();

        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}

void main_menu_file_export(void)
{
    GtkWidget *dialog;
    gint res;
    dialog = gtk_file_chooser_dialog_new(_("Export"),
            GTK_WINDOW(app.main_window),
            GTK_FILE_CHOOSER_ACTION_SAVE,
            _("_Cancel"),
            GTK_RESPONSE_CANCEL,
            _("_Export"),
            GTK_RESPONSE_ACCEPT,
            NULL);

    gtk_file_chooser_set_do_overwrite_confirmation(
            GTK_FILE_CHOOSER(dialog), TRUE);
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    if (res == GTK_RESPONSE_ACCEPT) {
        char *filename;

        filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));

        main_write_file_async(filename);
        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}

void main_menu_file_quit(void)
{
    /* TODO: query really quit */
    gtk_main_quit();
}

void _main_add_accelerator(GtkWidget *item, const char *accel_signal, GtkAccelGroup *accel_group,
        guint accel_key, GdkModifierType accel_mods, GtkAccelFlags accel_flags,
        GCallback accel_cb, gpointer accel_data)
{
    gtk_widget_add_accelerator(item, accel_signal, accel_group, accel_key, accel_mods, accel_flags);
    gtk_accel_group_connect(accel_group, accel_key, accel_mods, 0,
            g_cclosure_new_swap(accel_cb, accel_data, NULL));
}

GtkWidget *main_create_main_menu(void)
{
    GtkWidget *menu_bar = gtk_menu_bar_new();
    GtkWidget *menu = gtk_menu_new();

    GtkWidget *item;

    /* File */
    item = gtk_menu_item_new_with_label(_("Open project"));
    g_signal_connect_swapped(G_OBJECT(item), "activate",
            G_CALLBACK(main_menu_file_open_project), NULL);
    _main_add_accelerator(item, "activate", app.accelerator_group, GDK_KEY_o,
            GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE, G_CALLBACK(main_menu_file_open_project), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label(_("Save project"));
    g_signal_connect_swapped(G_OBJECT(item), "activate",
            G_CALLBACK(main_menu_file_save_project), NULL);
    _main_add_accelerator(item, "activate", app.accelerator_group, GDK_KEY_s,
            GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE, G_CALLBACK(main_menu_file_save_project), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label(_("Import"));
    g_signal_connect_swapped(G_OBJECT(item), "activate",
            G_CALLBACK(main_menu_file_import), NULL);
    _main_add_accelerator(item, "activate", app.accelerator_group, GDK_KEY_i,
            GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE, G_CALLBACK(main_menu_file_import), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label(_("Export"));
    g_signal_connect_swapped(G_OBJECT(item), "activate",
            G_CALLBACK(main_menu_file_export), NULL);
    _main_add_accelerator(item, "activate", app.accelerator_group, GDK_KEY_e,
            GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE, G_CALLBACK(main_menu_file_export), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label(_("Quit"));
    g_signal_connect_swapped(G_OBJECT(item), "activate",
            G_CALLBACK(main_menu_file_quit), NULL);
    _main_add_accelerator(item, "activate", app.accelerator_group, GDK_KEY_q,
            GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE, G_CALLBACK(main_menu_file_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label(_("File"));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), item);

    /* Edit */
    menu = gtk_menu_new();
    item = gtk_menu_item_new_with_label(_("Slice begin"));
    g_signal_connect_swapped(G_OBJECT(item), "activate",
            G_CALLBACK(main_menu_edit_slice_begin), NULL);
    _main_add_accelerator(item, "activate", app.accelerator_group, GDK_KEY_bracketleft,
            0, GTK_ACCEL_VISIBLE, G_CALLBACK(main_menu_edit_slice_begin), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label(_("Slice end"));
    g_signal_connect_swapped(G_OBJECT(item), "activate",
            G_CALLBACK(main_menu_edit_slice_end), NULL);
    _main_add_accelerator(item, "activate", app.accelerator_group, GDK_KEY_bracketright,
            0, GTK_ACCEL_VISIBLE, G_CALLBACK(main_menu_edit_slice_end), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label(_("Remove slice"));
    g_signal_connect_swapped(G_OBJECT(item), "activate",
            G_CALLBACK(main_menu_edit_slice_remove), NULL);
    _main_add_accelerator(item, "activate", app.accelerator_group, GDK_KEY_k,
            GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE, G_CALLBACK(main_menu_edit_slice_remove), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label(_("Select slice begin"));
    g_signal_connect_swapped(G_OBJECT(item), "activate",
            G_CALLBACK(main_menu_edit_slice_select_begin), NULL);
    _main_add_accelerator(item, "activate", app.accelerator_group, GDK_KEY_Left,
            GDK_SHIFT_MASK, GTK_ACCEL_VISIBLE, G_CALLBACK(main_menu_edit_slice_select_begin), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label(_("Select slice end"));
    g_signal_connect_swapped(G_OBJECT(item), "activate",
            G_CALLBACK(main_menu_edit_slice_select_end), NULL);
    _main_add_accelerator(item, "activate", app.accelerator_group, GDK_KEY_Right,
            GDK_SHIFT_MASK, GTK_ACCEL_VISIBLE, G_CALLBACK(main_menu_edit_slice_select_end), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label(_("Edit"));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), item);

    return menu_bar;
}

void main_init_window(void)
{
    app.main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_signal_connect(G_OBJECT(app.main_window), "destroy",
            G_CALLBACK(main_quit), NULL);

    app.accelerator_group = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(app.main_window), app.accelerator_group);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);

    GtkWidget *menu_bar = main_create_main_menu();
    gtk_box_pack_start(GTK_BOX(vbox), menu_bar, FALSE, FALSE, 0);

    app.drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(app.drawing_area, 120, 80);
    g_signal_connect(G_OBJECT(app.drawing_area), "size-allocate",
            G_CALLBACK(main_drawing_area_size_allocate), NULL);
    g_signal_connect(G_OBJECT(app.drawing_area), "realize",
            G_CALLBACK(main_drawing_area_realize), NULL);
    g_signal_connect(G_OBJECT(app.drawing_area), "draw",
            G_CALLBACK(main_drawing_area_draw), NULL);

    gtk_box_pack_start(GTK_BOX(vbox), app.drawing_area, TRUE, TRUE, 0);

    app.adjust_stream_pos = gtk_adjustment_new(0.0, 0.0, 0.0, 1.0, 100.0, 0.0);
    g_signal_connect(G_OBJECT(app.adjust_stream_pos), "value-changed",
            G_CALLBACK(main_adjustment_value_changed), NULL);

    app.slider = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, app.adjust_stream_pos);
    gtk_scale_set_has_origin(GTK_SCALE(app.slider), FALSE);
    gtk_scale_set_draw_value(GTK_SCALE(app.slider), FALSE);
    gtk_range_set_round_digits(GTK_RANGE(app.slider), 0);
    gtk_box_pack_end(GTK_BOX(vbox), app.slider, FALSE, FALSE, 15);

    app.progress_bar = gtk_progress_bar_new();
    gtk_box_pack_end(GTK_BOX(vbox), app.progress_bar, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(app.main_window), vbox);

    gtk_widget_show_all(app.main_window);
    gtk_widget_hide(app.progress_bar);
}

int main(int argc, char **argv)
{
    if (!XInitThreads()) {
        fprintf(stderr, "XInitThreads() failed.\n");
        exit(1);
    }

    gtk_init(&argc, &argv);

    main_app_init();
    main_init_window();

    if (argc >= 2) {
        if (ts_get_file_type(argv[1]) != TsFileTypeProject) {
            main_app_set_file(argv[1]);
        }
        else {
            main_app_set_project_file(argv[1]);
        }

        if (!app.tsn) {
            fprintf(stderr, "Error opening file.\n");
            exit(1);
        }
        main_analyze_file_async();
    }

    gtk_window_present(GTK_WINDOW(app.main_window));
    rebuild_surface();

    gtk_main();

    main_app_cleanup();

    return 0;
}

