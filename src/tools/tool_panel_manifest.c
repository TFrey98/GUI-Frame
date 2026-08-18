#include "tool_panel_manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "json_parser.h"

/* Manifests are small, hand-authored config files - anything past this
 * is almost certainly not a real manifest, so it's simplest to just
 * treat it as invalid rather than truncate and try to parse a partial
 * document. */
#define TOOL_PANEL_MANIFEST_MAX_FILE_SIZE (64 * 1024)

static char *read_file_capped(const char *path, size_t max_size) {
    struct stat st;
    if (stat(path, &st) != 0 || (size_t)st.st_size > max_size) {
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    size_t size = (size_t)st.st_size;
    char *buf = malloc(size + 1);
    size_t read_count = fread(buf, 1, size, f);
    fclose(f);
    if (read_count != size) {
        free(buf);
        return NULL;
    }
    buf[size] = '\0';
    return buf;
}

static const char *require_string(const JsonValue *object, const char *key) {
    const JsonValue *value = json_object_get(object, key);
    const char *out = NULL;
    if (!value || !json_as_string(value, &out) || out[0] == '\0') {
        return NULL;
    }
    return out;
}

/* Splits toolkit-root-relative path into its parent directory (may be
 * "" if path has no '/') - out_dir must be at least as large as path. */
static void relative_dirname(const char *path, char *out_dir, size_t out_size) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        out_dir[0] = '\0';
        return;
    }
    size_t len = (size_t)(slash - path);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out_dir, path, len);
    out_dir[len] = '\0';
}

static bool parse_columns(const JsonValue *columns, ToolPanelManifest *out) {
    int count = json_array_count(columns);
    if (count < 1 || count > TOOL_PANEL_MANIFEST_MAX_COLUMNS) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        const JsonValue *column = json_array_get(columns, i);
        const char *key = require_string(column, "key");
        const char *label = require_string(column, "label");
        if (!key || !label) {
            return false;
        }
        snprintf(out->columns[i].key, sizeof(out->columns[i].key), "%s", key);
        snprintf(out->columns[i].label, sizeof(out->columns[i].label), "%s", label);
    }
    out->column_count = count;
    return true;
}

/* Resolves data_file (as declared, relative to the manifest's own
 * directory) against toolkit_root, writing the containment-checked
 * absolute path into out->data_file_absolute_path. script_absolute_path
 * must already resolve inside toolkit_root - callers are expected to
 * have verified that themselves before ever calling us (every launch
 * path already resolves the script through workspace_root_resolve_path
 * first). */
static bool resolve_data_file(const WorkspaceRoot *toolkit_root, const char *script_absolute_path,
                               const char *data_file, ToolPanelManifest *out) {
    size_t root_len = strlen(toolkit_root->canonical_path);
    if (strncmp(script_absolute_path, toolkit_root->canonical_path, root_len) != 0 ||
        script_absolute_path[root_len] != '/') {
        return false;
    }
    const char *script_relative = script_absolute_path + root_len + 1;

    char dir[4096];
    relative_dirname(script_relative, dir, sizeof(dir));

    char combined[8192];
    int written = dir[0] != '\0' ? snprintf(combined, sizeof(combined), "%s/%s", dir, data_file)
                                  : snprintf(combined, sizeof(combined), "%s", data_file);
    if (written < 0 || (size_t)written >= sizeof(combined)) {
        return false;
    }

    return workspace_root_resolve_path(toolkit_root, combined, out->data_file_absolute_path,
                                        sizeof(out->data_file_absolute_path));
}

bool tool_panel_manifest_load(const WorkspaceRoot *toolkit_root, const char *script_absolute_path,
                               ToolPanelManifest *out) {
    char manifest_path[4200];
    int written = snprintf(manifest_path, sizeof(manifest_path), "%s.manifest.json", script_absolute_path);
    if (written < 0 || (size_t)written >= sizeof(manifest_path)) {
        return false;
    }

    char *text = read_file_capped(manifest_path, TOOL_PANEL_MANIFEST_MAX_FILE_SIZE);
    if (!text) {
        return false;
    }

    JsonValue *root = json_parse(text);
    free(text);
    if (!root) {
        return false;
    }

    bool ok = false;
    const JsonValue *panel = json_object_get(root, "panel");
    const char *title = panel ? require_string(panel, "title") : NULL;
    const char *data_file = panel ? require_string(panel, "data_file") : NULL;
    const JsonValue *columns = panel ? json_object_get(panel, "columns") : NULL;

    if (title && data_file && columns && json_value_type(columns) == JSON_ARRAY) {
        ToolPanelManifest candidate = {0};
        snprintf(candidate.title, sizeof(candidate.title), "%s", title);
        if (parse_columns(columns, &candidate) &&
            resolve_data_file(toolkit_root, script_absolute_path, data_file, &candidate)) {
            *out = candidate;
            ok = true;
        }
    }

    json_value_free(root);
    return ok;
}
