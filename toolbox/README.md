# toolbox

A GTK-based desktop toolbox application with a tabbed workspace, embedded
terminal (VTE), local SQLite-backed storage, and a pluggable tool registry.

## Layout

```
toolbox/
├── CMakeLists.txt
├── src/
│   ├── main.c              # Entry point
│   ├── app/                # Application lifecycle
│   ├── core/                # Workspace and tab domain logic
│   ├── ui/                  # Workbench (main window/layout)
│   ├── terminal/            # Terminal abstraction
│   ├── db/                  # SQLite persistence
│   ├── tools/                # Tool registry / plugin points
│   └── platform/linux/      # GTK + VTE platform backend
├── tests/
└── toolkit/                  # Auto-created next to the built binary; its
                               # top-level contents (not subfolders) are
                               # indexed at startup and shown in the sidebar
```

## Building

Requires GTK3, VTE 2.91, and SQLite3 development packages.

```sh
cmake -B build -S .
cmake --build build
./build/toolbox
```
