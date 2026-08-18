#ifndef WORKBENCH_TOOL_PANEL_MANIFEST_H
#define WORKBENCH_TOOL_PANEL_MANIFEST_H

#include <stdbool.h>

#include "files/workspace_root.h"

#define TOOL_PANEL_MANIFEST_MAX_COLUMNS 16

typedef struct ToolPanelColumn {
    char key[64];   /* matches a key in each JSON-Lines data row */
    char label[64]; /* column header shown in the panel */
} ToolPanelColumn;

typedef struct ToolPanelManifest {
    char title[128];
    char data_file_absolute_path[4096]; /* resolved + containment-checked against toolkit_root */
    ToolPanelColumn columns[TOOL_PANEL_MANIFEST_MAX_COLUMNS];
    int column_count;
} ToolPanelManifest;

/*
 * script_absolute_path is a tool's own resolved executable path, already
 * known by the caller to resolve inside toolkit_root (e.g. via
 * workspace_root_resolve_path() before launching it). Looks for a
 * sibling "<script_absolute_path>.manifest.json" file; if present,
 * parses and validates it (required panel.title/data_file/columns[],
 * each column needing non-empty key/label), then resolves the declared
 * data_file relative to the manifest's own directory and re-validates
 * it via workspace_root_resolve_path() against toolkit_root - the same
 * sandboxing discipline every other toolkit/files path in this app goes
 * through, so a manifest can never point its data file outside the
 * toolkit root.
 *
 * Returns true and populates *out on a valid manifest. Returns false
 * (leaving *out untouched) if no manifest file exists, or if one exists
 * but is malformed/invalid in any way - callers should treat both cases
 * identically (just run the tool with no panel tab), not as an error to
 * surface to the user.
 */
bool tool_panel_manifest_load(const WorkspaceRoot *toolkit_root, const char *script_absolute_path,
                               ToolPanelManifest *out);

#endif /* WORKBENCH_TOOL_PANEL_MANIFEST_H */
