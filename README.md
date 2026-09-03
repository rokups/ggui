# ggui

`ggui` is a graph-first desktop interface for the
[`gg`](https://github.com/rokups/gg) change-oriented Git workflow. It uses
Dear ImGui, SDL3, OpenGL, libgg, and libgit2.

Repository opening publishes refs before the working-tree scan completes and
builds History on a separate latest-wins worker. History first displays a
connected skeleton for the current workspace and selected heads, then replaces
it with up to 256 nearby commits. Ellipsis rows are real collapsed ancestry
regions: selecting one adds up to 128 nearby commits while preserving the
viewport anchor, and both the row and toolbar report that expansion is
loading. Very fast expansions leave a brief “Commits loaded” acknowledgement.
Merge commits show their first-parent history by default. Their graph dot has
a `+` control for revealing the merged branch and a `-` control for collapsing
it again; nested merges remain independently collapsed.
Collapsed regions never imply a relationship between unrelated
histories and never require a full-history count.

History search is debounced and runs in the repository worker. IDs, aliases,
and refs resolve directly; descriptions are scanned only when needed, with at
most 50 matches. Matches are highlighted and pinned into the connected graph,
so searching does not hide the commits and collapsed regions required to
explain their ancestry.

## Build

Install `gg` to a prefix, then configure ggui with that prefix:

```sh
cmake --preset linux-x64 -DCMAKE_PREFIX_PATH=/path/to/gg/prefix
cmake --build --preset linux-x64-debug
ctest --preset linux-x64-debug
```

For a Windows cross-build, install MinGW-w64 and a static Windows build of
`gg`, then use the `mingw-x64` and `mingw-x64-release` presets with that
`gg` install in `CMAKE_PREFIX_PATH`.

For a native Visual Studio 2022 x64 build, configure with `msvc-x64`, then
build with either `msvc-x64-debug` or `msvc-x64-release`.

The application operates on one repository at a time. Repository mutations
are serialized on a worker thread and use libgg's public C API.

## Source layout

- `Source/Application` contains the SDL/ImGui lifecycle, panels, dialogs,
  actions, presentation helpers, and UI-test adapters.
- `Source/Core` contains the repository model, typed commands and events, and
  the worker-side engine split by lifecycle, remote, snapshot, diff,
  working-copy, and mutation responsibilities.
- `Source/Graph` contains the independent history graph layout.
- `Source/Platform/Windows` contains native executable resources.
- `Assets` retains the source SVG artwork and preconverted platform formats,
  keeping platform builds independent of image-conversion tools.

## Software-rendered tests and coverage

The coverage build embeds Dear ImGui Test Engine in the real executable. On
Linux, run the complete GoogleTest and UI workflow suite through Mesa
`llvmpipe` and Xvfb with:

```sh
GGUI_GG_PREFIX=/path/to/gg/prefix ./scripts/run_software_coverage.sh
```

The harness also exercises settings recovery and SDL/OpenGL startup failures,
then enforces 100% line coverage across the production `.cpp` files under
`Source/Application`, `Source/Core`, `Source/Graph`, and `Source/Main.cpp`.
Exclusions are limited to marked native-dialog,
external-application, allocator/thread/bootstrap, and impossible dependency
failure paths; compiler-attributed brace-only lines are ignored.

## Workflow

Open, initialize, or clone a repository, then use the graph, changes, diff,
operation log, bookmarks, tags, workspaces, and sparse-checkout panels. The
Change menu exposes gg's local change operations. A newly opened plain Git
repository needs **New** before it has a gg working-copy change.

The **Compare with @** checkbox in Changes compares the selected change's
entire tree with the current working-copy snapshot (`@`); the matching checkbox
in Diff compares only the selected file while leaving the normal changed-file
list intact. Comparisons follow rewritten changes and the latest `@`, keep the
preferred file selected when possible, and support the same patch export and
external-diff actions as a normal change diff. File mutations and patch
application stay disabled until comparison mode is exited. The Diff context
selector includes **Full** for displaying both complete file versions.
Right-clicking a changed file can apply that change's inverse for the file to
the current working-copy change while preserving later edits where the patch
applies. Working-copy file menus can also delete the file from disk.
Conflicted files stay in the normal Changes list with a red **C**. Their diff
shows the stored conflict markers. Double-click a conflict, or use its context
menu, to open the configured Git merge tool with base, local, and remote
inputs. External Diff uses the same merge path for conflicted files instead of
showing all stored sides as one custom-marker file. When the tool closes, ggui
asks before it writes the result and marks the file resolved. Historical
conflicts are resolved in their selected graph
revision, with descendants restacked instead of copying over unrelated files.
Right-click a text diff to move the clicked line, the selected lines, or
the containing hunk to an adjacent parent or child change; unavailable targets
and unsupported diff contexts remain disabled. Unified and side-by-side views
both support character-level text selection and expose **Copy** in the diff
context menu. A hunk can be reverted from any selected
change onto `@`; files in the working-copy change additionally offer line-level
revert.

Use **F6** and **Shift+F6** to move to the next or previous changed file. The
navigation respects the active Changes filter and stops at the first and last
matching file. Repository, workspace, file, history, and bookmark context
menus provide portable open, copy, and rename actions. **Ctrl+W** closes the
current repository without changing it and returns to the recent-repository
welcome screen.

The Workspaces panel discovers both gg workspaces and native linked Git
worktrees. A single click leaves the current selection unchanged; double-click
an available non-current row to switch this window, or use its context menu to
open another ggui window. Opening a native linked worktree adopts it into gg.
Rows identify the current, primary, unmanaged, and stale states. Context actions
can reveal the working change, rename the selected managed workspace, and
safely remove a linked worktree or clean stale metadata after confirmation.
Removal snapshots recoverable tracked changes and refuses unsafe files, locks,
and conflicts; deleting the worktree directory itself is not undoable.

The Bookmarks panel controls which branches are visible in History: click to
add or remove a branch, or Ctrl-click to show only that branch. At least one
bookmark stays selected whenever bookmarks exist.
Local unnamed heads are always included, so switching the working-copy change
does not hide another unbookmarked line of work.
No tags are selected by default. Select tags in the Tags panel to ensure their
tagged history is found and revealed, or Ctrl-click to surface only one.
Tags and bookmarks on commits already present in History are always shown as
pills regardless of selection, with distinct local and remote colors. Distant tagged history stays behind collapsed
region rows, including the path that connects it back to selected bookmark
history.
Bookmark, tag, and remote selections are remembered per repository.
In History, **Up/Down** select the adjacent visible change and **N** creates a
new child of the selected change. Author email is available as a tooltip and
the author row's context menu supports copying or editing its identity.
History rows show each change's current Git commit ID. IDs retained from prior
rewrites remain searchable, keep selections attached across refreshes, and are
available from the change-information and copy menus as aliases.
Fetching updates remote bookmarks and shows their commits in History without
moving local bookmarks. Pulling additionally fast-forwards eligible tracked
local bookmarks, which can also be moved explicitly from a History row.

Left-drag a graph row onto the top of another row to reorder before it, onto
the middle to squash, or onto the bottom to rebase. Drop below the final row
to reorder after it. Right-drag anywhere onto a row to choose the action from
a popup menu. Every graph drop shows a confirmation preview and remains
undoable through gg's operation history.
Right-click **Rebase** to use that row as the destination for rebasing **@**
and all of its descendants.
When a tracked local and remote bookmark have both advanced, right-click the
bookmark and choose **Reconcile with remote/bookmark...** to rebase the local-only
branch onto the fetched remote tip. Reconciliation remains undoable and any
logical conflicts use the existing conflict workflow before the next push.

Repository and libgit2 handles stay on one worker thread. The UI exchanges
typed commands and immutable snapshots with that worker. External Git and
working-tree changes are adopted automatically from native filesystem
notifications. Clone credentials are requested on demand and are never persisted.

Common shortcuts: **Ctrl+O** open repository, **Ctrl+W** close repository,
**Ctrl+N** new change, **Ctrl+Z/Ctrl+Y** undo/redo, **F5** refresh,
**Shift+F6/F6** previous/next changed file, **Up/Down** previous/next history
item, **N** new child of the selected change.

Graph rendering was adapted from the ImGit graph lane renderer; the
surrounding application architecture is new.
