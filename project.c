#include "project.h"
#include <json-glib/json-glib.h>
#include <json-glib/json-gobject.h>

typedef struct {
    guint32 begin;
    guint32 end;
} SliceFrames;

struct _TsSnipperProject {
    gchar *input_filename;
    gchar *sha1sum; /* The sha1 saved in the project file for validation. */

    TsSnipper *tsn;

    GList *slices; /* SliceFrames */

    GArray *disabled_pids;
};

TsSnipperProject *ts_snipper_project_new(void)
{
    TsSnipperProject *project = g_new0(TsSnipperProject, 1);
    return project;
}

void ts_snipper_project_destroy(TsSnipperProject *project)
{
    if (project) {
        ts_snipper_unref(project->tsn);
        if (project->disabled_pids != NULL)
            g_array_free(project->disabled_pids, TRUE);
        g_list_free_full(project->slices, g_free);
        g_free(project->input_filename);
        g_free(project->sha1sum);
        g_free(project);
    }
}

static void _ts_snipper_project_read_input(TsSnipperProject *project, JsonNode *node)
{
    if (!JSON_NODE_HOLDS_OBJECT(node))
        return;
    JsonObject *obj = json_node_get_object(node);
    if (json_object_has_member(obj, "path")) {
        project->input_filename = g_strdup(json_object_get_string_member(obj, "path"));
    }
    if (json_object_has_member(obj, "sha1")) {
        project->sha1sum = g_strdup(json_object_get_string_member(obj, "sha1"));
    }
}

static void _ts_snipper_project_read_slices(TsSnipperProject *project, JsonNode *node)
{
    if (!JSON_NODE_HOLDS_ARRAY(node))
        return;
    GList *slices = json_array_get_elements(json_node_get_array(node));
    GList *tmp;
    JsonArray *slice;
    SliceFrames *slice_frames;
    for (tmp = slices; tmp; tmp = g_list_next(tmp)) {
        if (!JSON_NODE_HOLDS_ARRAY(tmp->data))
            continue;
        slice = json_node_get_array((JsonNode *)tmp->data);
        if (slice) {
            slice_frames = g_new0(SliceFrames, 1);
            slice_frames->begin = json_array_get_int_element(slice, 0);
            slice_frames->end = json_array_get_int_element(slice, 1);
            project->slices = g_list_prepend(project->slices, slice_frames);
        }
    }

    g_list_free(slices);
}

static void _ts_snipper_project_read_disabled_pids(TsSnipperProject *project, JsonNode *node)
{
    if (!JSON_NODE_HOLDS_ARRAY(node))
        return;
    GList *pids = json_array_get_elements(json_node_get_array(node));
    GList *tmp;
    if (project->disabled_pids == NULL) {
        project->disabled_pids = g_array_new(FALSE, FALSE, sizeof(guint16));
    }
    else {
        g_array_set_size(project->disabled_pids, 0);
    }
    for (tmp = pids; tmp; tmp = g_list_next(tmp)) {
        guint16 entry = json_node_get_int((JsonNode *)tmp->data);
        g_array_append_val(project->disabled_pids, entry);
    }
    g_list_free(pids);
}

static void ts_snipper_project_read(TsSnipperProject *project, JsonNode *root)
{
    if (!JSON_NODE_HOLDS_OBJECT(root))
        return;

    JsonObject *root_obj = json_node_get_object(root);
    _ts_snipper_project_read_input(project, json_object_get_member(root_obj, "input"));
    _ts_snipper_project_read_slices(project, json_object_get_member(root_obj, "slices"));
    if (json_object_has_member(root_obj, "piddisable")) {
        _ts_snipper_project_read_disabled_pids(project,
                json_object_get_member(root_obj, "piddisable"));
    }
}

TsSnipperProject *ts_snipper_project_new_from_file(const gchar *project_file)
{
    TsSnipperProject *project = ts_snipper_project_new();
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, project_file, NULL)) {
        g_object_unref(parser);
        goto err;
    }

    JsonNode *root = json_parser_get_root(parser);
    ts_snipper_project_read(project, root);
    g_object_unref(parser);

    project->tsn = ts_snipper_new(project->input_filename);
    if (project->tsn == NULL)
        goto err;

    if (project->disabled_pids != NULL) {
        guint i;
        for (i = 0; i < project->disabled_pids->len; ++i) {
            ts_snipper_disable_pid(project->tsn,
                    g_array_index(project->disabled_pids, guint16, i));
        }
    }

    return project;

err:
    ts_snipper_project_destroy(project);
    return NULL;
}

void ts_snipper_project_set_snipper(TsSnipperProject *project, TsSnipper *snipper)
{
    if (project) {
        if (project->tsn)
            ts_snipper_unref(project->tsn);
        project->tsn = snipper;
        g_list_free_full(project->slices, g_free);
        project->slices = NULL;
        g_free(project->input_filename);
        project->input_filename = g_strdup(ts_snipper_get_filename(snipper));
        ts_snipper_ref(project->tsn);
    }
}

TsSnipper *ts_snipper_project_get_snipper(TsSnipperProject *project)
{
    return project ? project->tsn : NULL;
}

gboolean ts_snipper_project_validate(TsSnipperProject *project)
{
    g_return_val_if_fail(project != NULL, FALSE);
    return (project->sha1sum == NULL || /* allow any sum if not set in project */
            0 == g_strcmp0(project->sha1sum,
                           ts_snipper_get_sha1sum(project->tsn)));
}

void ts_snipper_project_apply_slices(TsSnipperProject *project)
{
    g_return_if_fail(project != NULL);
    GList *tmp;
    for (tmp = project->slices; tmp; tmp = g_list_next(tmp)) {
        ts_snipper_add_slice(project->tsn,
                             ((SliceFrames *)tmp->data)->begin,
                             ((SliceFrames *)tmp->data)->end);
    }
}

static void _ts_snipper_project_write_input(TsSnipperProject *project, JsonBuilder *builder)
{
    json_builder_set_member_name(builder, "path");
    json_builder_add_string_value(builder, ts_snipper_get_filename(project->tsn));

    const gchar *sha1sum = ts_snipper_get_sha1sum(project->tsn);
    if (sha1sum) {
        json_builder_set_member_name(builder, "sha1");
        json_builder_add_string_value(builder, sha1sum);
    }
}

static gboolean _ts_snipper_project_write_slice_cb(TsSlice *slice, JsonBuilder *builder)
{
    json_builder_begin_array(builder);
    json_builder_add_int_value(builder, slice->begin_frame);
    json_builder_add_int_value(builder, slice->end_frame);
    json_builder_end_array(builder);
    return TRUE;
}

gboolean ts_snipper_project_write(TsSnipperProject *project, const gchar *filename)
{
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "version");
    json_builder_add_string_value(builder, "1.0");

    json_builder_set_member_name(builder, "input");
    json_builder_begin_object(builder); /* input */
    _ts_snipper_project_write_input(project, builder);
    json_builder_end_object(builder); /* input */

    json_builder_set_member_name(builder, "slices");
    json_builder_begin_array(builder);
    ts_snipper_enum_slices(project->tsn,
                           (TsSnipperEnumSlicesFunc)_ts_snipper_project_write_slice_cb,
                           builder);
    json_builder_end_array(builder);

    json_builder_end_object(builder); /* main */

    JsonNode *root = json_builder_get_root(builder);
    g_object_unref(builder);

    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, root);

    gboolean success = json_generator_to_file(generator, filename, NULL);

    json_node_free(root);
    g_object_unref(generator);

    return success;
}
