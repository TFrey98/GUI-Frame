/*
 * Exercises tool_panel_manifest_load() against a fixture toolkit root:
 * no manifest present, a valid manifest, malformed JSON, a manifest
 * missing a required field, and a data_file value that attempts to
 * escape the toolkit root - the last of which must be rejected the same
 * way every other toolkit/files path in this app is sandboxed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files/workspace_root.h"
#include "tools/tool_panel_manifest.h"

static int status = 0;

static void fail(const char *message) {
    fprintf(stderr, "tool_panel_manifest_test: %s\n", message);
    status = 1;
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

int main(void) {
    char root_template[] = "/tmp/tpm_test_root_XXXXXX";
    char *root_dir = mkdtemp(root_template);
    if (!root_dir) {
        fprintf(stderr, "tool_panel_manifest_test: mkdtemp failed\n");
        return 1;
    }

    WorkspaceRoot toolkit_root;
    if (!workspace_root_init_at(&toolkit_root, root_dir)) {
        fprintf(stderr, "tool_panel_manifest_test: workspace_root_init_at failed\n");
        return 1;
    }

    char script_path[4300];
    snprintf(script_path, sizeof(script_path), "%s/scan.sh", root_dir);
    write_file(script_path, "#!/bin/sh\necho hi\n");

    ToolPanelManifest manifest;

    /* No manifest file at all - not an error, just "no panel tab". */
    if (tool_panel_manifest_load(&toolkit_root, script_path, &manifest)) {
        fail("a script with no sibling manifest should return false");
    }

    /* A valid manifest. */
    char manifest_path[4400];
    snprintf(manifest_path, sizeof(manifest_path), "%s.manifest.json", script_path);
    write_file(manifest_path,
               "{ \"panel\": { \"title\": \"Scan Results\", \"data_file\": \"scan.out.jsonl\", "
               "\"columns\": [ {\"key\": \"host\", \"label\": \"Host\"}, {\"key\": \"port\", \"label\": \"Port\"} ] "
               "} }");
    if (!tool_panel_manifest_load(&toolkit_root, script_path, &manifest)) {
        fail("a valid manifest should load successfully");
    } else {
        if (strcmp(manifest.title, "Scan Results") != 0) {
            fail("manifest.title should be \"Scan Results\"");
        }
        if (manifest.column_count != 2 || strcmp(manifest.columns[0].key, "host") != 0 ||
            strcmp(manifest.columns[1].label, "Port") != 0) {
            fail("manifest.columns should have 2 entries matching the declared key/label pairs");
        }
        char expected_data_file[4300];
        snprintf(expected_data_file, sizeof(expected_data_file), "%s/scan.out.jsonl", root_dir);
        if (strcmp(manifest.data_file_absolute_path, expected_data_file) != 0) {
            fail("data_file should resolve relative to the manifest's own directory, inside the toolkit root");
        }
    }

    /* A manifest for a script nested one directory deep - data_file
     * should resolve relative to that subdirectory, not the toolkit
     * root itself. */
    char subdir[4300];
    snprintf(subdir, sizeof(subdir), "%s/nested", root_dir);
    mkdir(subdir, 0755);
    char nested_script[4400];
    snprintf(nested_script, sizeof(nested_script), "%s/scan2.sh", subdir);
    write_file(nested_script, "#!/bin/sh\necho hi\n");
    char nested_manifest_path[4500];
    snprintf(nested_manifest_path, sizeof(nested_manifest_path), "%s.manifest.json", nested_script);
    write_file(nested_manifest_path,
               "{ \"panel\": { \"title\": \"Nested\", \"data_file\": \"out.jsonl\", "
               "\"columns\": [ {\"key\": \"a\", \"label\": \"A\"} ] } }");
    if (!tool_panel_manifest_load(&toolkit_root, nested_script, &manifest)) {
        fail("a valid manifest for a nested script should load successfully");
    } else {
        char expected_nested_data_file[4500];
        snprintf(expected_nested_data_file, sizeof(expected_nested_data_file), "%s/out.jsonl", subdir);
        if (strcmp(manifest.data_file_absolute_path, expected_nested_data_file) != 0) {
            fail("a nested script's data_file should resolve relative to its own directory");
        }
    }

    /* Malformed JSON. */
    char bad_json_script[4300];
    snprintf(bad_json_script, sizeof(bad_json_script), "%s/bad_json.sh", root_dir);
    write_file(bad_json_script, "#!/bin/sh\n");
    char bad_json_manifest[4400];
    snprintf(bad_json_manifest, sizeof(bad_json_manifest), "%s.manifest.json", bad_json_script);
    write_file(bad_json_manifest, "{ this is not valid json");
    if (tool_panel_manifest_load(&toolkit_root, bad_json_script, &manifest)) {
        fail("malformed JSON should fail to load");
    }

    /* Missing a required field (columns). */
    char missing_field_script[4300];
    snprintf(missing_field_script, sizeof(missing_field_script), "%s/missing_field.sh", root_dir);
    write_file(missing_field_script, "#!/bin/sh\n");
    char missing_field_manifest[4400];
    snprintf(missing_field_manifest, sizeof(missing_field_manifest), "%s.manifest.json", missing_field_script);
    write_file(missing_field_manifest, "{ \"panel\": { \"title\": \"No Columns\", \"data_file\": \"out.jsonl\" } }");
    if (tool_panel_manifest_load(&toolkit_root, missing_field_script, &manifest)) {
        fail("a manifest missing panel.columns should fail to load");
    }

    /* data_file attempting to escape the toolkit root. */
    char escape_script[4300];
    snprintf(escape_script, sizeof(escape_script), "%s/escape.sh", root_dir);
    write_file(escape_script, "#!/bin/sh\n");
    char escape_manifest[4400];
    snprintf(escape_manifest, sizeof(escape_manifest), "%s.manifest.json", escape_script);
    write_file(escape_manifest, "{ \"panel\": { \"title\": \"Escape\", \"data_file\": \"../../etc/passwd\", "
                                 "\"columns\": [ {\"key\": \"a\", \"label\": \"A\"} ] } }");
    if (tool_panel_manifest_load(&toolkit_root, escape_script, &manifest)) {
        fail("a data_file containing '..' should be rejected, not resolved outside the toolkit root");
    }

    if (status == 0) {
        printf("tool_panel_manifest_test: missing manifest, valid manifest (top-level and nested), malformed "
               "JSON, missing field, and sandbox-escape rejection all verified\n");
    }
    return status;
}
