#pragma once

#include <glib.h>

typedef enum {
    TsFileTypeUnknown,
    TsFileTypeTransportStream,
    TsFileTypeProject
} TsFileType;

TsFileType ts_get_file_type(const gchar *filename);
