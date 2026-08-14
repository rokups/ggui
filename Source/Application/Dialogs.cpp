// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace Ggui
{
using namespace ApplicationInternal;

void Application::OpenDialog(Dialog dialog)
{
    if (!_active_operation.empty() && dialog != Dialog::Credentials)
        return;
    _dialog = dialog;
    _input_primary.clear();
    _input_secondary.clear();
    _input_tertiary.clear();
    _input_filesets.clear();
    _input_flag = false;
    _input_flag_secondary = false;
    _input_flag_tertiary = false;
    _input_mode = 0;
    if (dialog == Dialog::Metaedit)
    {
        const auto selected = std::ranges::find_if(
            _snapshot->revisions, [this](const Revision& revision) { return revision.oid == _selected_revision; });
        if (selected != _snapshot->revisions.end())
        {
            _input_primary = selected->description;
            _input_secondary = selected->author;
            if (!selected->author_email.empty())
                _input_secondary += " <" + selected->author_email + ">";
        }
    }
    if (dialog == Dialog::Split || dialog == Dialog::Restore)
        _input_filesets = _selected_file;
    if (dialog == Dialog::Rebase && _snapshot != nullptr)
    {
        _input_secondary = CurrentCommit(*_snapshot);
        _dialog_snapshot_generation = _snapshot->generation;
    }
    if (dialog == Dialog::Credentials)
        _input_primary = _credential_request.username;
}

void Application::RenderDialogs()
{
    // Save patch dialog
    if (_open_save_patch)
    {
        ImGui::OpenPopup("Save Patch");
        _open_save_patch = false;
    }
    static char patch_save_path[512] = "patch.diff";
    if (ImGui::BeginPopupModal("Save Patch", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Save patch to file");
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(FontPx(420.0f));
        ImGui::InputText("Path", patch_save_path, IM_ARRAYSIZE(patch_save_path));
        if (ImGui::Button("Save"))
        {
            std::ofstream output(patch_save_path, std::ios::binary);
            output.write(_diff.patch.data(), static_cast<std::streamsize>(_diff.patch.size()));
            if (output)
            {
                _status_message = "Patch saved to " + std::string(patch_save_path);
                ImGui::CloseCurrentPopup();
            }
            else
                _error_message = "Could not save patch to " + std::string(patch_save_path);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Apply patch dialog
    if (_open_apply_patch)
    {
        ImGui::OpenPopup("Apply Patch");
        _open_apply_patch = false;
    }
    static char patch_apply_path[512]{};
    if (ImGui::BeginPopupModal("Apply Patch", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("Apply a patch to the current working copy. Invalid or conflicting patches are rejected.");
        if (ImGui::Button("Apply from Clipboard"))
        {
            const char* clipboard = ImGui::GetClipboardText();
            _engine.Enqueue(ApplyPatch{clipboard == nullptr ? "" : clipboard, {}});
            ImGui::CloseCurrentPopup();
        }
        ImGui::Spacing();
        ImGui::SetNextItemWidth(FontPx(420.0f));
        ImGui::InputText("File", patch_apply_path, IM_ARRAYSIZE(patch_apply_path));
        if (ImGui::Button("Apply from File"))
        {
            _engine.Enqueue(ApplyPatch{{}, patch_apply_path});
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (_dialog == Dialog::None)
        return;

    // Repository action dialog window
    constexpr std::array popup_titles{"Action###ggui action", "Clone repository###ggui action",
        "Commit change###ggui action", "Edit metadata###ggui action", "Rebase change###ggui action", "Squash changes###ggui action",
        "Split change###ggui action", "Abandon change###ggui action", "Restore files###ggui action",
        "Create bookmark###ggui action", "Rename bookmark###ggui action", "Create tag###ggui action", "Add remote###ggui action",
        "Add workspace###ggui action", "Rename workspace###ggui action", "Push bookmark###ggui action",
        "Reconcile bookmark###ggui action", "Credentials###ggui action",
        "Confirm operation###ggui action", "Locked commit warning###ggui action",
        "Force bookmark move###ggui action"};
    if (!ImGui::IsPopupOpen("ggui action"))
        ImGui::OpenPopup("ggui action");
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(560.0f, 0.0f), ImVec2(560.0f, std::numeric_limits<float>::max()));
    const auto close_dialog = [this]() {
        if (_dialog == Dialog::Credentials) _engine.CancelCredential();
        if (_dialog == Dialog::ConfirmLocked)
        {
            _pending_commands.clear();
            _pending_change_info_save = false;
        }
        _dialog = Dialog::None;
        ImGui::ClearActiveID();
    };
    bool open = true;
    if (!ImGui::BeginPopupModal(
            popup_titles[static_cast<std::size_t>(_dialog)], &open, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (!open) close_dialog();
        return;
    }
    const bool focus_first = ImGui::IsWindowAppearing();
    const float browse_width = ImGui::CalcTextSize("Browse").x + ImGui::GetStyle().FramePadding.x * 2.0f;

    // Action-specific form
    switch (_dialog)
    {
    case Dialog::Clone:
        ImGui::TextUnformatted("Clone repository");
        DialogInput("URL", "https://host/owner/repository.git", &_input_primary, focus_first);
        ImGui::TextUnformatted("Destination");
        ImGui::SetNextItemWidth(-browse_width - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("###Destination", "/path/to/repository", &_input_secondary);
        ImGui::SameLine();
        if (ImGui::Button("Browse")) _input_secondary = PickFolder();
        break;
    case Dialog::Commit:
        ImGui::TextUnformatted("Commit working change and create a new one");
        DialogMultiline("Description", &_input_primary, 90.0f, focus_first);
        DialogMultiline("Filesets", &_input_filesets, 70.0f);
        ImGui::TextDisabled("Leave empty to commit all changed files.");
        break;
    case Dialog::Metaedit:
        TextLabelledId("Edit metadata for ", _selected_revision, RevisionPrefix(_selected_revision),
            CommitIdColor(_selected_revision == _snapshot->working_copy));
        DialogMultiline("Description", &_input_primary, 100.0f, focus_first && !_input_flag);
        DialogInput("Author", "Name <email>", &_input_secondary, focus_first && _input_flag);
        break;
    case Dialog::Rebase:
    {
        TextLabelledId("Rebase @ ", _input_secondary, RevisionPrefix(_input_secondary),
            CommitIdColor(_input_secondary == _snapshot->working_copy));
        DialogInput("Destination", "commit ID or bookmark", &_input_primary, focus_first);
        const Revision* source = RebaseSource();
        if (source == nullptr)
            ImGui::TextDisabled("The @ change is unavailable.");
        else
        {
            const std::string first_line =
                source->description.empty() ? "(no description)" : FirstLine(source->description);
            const std::string description = LimitedFragment(first_line, 48);
            ImGui::TextDisabled("@ — %s", description.c_str());
        }
        ImGui::TextWrapped("The @ change and all of its descendants will be rebased onto the destination.");
        if (_snapshot->generation != _dialog_snapshot_generation || CurrentCommit(*_snapshot) != _input_secondary)
            ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.24f, 1.0f),
                "Repository changed. Close this dialog and inspect @ again.");
        break;
    }
    case Dialog::Squash:
        TextLabelledId("Squash ", _selected_revision, RevisionPrefix(_selected_revision),
            CommitIdColor(_selected_revision == _snapshot->working_copy));
        DialogInput("Into", "defaults to parent", &_input_secondary, focus_first);
        DialogMultiline("Combined description", &_input_primary, 90.0f);
        break;
    case Dialog::Split:
        TextLabelledId("Split ", _selected_revision, RevisionPrefix(_selected_revision),
            CommitIdColor(_selected_revision == _snapshot->working_copy));
        DialogMultiline("Selected filesets", &_input_filesets, 90.0f, focus_first);
        DialogInput("Selected description", "optional", &_input_primary);
        break;
    case Dialog::Restore:
        TextLabelledId("Restore into ", _selected_revision, RevisionPrefix(_selected_revision),
            CommitIdColor(_selected_revision == _snapshot->working_copy));
        DialogInput("From", "defaults to parent", &_input_primary, focus_first);
        DialogMultiline("Filesets", &_input_filesets, 90.0f);
        ImGui::TextDisabled("Leave empty to restore all files.");
        break;
    case Dialog::Abandon:
    {
        TextLabelledId("Abandon ", _selected_revision, RevisionPrefix(_selected_revision),
            CommitIdColor(_selected_revision == _snapshot->working_copy));
        ImGui::SameLine();
        ImGui::TextWrapped("and restack its descendants. This remains undoable.");
        if (ImGui::Checkbox("Also abandon all descendants (full branch)", &_input_flag_tertiary)
            && _input_flag_tertiary)
        {
            const std::vector<std::string> revisions = AbandonRevisions(_selected_revision, true);
            _input_flag = _input_flag || std::ranges::any_of(_snapshot->refs, [&](const NamedRef& ref) {
                return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK
                    && std::ranges::find(revisions, ref.target) != revisions.end();
            });
        }
        const std::vector<std::string> revisions =
            AbandonRevisions(_selected_revision, _input_flag_tertiary);
        if (_input_flag_tertiary)
            ImGui::TextDisabled("%zu changes will be abandoned.", revisions.size());
        ImGui::Checkbox("Retain bookmarks", &_input_flag);
        const std::vector<RemoteBookmarkDelete> remote_bookmarks = RemoteBookmarksAt(revisions);
        if (!remote_bookmarks.empty())
        {
            ImGui::Checkbox("Also delete bookmark from remote", &_input_flag_secondary);
            if (_input_flag_secondary)
                for (const RemoteBookmarkDelete& bookmark : remote_bookmarks)
                    ImGui::TextDisabled("%s/%s", bookmark.remote.c_str(), bookmark.bookmark.c_str());
        }
        break;
    }
    case Dialog::Bookmark:
        ImGui::TextUnformatted("Create bookmark");
        DialogInput("Name", "bookmark name", &_input_primary, focus_first);
        DialogInput("Revision", "defaults to selected change", &_input_secondary);
        break;
    case Dialog::BookmarkRename:
    {
        ImGui::Text("Rename bookmark %s", _input_secondary.c_str());
        DialogInput("New name", "bookmark name", &_input_primary, focus_first);
        const bool conflict = _snapshot != nullptr && std::ranges::any_of(_snapshot->refs, [this](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == _input_primary;
        });
        if (_input_primary == _input_secondary)
            ImGui::TextDisabled("Choose a different name.");
        else if (conflict)
            ImGui::TextDisabled("A local bookmark already uses this name.");
        break;
    }
    case Dialog::Tag:
        ImGui::TextUnformatted("Create or move tag");
        DialogInput("Name", "tag name", &_input_primary, focus_first);
        DialogInput("Revision", "defaults to selected change", &_input_secondary);
        ImGui::Checkbox("Allow move", &_input_flag);
        break;
    case Dialog::RemoteAdd:
        ImGui::TextUnformatted("Add remote");
        DialogInput("Name", "origin", &_input_primary, focus_first);
        DialogInput("URL", "https://host/owner/repository.git", &_input_secondary);
        break;
    case Dialog::WorkspaceAdd:
        ImGui::TextUnformatted("Add workspace");
        ImGui::TextUnformatted("Destination");
        if (focus_first)
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-browse_width - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("###Destination", "/path/to/workspace", &_input_primary);
        ImGui::SameLine();
        if (ImGui::Button("Browse")) _input_primary = PickFolder();
        DialogInput("Name", "derived from directory if empty", &_input_secondary);
        DialogInput("Revision", "defaults to @", &_input_tertiary);
        break;
    case Dialog::WorkspaceRename:
        ImGui::TextUnformatted("Rename current workspace");
        DialogInput("New name", "workspace name", &_input_primary, focus_first);
        break;
    case Dialog::PushTo:
        ImGui::Text("Push bookmark %s", _input_secondary.c_str());
        ImGui::TextUnformatted("Remote");
        if (focus_first)
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("###Remote", _input_primary.c_str()))
        {
            for (const Remote& remote : _snapshot->remotes)
                if (ImGui::Selectable(remote.name.c_str(), remote.name == _input_primary))
                    _input_primary = remote.name;
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Force push", &_input_flag);
        if (_input_flag)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.24f, 1.0f),
                "Warning: force push can overwrite remote history.");
            ImGui::TextWrapped("Remote commits that are not in the local bookmark may become unreachable.");
        }
        break;
    case Dialog::Reconcile:
        ImGui::Text("Reconcile bookmark %s", _input_primary.c_str());
        TextLabelledId("Local tip: ", _input_tertiary, RevisionPrefix(_input_tertiary),
            CommitIdColor(_input_tertiary == _snapshot->working_copy));
        TextLabelledId(("Remote tip (" + _input_secondary + "): ").c_str(), _input_filesets,
            RevisionPrefix(_input_filesets), CommitIdColor(_input_filesets == _snapshot->working_copy));
        ImGui::Spacing();
        ImGui::TextWrapped("gg will rebase the local-only branch onto the fetched remote tip and move the local "
                           "bookmark atomically. This rewrites local changes, may produce logical conflicts, and can "
                           "be undone.");
        if (_snapshot->generation != _dialog_snapshot_generation)
            ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.24f, 1.0f),
                "Repository changed. Close this dialog and inspect the updated bookmark tips.");
        break;
    case Dialog::Credentials:
        ImGui::TextWrapped("Credentials requested by %s", _credential_request.url.c_str());
        ImGui::TextUnformatted("Method");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Combo("###Method", &_input_mode, "Username / token\0SSH agent\0SSH key\0");
        DialogInput("Username", "username", &_input_primary, focus_first);
        if (_input_mode == 2)
        {
            DialogInput("Private key", "/path/to/private/key", &_input_secondary);
            DialogInput("Public key", "/path/to/public/key", &_input_tertiary);
        }
        if (_input_mode != 1)
            DialogInput("Token or passphrase", "secret", &_input_filesets, false, ImGuiInputTextFlags_Password);
        break;
    case Dialog::ConfirmDrop:
    {
        const char* action = _pending_drop.action == DropAction::Squash ? "Squash"
            : _pending_drop.action == DropAction::Rebase               ? "Rebase"
            : _pending_drop.action == DropAction::ReorderAfter         ? "Move after"
                                                                       : "Move before";
        TextLabelledId(std::string(action) + " ", _pending_drop.source, RevisionPrefix(_pending_drop.source),
            CommitIdColor(_pending_drop.source == _snapshot->working_copy));
        TextLabelledId("Target: ", _pending_drop.target, RevisionPrefix(_pending_drop.target),
            CommitIdColor(_pending_drop.target == _snapshot->working_copy));
        int affected = 0;
        for (const Revision& revision : _snapshot->revisions)
            affected += std::ranges::find(revision.parents, _pending_drop.source) != revision.parents.end();
        int refs = 0;
        for (const NamedRef& ref : _snapshot->refs)
            refs += ref.target == _pending_drop.source || ref.target == _pending_drop.target;
        ImGui::TextWrapped("gg will restack affected descendants and move associated refs atomically. Direct children: %d; refs on source/target: %d. Conflicts remain editable and this operation can be undone.",
            affected, refs);
        if (_pending_drop.action == DropAction::Rebase && CurrentCommit(*_snapshot) != _pending_drop.source)
            ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.24f, 1.0f),
                "The @ change moved. Close this dialog and inspect @ again.");
        break;
    }
    case Dialog::ConfirmLocked:
        ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.24f, 1.0f), "Warning: locked commit");
        ImGui::TextWrapped("%s", _locked_warning.c_str());
        ImGui::TextWrapped("Locked commits have already been pushed. Continuing can make local history diverge from the remote and require a force push.");
        break;
    case Dialog::ConfirmBookmarkMove:
        ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.24f, 1.0f), "Warning: non-forward bookmark move");
        ImGui::Text("Move bookmark %s", _input_primary.c_str());
        TextLabelledId("Current tip: ", _input_secondary, RevisionPrefix(_input_secondary),
            CommitIdColor(_input_secondary == _snapshot->working_copy));
        TextLabelledId("Target: ", _input_tertiary, RevisionPrefix(_input_tertiary),
            CommitIdColor(_input_tertiary == _snapshot->working_copy));
        ImGui::TextWrapped("This moves the bookmark backwards or sideways. Commits reachable only from its current "
                           "tip may become unreachable. The operation remains undoable.");
        if (_snapshot->generation != _dialog_snapshot_generation)
            ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.24f, 1.0f),
                "Repository changed. Close this dialog and inspect the bookmark again.");
        break;
    case Dialog::None: break; // GCOV_EXCL_LINE: RenderDialogs returns before switching on None
    }

    // Locked commit warning
    const bool modifies_locked = DialogModifiesLockedCommit();
    if (modifies_locked && _dialog != Dialog::ConfirmLocked)
    {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.24f, 1.0f), "Warning: this operation modifies a locked commit.");
        ImGui::TextWrapped("Locked commits have already been pushed. Continuing can make local history diverge from the remote.");
    }

    // Submit and cancel actions
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const bool can_submit = CanSubmitDialog();
    const bool operation_blocks_submit = !_active_operation.empty() && _dialog != Dialog::Credentials;
    const bool submit_shortcut = ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter);
    const bool cancel_shortcut = ImGui::IsKeyPressed(ImGuiKey_Escape);
    const bool focus_submit = _dialog == Dialog::ConfirmDrop || _dialog == Dialog::Reconcile;
    const bool focus_cancel = _dialog == Dialog::Abandon || _dialog == Dialog::ConfirmLocked
        || _dialog == Dialog::ConfirmBookmarkMove;
    if (focus_first && focus_submit)
        ImGui::SetKeyboardFocusHere();
    ImGui::BeginDisabled(operation_blocks_submit || !can_submit);
    const char* submit_label = _dialog == Dialog::PushTo ? "Push"
        : _dialog == Dialog::Reconcile ? "Reconcile"
        : _dialog == Dialog::ConfirmBookmarkMove ? "Force move"
        : _dialog == Dialog::ConfirmDrop || _dialog == Dialog::ConfirmLocked ? "Confirm"
                                                                            : "Apply";
    const bool dangerous_submit = modifies_locked || (_dialog == Dialog::PushTo && _input_flag)
        || _dialog == Dialog::ConfirmBookmarkMove;
    const bool submit = (dangerous_submit ? DangerButton(submit_label, ImVec2(110.0f, 0.0f))
                                          : ImGui::Button(submit_label, ImVec2(110.0f, 0.0f)))
        || (submit_shortcut && !operation_blocks_submit && can_submit);
    if (!can_submit && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Fill in the required fields before applying.");
    if (submit)
        SubmitDialog();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (focus_first && focus_cancel)
        ImGui::SetKeyboardFocusHere();
    const bool cancel = ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)) || cancel_shortcut;
    ImGui::SameLine();
    ImGui::TextDisabled("Ctrl+Enter apply | Esc cancel");
    if (cancel)
    {
        close_dialog();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Application::SubmitDialog()
{
    switch (_dialog)
    {
    case Dialog::Clone: _engine.Enqueue(CloneRepository{_input_primary, _input_secondary}); break;
    case Dialog::Commit: _engine.Enqueue(Commit{_input_primary, SplitLines(_input_filesets)}); break;
    case Dialog::Metaedit: _engine.Enqueue(Metaedit{_selected_revision, _input_primary, _input_secondary}); break;
    case Dialog::Rebase: _engine.Enqueue(Rebase{_input_secondary, _input_primary}); break;
    case Dialog::Squash: _engine.Enqueue(Squash{_selected_revision, _input_secondary, _input_primary}); break;
    case Dialog::Split: _engine.Enqueue(Split{_selected_revision, _input_primary, SplitLines(_input_filesets)}); break;
    case Dialog::Abandon:
    {
        const std::vector<std::string> revisions =
            AbandonRevisions(_selected_revision, _input_flag_tertiary);
        _engine.Enqueue(Abandon{revisions, _input_flag, false,
            _input_flag_secondary ? RemoteBookmarksAt(revisions)
                                  : std::vector<RemoteBookmarkDelete>{}});
        break;
    }
    case Dialog::Restore:
        _engine.Enqueue(Restore{_input_primary, _selected_revision, SplitLines(_input_filesets)});
        break;
    case Dialog::Bookmark:
        _engine.Enqueue(Bookmark{GG_BOOKMARK_CREATE, {_input_primary},
            _input_secondary.empty() ? _selected_revision : _input_secondary, {}});
        break;
    case Dialog::BookmarkRename:
        _engine.Enqueue(Bookmark{GG_BOOKMARK_RENAME, {_input_secondary}, {}, _input_primary});
        break;
    case Dialog::Tag:
        _engine.Enqueue(Tag{GG_TAG_SET, {_input_primary},
            _input_secondary.empty() ? _selected_revision : _input_secondary, _input_flag});
        break;
    case Dialog::RemoteAdd: _engine.Enqueue(AddRemote{_input_primary, _input_secondary}); break;
    case Dialog::WorkspaceAdd:
        _engine.Enqueue(WorkspaceAdd{_input_primary, _input_secondary,
            _input_tertiary.empty() ? "@" : _input_tertiary, {}});
        break;
    case Dialog::WorkspaceRename: _engine.Enqueue(WorkspaceRename{_input_primary}); break;
    case Dialog::PushTo: _engine.Enqueue(Push{_input_secondary, _input_primary, _input_flag}); break;
    case Dialog::Reconcile: _engine.Enqueue(Rebase{_input_tertiary, _input_filesets, true}); break;
    case Dialog::Credentials:
    {
        CredentialResponse response;
        response.method = static_cast<CredentialResponse::Method>(_input_mode);
        response.username = _input_primary;
        response.secret = _input_filesets;
        response.private_key = _input_secondary;
        response.public_key = _input_tertiary;
        _engine.SubmitCredential(std::move(response));
        std::fill(_input_filesets.begin(), _input_filesets.end(), '\0');
        break;
    }
    case Dialog::ConfirmDrop:
        if (_pending_drop.action == DropAction::Squash)
            _engine.Enqueue(Squash{_pending_drop.source, _pending_drop.target, {}});
        else if (_pending_drop.action == DropAction::Rebase)
            _engine.Enqueue(Rebase{_pending_drop.source, _pending_drop.target});
        else
            _engine.Enqueue(Reorder{
                _pending_drop.source, _pending_drop.target, DropPlacement(_pending_drop.action)});
        break;
    case Dialog::ConfirmLocked:
        for (Command& command : _pending_commands)
            _engine.Enqueue(std::move(command));
        _pending_commands.clear();
        if (_pending_change_info_save)
            _change_info_dirty = false;
        _pending_change_info_save = false;
        break;
    case Dialog::ConfirmBookmarkMove:
        _engine.Enqueue(Bookmark{GG_BOOKMARK_MOVE, {_input_primary}, _input_tertiary, {}, true});
        break;
    case Dialog::None: break; // GCOV_EXCL_LINE: no dialog can submit None
    }
    _dialog = Dialog::None;
    ImGui::ClearActiveID();
    ImGui::CloseCurrentPopup();
}

} // namespace Ggui
