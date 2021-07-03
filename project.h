#pragma once

#include <glib.h>
#include "ts-snipper.h"

/** @brief Opaque type to handle projects. */
typedef struct _TsSnipperProject TsSnipperProject;


/** @brief Create a new project.
 *  @return A newly created project.
 */
TsSnipperProject *ts_snipper_project_new(void);

/** @brief Destroy a project.
 */
void ts_snipper_project_destroy(TsSnipperProject *project);

/** @brief Read a project file and create a new snipper.
 */
TsSnipperProject *ts_snipper_project_new_from_file(const gchar *project_file);

/** @brief Set the snipper of the project.
 */
void ts_snipper_project_set_snipper(TsSnipperProject *project, TsSnipper *snipper);

/** @brief Get the snipper from the project.
 */
TsSnipper *ts_snipper_project_get_snipper(TsSnipperProject *project);

/** @brief Validate the snipper after analyzing.
 */
gboolean ts_snipper_project_validate(TsSnipperProject *project);

/** @brief Apply all slices to the snipper.
 *  Has to be done after the snipper is done analyzing.
 */
void ts_snipper_apply_slices(TsSnipperProject *project);

/** @brief Write the project to a file.
 */
gboolean ts_snipper_project_write(TsSnipperProject *project, const gchar *filename);

