# ggui

`ggui` is a graph-first desktop interface for the
[`gg`](https://github.com/rokups/gg) change-oriented Git workflow. It uses
Dear ImGui, SDL3, OpenGL, libgg, and libgit2.

## Build

Install `gg` to a prefix, then configure ggui with that prefix:

```sh
cmake --preset linux-x64 -DCMAKE_PREFIX_PATH=/path/to/gg/prefix
cmake --build --preset linux-x64-debug
ctest --preset linux-x64-debug
```

For a Windows cross-build, install MinGW-w64 and a static Windows build of
`gg`, then use the `windows-x64` and `windows-x64-release` presets with that
`gg` install in `CMAKE_PREFIX_PATH`.

The application operates on one repository at a time. Repository mutations
are serialized on a worker thread and use libgg's public C API.

## Software-rendered tests and coverage

The coverage build embeds Dear ImGui Test Engine in the real executable. On
Linux, run the complete GoogleTest and UI workflow suite through Mesa
`llvmpipe` and Xvfb with:

```sh
GGUI_GG_PREFIX=/path/to/gg/prefix ./scripts/run_software_coverage.sh
```

The harness also exercises settings recovery and SDL/OpenGL startup failures,
then enforces 100% line coverage across `Application.cpp`, `Core.cpp`,
`Graph.cpp`, and `Main.cpp`. Exclusions are limited to marked native-dialog,
external-application, allocator/thread/bootstrap, and impossible dependency
failure paths; compiler-attributed brace-only lines are ignored.

## Workflow

Open, initialize, or clone a repository, then use the graph, changes, diff,
operation log, bookmarks, tags, workspaces, and sparse-checkout panels. The
Change menu exposes gg's local change operations. A newly opened plain Git
repository needs **New** before it has a gg working-copy change.

Left-drag a graph row onto the top of another row to reorder before it, onto
the middle to squash, or onto the bottom to rebase. Drop below the final row
to reorder after it. Right-drag anywhere onto a row to choose the action from
a popup menu. Every graph drop shows a confirmation preview and remains
undoable through gg's operation history.

Repository and libgit2 handles stay on one worker thread. The UI exchanges
typed commands and immutable snapshots with that worker. External Git and
working-tree changes are adopted automatically once per second and on window
focus. Clone credentials are requested on demand and are never persisted.

Graph rendering was adapted from the ImGit graph lane renderer; the
surrounding application architecture is new.
