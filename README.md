# toolbox

A GTK3 desktop toolbox for running and observing network listeners and
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
  and the response that follows it into `toolbox.db`.
- **File explorer & editor** — a sandboxed workspace root (`files/`,
  created next to the built binary) with create/rename/move/delete,
  drag-and-drop, live filesystem-change watching, and a built-in text
  editor with save/save-as/revert and unsaved-change confirmation.
- **Toolkit sidebar** — `toolkit/` (also created next to the binary) is
  indexed at startup; its top-level contents show up alongside the file
  explorer for quick access to scripts/tools.
- **Run in Terminal** — run a file (with optional arguments/environment
  overrides) in a fresh terminal tab or an already-open one; the command
  and its output are captured the same way typed commands are.

## Layout

```
toolbox/
├── CMakeLists.txt
├── src/
│   ├── main.c            # Entry point
│   ├── app/               # Application lifecycle (App create/run/destroy)
│   ├── core/               # Platform-agnostic domain logic: Workspace/Tab,
│   │                        # TerminalSession, LineAccumulator (input
│   │                        # capture), AnsiStripper (output capture)
│   ├── db/                 # SQLite persistence (schema, capture, export)
│   ├── files/               # Workspace root, file tree/ops/search/watch,
│   │                         # editor document model
│   ├── listeners/            # Listener/connection domain: TCP/HTTP/HTTPS
│   │                          # workers, ObjectRegistry, ConnectionManager,
│   │                          # TerminalHistory
│   ├── terminal/              # Terminal abstraction + PtyWorker (owns the
│   │                           # pty for local shell terminals)
│   ├── tools/                  # Built-in tool registry / toolkit/ indexer
│   └── ui/
│       ├── workbench.c          # Platform-neutral seam
│       └── gtk/                  # GTK+VTE backend 
├── tests/                 # ~49 unit/integration/GTK-driven smoke tests
└── toolkit/                # Auto-created next to the built binary; its
                             # top-level contents (not subfolders) are
                             # indexed at startup and shown in the sidebar
```

## Building

Requires GTK3, VTE 2.91, SQLite3, and OpenSSL development packages (used
for HTTPS listeners).

```sh
cmake -B build -S .
cmake --build build
./build/toolbox
```

## Testing

```sh
cmake --build build
ctest --test-dir build
```

## Running on Windows (Beta)

toolbox is a GTK3 + VTE Linux application and is not natively portable to
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
   ./build/toolbox
   ```

The window will appear on the Windows desktop via WSLg automatically —
no X server or extra display configuration needed.
