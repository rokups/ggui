# Graph action audit

Reviewed `ggui` graph actions and their sibling `gg` implementations on
2026-09-14. The identified history-scope, lock-warning, data-transfer and failure
recovery defects from the audited baseline are fixed. The original validation
is recorded below; subsequent upstream integration is recorded separately.

## Verification

- **203/203 backend checks passed**, including checkout recovery, recovery-marker
  cleanup, HEAD locks, root/no-workspace transitions, alias behavior and linked
  worktree first-import Undo/Redo, HEAD-lock preservation and repeated refresh
  after Undo/Forget.
- **50/50 engine tests passed against the final backend**, including
  file/line movement, metadata clearing, default restore source and snapshotting
  pending edits before Undo/Redo.
- **16 squash endpoint scenarios passed** exact tree, unaffected commit ID/ref,
  workspace and Undo/Redo assertions.
- **41/41 UI tests passed** in the final combined CTest run, using an isolated
  data directory and a 2560×1440 Xvfb display with software rendering. Coverage
  includes locked historical New, multi-parent creation, graph warnings,
  stale confirmations, menus, keyboard actions and diff context actions.
- Final logs: `gg/build/audit-tests.log` and
  `ggui/build/audit-final-tests.log`. Both projects build successfully and
  `git diff --check` passes.
- **Linked-worktree refresh after Undo: closed.** Bootstrap creation is limited
  to initial setup or retry of a failed initial HEAD transition. The passing
  same-instance C API regression repeats GUI adoption-plus-snapshot after Undo
  and Forget and checks unchanged operation identity and available Redo.

All builds and test runs were coordinated through one build owner. The action
matrix records the reviewed contracts and supporting tests; it does not claim
exhaustive enumeration of every possible graph and file-kind combination.

## Integration with newer upstream

Before publication, the audit changes were rebased onto `gg` commit `fbc9868`
and `ggui` commit `153212e`. These revisions introduced V4 operation storage,
direct aliases, selected-head history loading, more workspace controls and an
SDL_GPU renderer.

- Failed mutations restore the exact previous alias overlay, while normal
  Undo/Redo clears the overlay according to upstream semantics.
- Detached HEAD-only commits are retained by operation history so Undo/Redo
  still works after reflog expiry and Git garbage collection.
- Reorder copy keeps a distinct identity even when the new rewrite no-op
  shortcut would otherwise preserve the source object.
- Lock checks and confirmation targets use the separately loaded history.
- UI navigation fixtures exercise selected-head history with disposable Git
  repositories. Synthetic fixtures are ordered for graph layout, and test
  inspection skips collapsed-region rows.
- Upstream removed the right-drag action menu, final drop zone, graph bookmark
  move menu and full-description copy menu. Tests cover the retained row-edge
  reorder, modifier-based center drops, bookmark creation and commit-ID copy.
- Branch-abandon discovery now uses the core live graph, excluding retained
  operation metadata and superseded revisions. Loaded aliases also update
  selected revision IDs and dependent diff/comparison state.
- SDL_GPU option structures are zero-initialized for the GCC warning policy.
- The newer upstream descendant-squash mode still performs separate tip
  operations. It was introduced after the audited baseline and is unchanged
  by this integration; it does not have the single-operation guarantee of
  the audited squash modes.

Integrated validation: **236/236 backend checks**, **63/63 engine tests**,
**51/51 UI tests** and the **16 squash endpoint scenarios** passed. The final
combined engine/UI run used isolated data directories, a 2560×1440 Xvfb display
and Vulkan software rendering. Logs: `gg/build/push-tests.log` and
`ggui/build/push-final-tests.log`. Both repositories pass `git diff --check`.

## Reviewed invariants

Mutations affect the requested changes and the descendants that must be
restacked. Ancestors and sibling branches outside that set preserve commit IDs,
trees and refs. Unchanged pushed revisions remain locked. Read-only sources and
destinations do not trigger rewrite warnings; actual affected descendants are
included in lock checks. No-op rewrites preserve commit identities.

Confirmations bind to the reviewed repository state. Rejected operations
preserve refs, HEAD and working files, apart from working-copy synchronization
that precedes the command. Local operations remain undoable. Checkout collision
checks protect ignored files and files excluded from snapshots by size limits or
explicit untracking.

## Action matrix

Backend names are `gg_repository_*` C API suffixes. GUI dispatch is in
`Source/Core/RepositoryMutations.cpp`; backend implementations are primarily in
`gg/src/commands_change.cpp`, `commands_rewrite.cpp`, `commands_remote.cpp`,
`rewrite.cpp` and `working_copy.cpp`.

| Action and entry points | Backend | Reviewed behavior | Supporting coverage |
| --- | --- | --- | --- |
| New: toolbar, Ctrl+N, N, row menu; selected merge parents | `new_change` | Creates a child without abandoning empty or pushed parents. One GUI command and one logical undo step. | Workflow creation/insertion/merge tests; engine creation tests; real UI locked historical parent and multi-parent regressions. |
| Edit: E/menu | `edit` | Moves workspace to selected change; root editing retains identity through later snapshots. Dirty files are synchronized first. | Workflow edit/navigation; root identity, no-workspace and HEAD-lock regressions. |
| Previous/Next: menu/toolbar | `move` | Default navigation creates a working change at the selected adjacent revision; ambiguous/missing targets reject cleanly. | Workflow navigation tests, including `NavigationCreatesChangesUnlessEditIsRequested`. |
| Duplicate Change/Branch: D/Shift+D/menu | `duplicate` | Original graph and refs remain intact. Copies remap their selected parents and workspace only. | `DuplicatesABranchWithoutRewritingTheOriginal`, selected-only duplication test. |
| Commit; selected filesets | `commit` | Selected contents become committed change; unselected contents remain in a new working child; necessary descendants restack. | `commit_test` selection, description, validation and rename cases. |
| Save message; Edit author | `describe`, `metaedit` | Rewrites selected metadata and necessary descendants. Empty descriptions can be supplied independently of author-only edits. | Metadata/workflow tests; engine `MetadataCanClearDescriptionAndPreserveItForAuthorOnlyEdits`. |
| Rebase menu/context destination | `rebase` | Rebases @ and descendants; destination remains unchanged. Rebasing to current parent is a no-op. | Workflow rebase; unchanged-parent identity and attached/detached no-workspace regressions. |
| Alt drop; Alt+Shift branch drop; Reconcile | `rebase` | Single-source mode starts at source; branch mode resolves divergence root. Both restack required descendants. | Engine branch-divergence and reconciliation tests; backend scope regressions. |
| Reorder before/after: row edges, final drop zone, right-drag menu | `reorder` | Reorders the affected interval while retaining unchanged prefix IDs. Unsupported unrelated/merge/ambiguous stacks reject. | C API atomic/root reorder tests; `ReorderAfterTargetKeepsUnchangedPrefixIdentity`; graph UI warning and context-menu tests. |
| Squash dialog/center drop; Shift branch squash | `squash_ex` | Moves source changes into destination and restacks affected children. Branch mode consumes selected branch path and restacks side children. | Sibling/descendant targets, overlapping edits, logical conflicts, side children and 16-case exact-state matrix. |
| Split: Alt+S/menu | `split` | Selected and remaining changes conserve cumulative contents. Parents stay unchanged; explicit source works without a workspace. | Workflow/rewrite split tests; `SplitsAnExplicitRevisionWithoutAWorkspace`. |
| Abandon/branch: A/Shift+A/dialog | `abandon` | Removes selected changes and restacks descendants. Protects another active workspace's exact revision. Empty replacement gets a fresh identity. | Abandon/retain-bookmark/descendant tests; exact-workspace rejection and fresh replacement regressions. |
| Simplify parents | `simplify_parents` | Removes redundant edges while preserving trees; necessary descendants follow. Already-simple revisions remain unchanged; absent-workspace requests reject explicitly. | Dedicated `simplify_parents_test`. |
| Restore files/all | `restore` | Rewrites destination and descendants; source is read-only. Empty source means selected change's parent. | Dedicated restore tests; engine `RestoreWithoutSourceUsesSelectedChangesParent`. |
| File drag to graph; Move to parent/child | `move_files` | Transfers selected patch without losing destination edits or changing unrelated history. | C API remote-only/unreferenced endpoints and carrier validation; engine edited-child, rename and merge-child tests. |
| Move line/selection/hunk to parent/child | Atomic selected transfer API | Transfers only selected deltas; sibling branches do not receive copied changes. Stale contents are checked as well as coordinates. | Engine transfer/topology and `RejectsStaleLineSelectionsWhenNewContentUsesTheSameCoordinates`. |
| Revert file/hunk; Revert line | Working-copy helpers | Historical source stays unchanged; reverse patch applies to @. Other selected-file edits remain intact. | Engine file/hunk/line revert and original-position preservation tests. |
| Bookmark create/move/set/advance/rename/delete/forget; tags | `bookmark`, `tag` | Ref-only edits preserve checkout. Rename follows attached HEAD; deleting detaches it; moving its branch detaches at original commit. HEAD-lock failures restore operation/refs. | Remote/tag/API tests; attached bookmark and HEAD-lock regressions with status, disk/index and Undo/Redo assertions. |
| Undo/Redo; operation Restore | `undo`, `redo`, `restore_operation` | Synchronizes pending edits before applying operation state; restores graph/HEAD/worktree coherently and respects other workspaces. | Workflow operation/workspace tests; engine pending-edit regression; checkout/HEAD-lock recovery tests. |
| Fetch/Pull/Push graph updates | Remote APIs | Imported refs establish correct pushed ancestry. Unchanged local ancestor IDs and locked state survive rewrites. | Remote/engine tests and `KeepsUnchangedAncestorsLockedAcrossARewrite`. |

## Fixed findings

- Added the squash API used by the GUI, with arbitrary supported endpoints and
  branch-path semantics. Reduced unnecessary rewrites for no-op rebase,
  unchanged identities and reorder prefixes.
- Removed New's implicit empty-parent abandonment. Lock checks now distinguish
  rewritten roots/descendants from read-only inputs and share revision
  resolution. Rewrite confirmations bind to reviewed state.
- Fixed metadata clearing, default Restore source, and Undo/Redo snapshotting.
- Replaced broad descendant-tree restoration during partial transfers with
  atomic selected transfers. Preserved edited destinations, renames, merge
  resolutions and sibling-branch semantics; rejected stale selected contents.
- Fixed absent-workspace Split, protected-workspace Abandon, root edit/snapshot
  identity and no-workspace history/HEAD/checkout transitions. Linked-worktree
  first import now shares transactional snapshot recording; its initial Undo
  lineage and dirty-file preservation under HEAD.lock have passing regressions.
  Repeated refresh respects undone/forgotten workspaces and preserves Redo.
- Fixed attached bookmark HEAD handling for rename, delete, forget, move, set
  and advance, including HEAD-lock rejection and Undo/Redo.
- Added coherent operation/worktree recovery after checkout failure, including
  partially written files and originally unborn repositories. Checkout uses
  the actual tracked baseline so recovery avoids rewriting already-correct
  protected files. Physical ref updates skip unchanged values after alias
  translation, avoiding unnecessary ref locks.
- Limited attempted-checkout exemptions to immediate recovery. Terminal
  recovery failure clears the marker; a same-C-API-instance regression verifies
  a subsequent edit cannot overwrite newly untracked precious contents.
- Ordered local Abandon before optional remote bookmark deletion and added
  explicit reporting of partial remote failure.

## Review verdict and remaining limits

Independent review covered checkout preflight and actual-baseline construction,
recovery-marker lifetime, physical ref filtering, root/import transitions and
the no-workspace finish helper. The latest linked-worktree helper promotion is
consistent with transactional snapshot semantics. An additional refresh/adoption
gate and passing same-instance C API regression preserve redo lineage after Undo
and respect forgotten workspaces. No original audit finding remains open; the
original backend, engine and UI suites all pass. The later upstream integration
and its scope are described above.

If the filesystem prevents recovery itself, the error explicitly states that
recovery could not finish. Restored refs are not silently replaced with the
failed graph, and terminal cleanup prevents stale overwrite exemptions. An
arbitrary persistent filesystem failure cannot guarantee a restored worktree.

Remote server deletion is outside local Undo. Local abandonment precedes the
remote request, and partial remote failures are reported explicitly.
