# TODO

## Bottom Panel: Tabbed, Manifest-Driven Tool Objects

The bottom panel (`build_bottom_panel` in [ui_gtk_object_list.c](src/ui/gtk/ui_gtk_object_list.c)) is currently a single, non-tabbed tree view hard-wired to show Listeners → Connections from the `ObjectRegistry`. The next major feature makes it tabbed and extensible: any tool run from the `toolkit/` sidebar can declare, via a manifest file, its own bottom-panel tab showing whatever "objects" that tool produces (e.g. scan results, findings, discovered hosts) — without the app needing to know about that tool type ahead of time. This turns the bottom panel from a single built-in view into a general per-tool reporting surface, while keeping the existing Listener/Connection panel as the trusted, unchanged built-in tab.

Decisions already made with the user:
- **Data channel**: a tool writes structured records to a data file; the app watches it via the existing FileWatcher and re-renders. No new IPC/socket surface.
- **Format**: JSON. Since the codebase has zero external dependencies today, a small hand-rolled JSON parser is added to `src/`, not vendored from a third party (nothing pulled in unreviewed) — this keeps with the project's existing style of hand-writing its own serialization code (`database.c` already hand-writes JSON/YAML for export).
- **Tab lifecycle**: a tool's panel tab exists only while that tool is running (tied to its terminal process), mirroring how listener/connection state already works elsewhere in the app.
- **Scope boundary**: the built-in Listener/Connection tab is explicitly *not* refactored onto the generic system — it stays a separate, hard-coded, already-tested implementation. The generic manifest system is purely additive, living alongside it in the same notebook so the UI reads as one unified panel.

### Manifest & data-file convention

A tool opts in by placing a sibling manifest next to its script in `toolkit/`, e.g. `toolkit/nmap_scan.sh` + `toolkit/nmap_scan.sh.manifest.json`:

```json
{
  "panel": {
    "title": "Nmap Results",
    "data_file": "nmap_scan.out.jsonl",
    "columns": [
      { "key": "host", "label": "Host" },
      { "key": "port", "label": "Port" },
      { "key": "state", "label": "State" }
    ]
  }
}
```

- `data_file` is resolved relative to the manifest's own directory, then re-validated through the existing `workspace_root_resolve_path()` containment check against the toolkit `WorkspaceRoot` — same sandboxing discipline used everywhere else in `files/`.
- The data file itself is **JSON Lines**: one JSON object per line, one line per "object"/row. Missing declared keys render blank; extra keys are ignored. Columns are fixed by the manifest (not auto-derived from data) so the tab's shape doesn't jitter as rows arrive.
- A malformed manifest or unreachable/escaping `data_file` means no panel tab is created — the tool's terminal still runs normally, matching the existing "silently skip what can't be watched" resilience already documented in `FileWatcher`.
- **Scope note**: only tools launched via "Run in Terminal" into a **new** terminal tab (`run_command_in_new_terminal`) get a panel tab. Running a manifest-bearing tool into an already-active shared shell (`run_command_in_active_terminal`) does not, because in that case there's no distinct child-process exit signal for the tool itself — `TerminalSession.running`/`exit_code` belong to the shell, not the individual command run inside it.

### New modules

- **`src/tools/json_parser.h` / `.c`** — minimal hand-rolled recursive-descent JSON parser covering exactly what manifests/data files need: objects, arrays, strings, numbers, booleans, null. Small, allocation-owning result tree (`JsonValue`), a `json_parse(const char *text, JsonValue **out)`/`json_free()` pair. Modeled stylistically on this codebase's existing hand-rolled parsers (`toolkit_index.c`'s directory scan, `ansi_strip.c`).
- **`src/tools/tool_panel_manifest.h` / `.c`** — `ToolPanelManifest` struct (title, resolved absolute `data_file` path, `ToolPanelColumn[]`), `tool_panel_manifest_load(const WorkspaceRoot *toolkit_root, const char *script_relative_path, ToolPanelManifest *out)`. Looks for `<script_relative_path>.manifest.json`, parses via `json_parser`, validates required fields, and resolves/contains `data_file` via `workspace_root_resolve_path()`.
- **`src/ui/gtk/ui_gtk_tool_panel.c`** (+ declarations added to [ui_gtk_backend.h](src/ui/gtk/ui_gtk_backend.h)) — GTK glue:
  - `ToolPanelTab` struct: `terminal_tab_id` (the owning `Tab`'s id), `title`, resolved `data_file` path + its parent directory (for watching), `ToolPanelColumn[]`, `GtkListStore *store`, `GtkWidget *page_widget`, running/stopped flag.
  - `backend->tool_panel_tabs` (`GPtrArray *`, same ownership convention as `backend->terminal_entries`).
  - `open_tool_panel_tab_for_launch(backend, tab_id, manifest)` — called right after a successful `run_command_in_new_terminal` when a manifest was found; builds the `GtkListStore`/`GtkTreeView` from `manifest->columns`, appends a new page to `backend->bottom_panel_notebook`, and calls `file_watcher_watch_directory(backend->toolkit_watcher, data_file_parent_dir)` (idempotent, per existing `FileWatcher` docs, so it's safe even if another tab already watches that directory).
  - `tool_panel_handle_watch_event(backend, const FileWatchEvent *event)` — called from the tick loop; if `event->new_relative_path` matches an active tab's toolkit-relative `data_file`, fully re-reads and re-parses that file (JSON Lines), and resyncs the tab's `GtkListStore` using the same bidirectional diff-in-place idiom `refresh_object_panel`/`sync_connections_for_listener` in `ui_gtk_object_list.c` already establish (row identity = line number within the file, since these are tool-generated logs, not addressable objects with their own ids).
  - `tool_panel_sync_running_state(backend)` — called from the tick loop; for each `ToolPanelTab`, looks up its `terminal_tab_id`'s `TerminalSession` and, on `running` flipping to 0, marks the tab "(stopped)" in its label (tabs persist after exit, same "state changes, row doesn't disappear" convention as listener/connection rows — user closes it manually via the existing `ui_gtk_tab_close.c`-style close button).

### Changes to existing files

- **[ui_gtk_object_list.c](src/ui/gtk/ui_gtk_object_list.c)**: extract the current `build_bottom_panel` body (tree store + view + columns + scroller) into `build_object_panel_page(backend)`. `build_bottom_panel` becomes: create `backend->bottom_panel_notebook` (`GtkNotebook`), append `build_object_panel_page(backend)` as page 0 labeled "Objects" (no close button — permanent), return the notebook. `refresh_object_panel` is unchanged.
- **[ui_gtk_backend.h](src/ui/gtk/ui_gtk_backend.h)**: add `GtkWidget *bottom_panel_notebook;` and `GPtrArray *tool_panel_tabs;` to `GtkBackend`; declare the new `ui_gtk_tool_panel.c` functions.
- **[ui_gtk_window.c](src/ui/gtk/ui_gtk_window.c)**: in `on_tick`, after the existing `drain_and_apply_watcher(backend, backend->toolkit_watcher, EXPLORER_SOURCE_TOOLKIT)` call, also drain toolkit watch events into `tool_panel_handle_watch_event` (both consumers can observe the same drained batch — same pattern `on_tick` already uses for `listener_system_pump`'s events feeding multiple downstream reactions). Add a call to `tool_panel_sync_running_state(backend)`.
- **[ui_gtk_run_dialog.c](src/ui/gtk/ui_gtk_run_dialog.c)** and wherever plain "Run in Terminal" (no-arguments) is wired (`ui_gtk_explorer_menu.c`): after a `run_command_in_new_terminal` call succeeds, call `tool_panel_manifest_load()` on the launched script's path; if it returns a valid manifest, call `open_tool_panel_tab_for_launch()`.
- **`CMakeLists.txt`**: add `src/tools/json_parser.c`, `src/tools/tool_panel_manifest.c`, `src/ui/gtk/ui_gtk_tool_panel.c` to the `workbench_core` target; add corresponding test executables to `tests/CMakeLists.txt`.

### Tests

- **`tests/json_parser_test.c`** — unit tests for the new parser: valid objects/arrays/nesting, the specific shapes manifests/data lines use, and malformed input rejected cleanly.
- **`tests/tool_panel_manifest_test.c`** — unit tests for `tool_panel_manifest_load`: valid manifest, missing/absent manifest (no error, just "not found"), malformed JSON, and a `data_file` value that attempts to escape the toolkit root (must be rejected via the existing containment check).
- **`tests/tool_panel_smoke.c`** — GTK smoke test modeled on `tests/listener_console_tab_smoke.c` and `tests/file_watch_explorer_smoke.c`'s pump-driven polling style: a fixture script + manifest in a temp toolkit dir, launched via the real "Run in Terminal" path, asserts a new bottom-panel notebook page appears with rows matching appended JSON lines, and that the tab is marked stopped once the fixture script exits.

### Verification

1. `cmake --build build && ctest --test-dir build` — full existing suite (49 tests today) plus the three new test files must pass.
2. Manual run: `./build/workbench`, drop a small fixture tool + `*.manifest.json` + a script that appends JSON lines on a timer into `toolkit/`, launch it via "Run in Terminal", and confirm in the live UI: a new bottom-panel tab appears labeled per the manifest, rows populate and update live as the data file grows, the existing "Objects" tab (Listeners/Connections) is unaffected, and the tool's tab shows "(stopped)" once the script exits.
