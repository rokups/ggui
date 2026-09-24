# Remaining end-to-end testing

The responsive history and inspection work has been exercised against the large Linux and rbfx repositories. The following broader mutation audit remains intentionally deferred:

- In an isolated Linux clone, exercise create/edit/describe/commit, undo/redo, and restore workflows.
- Exercise move/revert/delete/apply-patch operations, conflicts, and the external merge-tool flow.
- Exercise branch, tag, workspace, and offline remote mutations.
- Record large-Linux benchmarks for initial frame/page, selection latency, scrolling p95, RSS, and commit-graph maintenance.

Do not mutate `/home/rk/Desktop/src/projects/ggui/linux` or `/home/rk/Desktop/src/rbfx/rbfx`; use disposable clones. Filtered Xvfb UI tests run reliably with desktop/session environment variables unset. A blanket UI run can still be disrupted by the host GTK/NFD icon-loader sandbox, independently of ggui behavior.
