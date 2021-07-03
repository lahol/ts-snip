#include "filetype.h"
#include <magic.h>

TsFileType ts_get_file_type(const gchar *filename)
{
    if (filename == NULL || filename[0] == 0)
        return TsFileTypeUnknown;

    /* Accept .ts files as video */
    if (g_str_has_suffix(filename, ".ts"))
        return TsFileTypeTransportStream;

    /* Try to guess using libmagic.
     * Accept application/octet-stream or video/MP2T as ts,
     * application/json as project.
     */

    magic_t cookie = magic_open(MAGIC_MIME | MAGIC_CONTINUE);
    if (cookie == NULL)
        return TsFileTypeUnknown;

    if (magic_load(cookie, NULL) != 0) {
        magic_close(cookie);
        return TsFileTypeUnknown;
    }

    const char *mimetype = magic_file(cookie, filename);
    TsFileType ft = TsFileTypeUnknown;
    if (g_strstr_len(mimetype, -1, "application/json")) {
        ft = TsFileTypeProject;
    }
    else if (g_strstr_len(mimetype, -1, "application/octet-stream")
            || g_strstr_len(mimetype, -1, "video/MP2T")) {
        ft = TsFileTypeTransportStream;
    }

    magic_close(cookie);

    return ft;
}
