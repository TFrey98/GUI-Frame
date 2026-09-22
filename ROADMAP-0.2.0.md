# 0.2.0 — brainstorm

A scratchpad, not a plan. Nothing here is committed to, ordered, or
estimated. Ideas get added freely; they earn a design section only once
they look likely, and only then do they move into `TODO.md`, which is
where worked-out designs live (see the bottom-panel entry there for the
level of detail that means).

## Where 0.1.0 leaves off

Worth stating, because most of what follows either reuses these seams or
has to argue with them:

- **Objects.** `ObjectRegistry` already models listeners and their
  connections as live objects with state, and the bottom panel renders
  them. Tools can add their own tabs through a manifest without the app
  knowing the tool type ahead of time (`src/tools/tool_panel_manifest.*`).
- **Connections.** `ConnectionManager` owns accepted reverse-TCP/HTTP/HTTPS
  connections, each with its own terminal view and single-writer handoff.
- **Terminals.** Each terminal owns its pty directly rather than letting
  VTE spawn it, so input and output are observable — that is what makes
  capture possible at all.
- **Capture.** Every submitted command and its response land in
  `workbench.db`, with the prompt and the command's own echo trimmed off.
- **Files.** A sandboxed `files/` workspace and an indexed `tools/`
  directory, with transfer plumbing already present in
  `src/ui/gtk/ui_gtk_explorer_transfer.c`.

## 1. Top bar: buttons → organized menus

### Today

Eight controls and three labels sit in one flat row, built inline in
`build_main_window()` (`src/ui/gtk/ui_gtk_window.c`):

| Control | Kind |
| --- | --- |
| Save All | icon button |
| + New Listener | button |
| Search | button |
| Export Database | button |
| Clear Database | button |
| *(listener status text)* | label |
| Sidebar | toggle |
| Bottom Panel | toggle |
| Dark Mode | toggle |

(*+ Terminal* is a notebook action widget rather than part of this row,
but it is the same question.)

### Why move

- The row does not scale. Every feature below wants to add to it, and
  network work alone would add several.
- It is a real constraint on layout, not just clutter. Measured: the top
  bar's minimum width is **1144px — exactly the window's minimum**, while
  everything below it (sidebar, terminals, bottom panel) needs only
  620px. The row is not *part* of the floor, it *is* the floor, and it
  costs the rest of the layout ~524px it could otherwise use. Menus would
  collapse it to a handful of short labels.
- Destructive actions sit inches from routine ones. *Clear Database* is
  one mis-click from *Export Database*, with no grouping to signal it.

### A first cut at the grouping

Not settled — the argument is about where *Search* and the toggles
belong:

- **File** — New Terminal, New Listener, Save All
- **Edit** — Search
- **View** — Sidebar, Bottom Panel, Dark Mode
- **Database** — Export…, Clear…
- **Network** — everything from section 2
- **Help** — About

The listener status text is not a control and should probably become a
status bar rather than live in a menu.

### Mechanism

Two options, and the choice matters more than it looks:

1. **`GtkMenuBar` built from widgets** — closest to the existing code,
   which builds everything inline. Keeps the current style, but keeps the
   current testing story too: controls are found by walking the widget
   tree and matching labels.
2. **`GMenu` + `GAction` on the existing `GtkApplication`** — the app
   already is a `GtkApplication` but uses no actions at all today.
   Actions decouple "what the app can do" from "what the menu looks
   like", which is worth something on its own, and they can be activated
   by name.

Leaning towards 2, largely for testability: **nine smoke tests currently
locate top-bar controls by their label text** (`context_menu_smoke`,
`connection_terminal_smoke`, `object_panel_smoke`,
`new_listener_dialog_smoke`, `https_listener_smoke`,
`multi_terminal_smoke`, `listener_console_tab_smoke`,
`http_listener_smoke`, `database_export_clear_smoke`). Under option 1
those tests have to learn to open a menu and find an item; under option 2
they activate an action by name and stop caring what the UI looks like.
That is a smaller and much more durable change.

### Open questions

- Menu bar, or header bar with menu buttons? The latter is the current
  GNOME idiom but sits oddly with a tool that will also run under WSLg.
- Do the toggles stay as visible toggles somewhere as well? Losing the
  at-a-glance state of *Sidebar* / *Bottom Panel* may be a downgrade.
- Keyboard accelerators: worth doing at the same time, since actions make
  them nearly free.

## 2. Network and pivoting support

The theme: the app already watches listeners and connections. The gap is
everything a red teamer does *through* a foothold once they have one.

### The decision underneath all of it

Each item below can be built two ways, and it is worth picking a default
rather than deciding case by case:

- **Drive external tools.** Run `proxychains4`, `ssh -D`, `socat` in a
  managed terminal and track them as objects. Cheap, familiar to users,
  inherits their existing config — but the app is then only as reliable
  as its parsing of someone else's output.
- **Implement natively.** Own the SOCKS server, the port forward, the
  tunnel. More work, but it makes tunnels first-class objects with real
  state, and it keeps the dependency discipline the project has held so
  far.

A reasonable split: drive external tools first to learn what the workflow
wants, implement natively where the object model earns it.

### proxychains

- Generate a `proxychains.conf` from proxies the app already knows about,
  rather than making the user hand-maintain one.
- A per-run "route through…" choice on *Run in Terminal*, which already
  supports environment overrides and argument editing — this may be
  mostly a UI change over existing plumbing.
- A global default for new terminals, shown clearly, because a terminal
  that is silently proxied is a foot-gun.

### Tunnel mapping

- Model tunnels as a new object kind in `ObjectRegistry`, so they get a
  bottom-panel tab, state, and lifecycle for free.
- Local / remote / dynamic forwards, each showing its listening side, its
  destination, and whether traffic is actually flowing.
- A map or tree view: which host reaches which, through what. This is the
  part that is genuinely hard to hold in your head during an engagement,
  and the part a GUI can actually beat a terminal at.

### Other candidates

Unsorted, no commitment:

- Port forwarding over an established reverse connection (pivoting
  through a foothold rather than through SSH).
- A SOCKS5 proxy served over an existing connection.
- Host and route discovery, feeding the same map as tunnels.
- Credential and loot storage — `workbench.db` already exists and already
  has an export path.
- Listener templates and payload generation, so a listener and the thing
  that calls back to it are configured together.
- File transfer over a connection, reusing the explorer transfer code.
- Session notes and tagging, for the write-up afterwards.

### Open questions

- How much of this belongs in the app versus in `tools/` scripts with
  manifests? The manifest system was built precisely so tools could add
  their own panels — some of the above may not need app changes at all.
- Tunnels and proxies have credentials attached. `workbench.db` is
  currently a plain, exportable SQLite file, which is the right call for
  captured output and the wrong one for secrets.
- What happens to tunnels and proxies when the app exits? Leaving them
  running is sometimes what you want and sometimes a disaster.

## Parking lot

Ideas without a home yet:

- Windows support beyond WSLg.
- Multiple workspaces / projects, rather than one `files/` root.
- A session log that reads as a report, from the captured input/output.

## Constraints worth keeping

Carried over from 0.1.0, and worth arguing with explicitly rather than
drifting away from:

- **Dependencies stay deliberate.** GTK3, VTE, SQLite3 and OpenSSL are
  the whole list. JSON parsing and serialization are hand-written rather
  than vendored. Anything that adds a fifth should have to justify it.
- **Linux/POSIX, with WSLg as the Windows story.** The pty handling,
  file watching and terminal are all POSIX-specific by design.
- **Features arrive with tests.** The suite is the reason the UI fixes in
  0.1.0 could be verified rather than eyeballed, and several of those
  fixes were only distinguishable from each other by measurement.
- **Capture semantics are load-bearing.** Anything that changes how
  commands are run needs to keep the captured input and output meaning
  what they say — the prompt and the echo stay out of it.
