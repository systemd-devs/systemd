/* SPDX-License-Identifier: LGPL-2.1-or-later */


#include "analyze-elf.h"
#include "elf-util.h"
#include "errno-util.h"
#include "fd-util.h"
#include "format-table.h"
#include "format-util.h"
#include "json.h"
#include "path-util.h"
#include "strv.h"

int analyze_elf(char **filenames, JsonFormatFlags json_flags) {
        char **filename;
        int r;

        STRV_FOREACH(filename, filenames) {
                _cleanup_(json_variant_unrefp) JsonVariant *package_metadata = NULL;
                _cleanup_(table_unrefp) Table *t = NULL;
                _cleanup_free_ char *abspath = NULL;
                _cleanup_close_ int fd = -1;

                r = path_make_absolute_cwd(*filename, &abspath);
                if (r < 0)
                        return log_error_errno(r, "Could not make an absolute path out of \"%s\": %m", *filename);

                path_simplify(abspath);

                fd = RET_NERRNO(open(abspath, O_RDONLY|O_CLOEXEC));
                if (fd < 0)
                        return log_error_errno(fd, "Could not open \"%s\": %m", abspath);

                r = parse_elf_object(fd, abspath, NULL, &package_metadata);
                if (r < 0)
                        return log_error_errno(r, "Parsing \"%s\" as ELF object failed: %m", abspath);

                t = table_new("elf metadata", "");
                if (!t)
                        return log_oom();

                r = table_add_many(
                                t,
                                TABLE_STRING, "path",
                                TABLE_STRING, abspath);
                if (r < 0)
                        return table_log_add_error(r);

                if (package_metadata) {
                        JsonVariant *module_json;
                        const char *module_name;

                        JSON_VARIANT_OBJECT_FOREACH(module_name, module_json, package_metadata) {
                                const char *field_name;
                                JsonVariant *field;

                                /* The ELF type and architecture are added as top-level objects,
                                 * since they are only parsed for the file itself, but the packaging
                                 * metadata is parsed recursively in core files, so there might be
                                 * multiple modules. */
                                if (STR_IN_SET(module_name, "elfType", "elfArchitecture")) {
                                        r = table_add_many(
                                                        t,
                                                        TABLE_STRING, module_name,
                                                        TABLE_STRING, json_variant_string(module_json));
                                        if (r < 0)
                                                return table_log_add_error(r);

                                        continue;
                                }

                                /* In case of core files the module name will be the executable,
                                 * but for binaries/libraries it's just the path, so don't print it
                                 * twice. */
                                if (!streq(abspath, module_name)) {
                                        r = table_add_many(
                                                        t,
                                                        TABLE_STRING, "module name",
                                                        TABLE_STRING, module_name);
                                        if (r < 0)
                                                return table_log_add_error(r);
                                }

                                JSON_VARIANT_OBJECT_FOREACH(field_name, field, module_json)
                                        if (json_variant_is_string(field)) {
                                                r = table_add_many(
                                                                t,
                                                                TABLE_STRING, field_name,
                                                                TABLE_STRING, json_variant_string(field));
                                                if (r < 0)
                                                        return table_log_add_error(r);
                                        }
                        }
                }
                if (json_flags & JSON_FORMAT_OFF) {
                        (void) table_set_header(t, true);

                        r = table_print(t, NULL);
                        if (r < 0)
                                return table_log_print_error(r);
                } else
                        json_variant_dump(package_metadata, json_flags, stdout, NULL);
        }

        return 0;
}
