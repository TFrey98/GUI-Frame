# workbench

A GTK3 desktop workbench for running and observing network listeners and
local terminals side by side: a tabbed workspace with an embedded shell
terminal, reverse-TCP/HTTP/HTTPS listeners with live per-connection
terminal views, a sandboxed file explorer and text editor, and a local
SQLite database that captures terminal activity for later export.

## Features

- **Listeners & connections** — start reverse-TCP, HTTP, or HTTPS
  listeners; each accepted connection gets its own live terminal view
  (single-writer, with a "Take Control" handoff between multiple open
  views of the same connection) and shows up in the object panel.
- **Local terminals** — tabbed shell terminals that own their own pty
  directly (fork/exec + a dedicated reader/writer thread per terminal,
  not libvte's built-in spawn), so their input and output are observable
  for capture, not just rendered.
- **Terminal capture** — every terminal (local and connection) records
  the final submitted line of each command (not individual keystrokes)
  and the response that follows it into `workbench.db`.
- **File explorer & editor** — a sandboxed workspace root (`files/`,
  created next to the built binary) with create/rename/move/delete,
  drag-and-drop, live filesystem-change watching, and a built-in text
  editor with save/save-as/revert and unsaved-change confirmation.
- **Tools sidebar** — `tools/` (also created next to the binary) is
  indexed at startup; its top-level contents show up in the explorer
  sidebar under **Tools**, alongside the **Files** workspace root, for
  quick access to runnable scripts.
- **Run in Terminal** — run a file (with optional arguments/environment
  overrides) in a fresh terminal tab or an already-open one; the command
  and its output are captured the same way typed commands are.

## Layout

```
workbench/
├── CMakeLists.txt
├── src/
│   ├── main.c            # Entry point
│   ├── app/               # Application lifecycle (App create/run/destroy)
│   ├── core/               # Platform-agnostic domain logic: Workspace/Tab,
│   │                        # TerminalSession, LineAccumulator (input
│   │                        # capture), AnsiStripper (output capture),
│   │                        # AppPaths (where writable state lives)
│   ├── db/                 # SQLite persistence (schema, capture, export)
│   ├── files/               # Workspace root, file tree/ops/search/watch,
│   │                         # editor document model
│   ├── listeners/            # Listener/connection domain: TCP/HTTP/HTTPS
│   │                          # workers, ObjectRegistry, ConnectionManager,
│   │                          # TerminalHistory
│   ├── terminal/              # Terminal abstraction + PtyWorker (owns the
│   │                           # pty for local shell terminals)
│   ├── tools/                  # Built-in tool registry / tools/ indexer
│   └── ui/
│       ├── workbench.c          # Platform-neutral seam
│       └── gtk/                  # GTK+VTE backend 
├── packaging/             # .desktop entry, icons, Debian maintainer scripts
├── package.sh             # one-command release build -> dist/*.deb
├── release/               # the published .deb testers install (tracked)
├── tests/                 # ~53 unit/integration/GTK-driven smoke tests
└── tools/                  # Auto-created next to the built binary; its
                             # top-level contents (not subfolders) are
                             # indexed at startup and shown in the sidebar
```

## Building

Requires GTK3, VTE 2.91, SQLite3, and OpenSSL development packages (used
for HTTPS listeners).

```sh
sudo apt install build-essential cmake pkg-config \
    libgtk-3-dev libvte-2.91-dev libsqlite3-dev libssl-dev
cmake -B build -S .
cmake --build build
./build/workbench
```

## Testing

```sh
cmake --build build
ctest --test-dir build
```

## Packaging a beta build

`package.sh` produces a single `.deb` to hand to testers. It builds into
`build-release/` so it never disturbs the incremental Debug tree in
`build/`, and turns the test suite off for the release build.

```sh
./package.sh                # -> dist/workbench_0.1.0~beta_amd64.deb
./package.sh 0.2.0          # version 0.2.0, still a ~beta package
./package.sh 1.0.0 ""       # a final (non-beta) 1.0.0 package
```

The package's `Depends` are computed by `dpkg-shlibdeps` from the linked
binary rather than hand-maintained, so adding a library to
`CMakeLists.txt` automatically shows up in the next package.

The `~beta` suffix sorts *before* the plain version under dpkg's version
ordering, so a tester running `0.1.0~beta` is correctly upgraded by a
later `0.1.0`.

Add `--publish` to also copy the package into `release/`, which is
tracked in git — that directory is how testers get it:

```sh
./package.sh --publish
git add -A release
git commit -m "Release 0.1.0~beta"
git push
```

`release/` holds exactly one `.deb`: `--publish` deletes any previous
package before copying the new one in, so the working tree never
accumulates stale versions and there is never a question about which file
to take. `dist/` stays gitignored — it is scratch build output, and
`release/` is the one published artifact.

### What testers do

Point them at [`release/`](release/), which has its own install guide.
They can either download the `.deb` directly from GitHub, or — if they
have the repo cloned — just pull:

```sh
git pull
sudo apt install ./release/workbench_0.1.0~beta_amd64.deb
```

`apt` pulls in GTK3, VTE, SQLite3, and OpenSSL automatically — nothing
else to install. **Workbench** then appears in the applications menu, or
runs as `workbench` from a shell.

Installing a newer package over an older one upgrades it in place. To
uninstall: `sudo apt remove workbench`.

## Where data is stored

`files/`, `tools/`, and `workbench.db` live next to the executable
whenever that directory is writable — so a development build keeps
everything in `build/`, exactly as before, and the test suite resolves
the same roots it always has.

An installed binary lives in a read-only `/usr/bin`, so it falls back to
the per-user XDG data directory instead, created on first launch:

```
~/.local/share/workbench/
├── files/          # shown as "Files" - the sandboxed editor workspace
├── tools/          # shown as "Tools" - indexed at startup, run in a terminal
└── workbench.db    # captured terminal activity
```

Set `XDG_DATA_HOME` to relocate that. `sudo apt remove workbench` leaves
it in place — testers keep their data across beta upgrades, and can wipe
it by deleting the directory. The resolution order lives in
`src/core/app_paths.c`.

## Running on Windows (Beta)

workbench is a GTK3 + VTE Linux application and is not natively portable to
Windows — the terminal widget (VTE), pty handling, and file-change
watching are all POSIX/Linux-specific. The supported way to run it on
Windows is under **WSL2 with WSLg**, which runs Linux GUI apps directly
on the Windows desktop with no code changes required.

1. Install WSL2 with a distro (Ubuntu is recommended and includes WSLg
   by default on Windows 11 / Windows 10 21H2+). From an elevated
   PowerShell:

   ```powershell
   wsl --install
   ```

   If WSL is already installed, make sure it's up to date:

   ```powershell
   wsl --update
   ```

2. Inside the WSL Ubuntu shell, install the build dependencies:

   ```sh
   sudo apt update
   sudo apt install build-essential cmake pkg-config \
       libgtk-3-dev libvte-2.91-dev libsqlite3-dev libssl-dev
   ```

3. Build and run exactly as on native Linux:

   ```sh
   cmake -B build -S .
   cmake --build build
   ./build/workbench
   ```

   Testers who were sent a `.deb` skip steps 2 and 3 and just install it
   inside the WSL shell — `apt` pulls the runtime libraries in:

   ```sh
   sudo apt install ./workbench_0.1.0~beta_amd64.deb
   workbench
   ```

The window will appear on the Windows desktop via WSLg automatically —
no X server or extra display configuration needed.
