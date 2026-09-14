// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "Application/Application.hpp"
#include "Application/ApplicationInternal.hpp"

#include <IconsMaterialSymbols.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace Ggui
{
namespace
{

using namespace std::chrono_literals;

std::string Quote(const std::string& value)
{
    std::string result = "'";
    for (const char character : value)
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

class UiRepository
{
public:
    UiRepository()
    {
        static std::atomic_uint counter = 0;
        _path = std::filesystem::temp_directory_path() /
            ("ggui-ui-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(++counter));
        std::filesystem::create_directories(_path);
        Git("init");
        Git("config user.name 'ggui UI test'");
        Git("config user.email 'ggui-ui@example.test'");
        Write("tracked.txt", "base\n");
        Git("add tracked.txt");
        Git("commit -m base");
        Git("branch -M main");
    }

    ~UiRepository() { std::filesystem::remove_all(_path); }

    const std::filesystem::path& Path() const { return _path; }

    void Write(const std::string& relative, const std::string& contents) const
    {
        std::ofstream(_path / relative) << contents;
    }

    std::string RevisionId(const std::string& revision) const
    {
        git_repository* repository = nullptr;
        if (git_repository_open(&repository, _path.string().c_str()) != GIT_OK)
            throw std::runtime_error("could not open UI test repository");
        git_object* object = nullptr;
        const int result = git_revparse_single(&object, repository, revision.c_str());
        const std::string oid = result == GIT_OK ? git_oid_tostr_s(git_object_id(object)) : "";
        git_object_free(object);
        git_repository_free(repository);
        if (result != GIT_OK) throw std::runtime_error("could not resolve UI test revision");
        return oid;
    }

    void Git(const std::string& arguments)
    {
        const std::string command = "git -C " + Quote(_path.string()) + " " + arguments + " >/dev/null 2>&1";
        if (std::system(command.c_str()) != 0)
            throw std::runtime_error("could not prepare UI test repository");
    }

private:
    std::filesystem::path _path;
};

std::string NavigationCommit(const UiRepository& fixture, const std::string& description,
    const std::vector<std::string>& parents)
{
    const auto check = [](int result) {
        if (result != GIT_OK) throw std::runtime_error("could not create navigation test history");
    };
    git_repository* raw_repository = nullptr;
    check(git_repository_open(&raw_repository, fixture.Path().string().c_str()));
    std::unique_ptr<git_repository, decltype(&git_repository_free)> repository(raw_repository, git_repository_free);
    git_object* raw_head = nullptr;
    check(git_revparse_single(&raw_head, repository.get(), "HEAD^{tree}"));
    std::unique_ptr<git_object, decltype(&git_object_free)> tree(raw_head, git_object_free);
    git_signature* raw_signature = nullptr;
    check(git_signature_now(&raw_signature, "Navigation test", "navigation@example.test"));
    std::unique_ptr<git_signature, decltype(&git_signature_free)> signature(raw_signature, git_signature_free);
    std::vector<std::unique_ptr<git_commit, decltype(&git_commit_free)>> owned;
    std::vector<const git_commit*> parent_commits;
    for (const std::string& parent : parents)
    {
        git_oid oid{};
        check(git_oid_fromstr(&oid, parent.c_str(), git_repository_oid_type(repository.get())));
        git_commit* commit = nullptr;
        check(git_commit_lookup(&commit, repository.get(), &oid));
        owned.emplace_back(commit, git_commit_free);
        parent_commits.push_back(commit);
    }
    git_oid oid{};
    check(git_commit_create(&oid, repository.get(), nullptr, signature.get(), signature.get(), nullptr,
        description.c_str(), reinterpret_cast<git_tree*>(tree.get()), parent_commits.size(), parent_commits.data()));
    return git_oid_tostr_s(&oid);
}

template <class Predicate>
bool WaitNavigation(ImGuiTestContext* context, Predicate predicate)
{
    for (int attempt = 0; attempt < 1000; ++attempt)
    {
        context->Yield();
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

bool OpenNavigationRepository(ImGuiTestContext* context, const UiRepository& repository)
{
    Application& application = Application::Instance();
    application.OpenTestRepository(repository.Path().string());
    return WaitNavigation(context, [&] {
        const auto snapshot = application.SnapshotForTest();
        return snapshot != nullptr && std::filesystem::weakly_canonical(snapshot->root)
                == std::filesystem::weakly_canonical(repository.Path())
            && snapshot->worktree_state == RepoSnapshot::WorktreeState::Ready
            && application.ActiveOperationForTest().empty()
            && !application.HistoryLoadPendingForTest()
            && !application.VisibleHistoryRevisionsForTest().empty();
    });
}

UiRepository& Repository()
{
    static UiRepository repository;
    return repository;
}

ImGuiWindow* WaitForWindow(ImGuiTestContext* context, const char* name)
{
    ImGuiWindow* window = nullptr;
    for (int attempt = 0; attempt < 200 && (window == nullptr || !window->Active); ++attempt)
    {
        context->Yield();
        window = ImGui::FindWindowByName(name);
        if (window == nullptr || !window->Active)
            std::this_thread::sleep_for(5ms);
    }
    return window;
}

bool ActionDialogOpen()
{
    const ImGuiWindow* window = ImGui::FindWindowByName("ggui action");
    return window != nullptr && window->Active;
}

std::string CaptureRenderedText(ImGuiTestContext* context)
{
    struct Capture
    {
        std::string text;
        bool started = false;
        bool finished = false;
    } capture;
    ImGuiContextHook begin;
    begin.Type = ImGuiContextHookType_NewFramePost;
    begin.UserData = &capture;
    begin.Callback = [](ImGuiContext* gui, ImGuiContextHook* hook) {
        auto& state = *static_cast<Capture*>(hook->UserData);
        if (state.finished || gui->LogEnabled)
            return;
        ImGui::LogToBuffer(0);
        // Capture the complete application frame, across window boundaries.
        gui->LogWindow = nullptr;
        state.started = true;
    };
    ImGuiContextHook end;
    end.Type = ImGuiContextHookType_EndFramePre;
    end.UserData = &capture;
    end.Callback = [](ImGuiContext* gui, ImGuiContextHook* hook) {
        auto& state = *static_cast<Capture*>(hook->UserData);
        if (!state.started || state.finished)
            return;
        state.text = gui->LogBuffer.c_str();
        ImGui::LogFinish();
        state.finished = true;
    };
    const ImGuiID begin_id = ImGui::AddContextHook(GImGui, &begin);
    const ImGuiID end_id = ImGui::AddContextHook(GImGui, &end);
    for (int frame = 0; frame < 3 && !capture.finished; ++frame)
        context->Yield();
    ImGui::RemoveContextHook(GImGui, begin_id);
    ImGui::RemoveContextHook(GImGui, end_id);
    return capture.text;
}

bool RenderedTextContains(ImGuiTestContext* context, std::string_view text)
{
    return CaptureRenderedText(context).find(text) != std::string::npos;
}

bool WaitForItem(ImGuiTestContext* context, const char* window, const char* item)
{
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        context->Yield();
        context->SetRef(window);
        if (context->ItemExists(item))
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

bool WaitForStatus(ImGuiTestContext* context, const std::string& path, git_delta_t status)
{
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        context->Yield();
        const auto snapshot = Application::Instance().SnapshotForTest();
        if (snapshot != nullptr && std::ranges::any_of(snapshot->status, [&](const StatusEntry& entry) {
                return entry.path == path && entry.status == status;
            }))
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

bool OpenTestRepo(ImGuiTestContext* context)
{
    Application& application = Application::Instance();
    const std::filesystem::path expected = std::filesystem::weakly_canonical(Repository().Path());
    application.OpenTestRepository(expected.string());
    bool opened = false;
    for (int attempt = 0; attempt < 1000 && !opened; ++attempt)
    {
        context->Yield();
        const auto snapshot = application.SnapshotForTest();
        opened = snapshot != nullptr && !snapshot->root.empty()
            && !application.VisibleHistoryRevisionsForTest().empty()
            && application.ActiveOperationForTest().empty()
            && std::filesystem::weakly_canonical(snapshot->root) == expected;
        if (!opened) std::this_thread::sleep_for(5ms);
    }
    IM_CHECK_RETV(opened, false);
    ImGuiWindow* history = WaitForWindow(context, "History");
    IM_CHECK_RETV(history != nullptr, false);
    return history != nullptr;
}

void OpenAndCancel(ImGuiTestContext* context, const char* menu_path)
{
    context->MenuClick(menu_path);
    IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
    context->SetRef("ggui action");
    context->ItemClick("Cancel");
    context->Yield();
}

void FocusWindow(ImGuiTestContext* context, const char* name)
{
    const std::string path = std::string("//") + name;
    context->WindowFocus(path.c_str());
    context->SetRef(name);
    context->Yield();
}

void HoverRichSnapshotAuthor(ImGuiTestContext* context)
{
    FocusWindow(context, "Change information");
    // The parent buttons extend beyond the panel width. Visiting one can scroll
    // the metadata row horizontally and leave its leading author text clipped.
    context->ScrollToX("//Change information", 0.0f);
    context->ScrollToY("//Change information", 0.0f);
    context->Yield(2);
    ImGuiWindow* information = ImGui::FindWindowByName("Change information");
    const ImVec2 author = information->DC.CursorStartPos
        + ImVec2(4.0f, information->FontRefSize * 0.5f);
    IM_CHECK(information->InnerClipRect.Contains(author));
    context->MouseMoveToPos(author);
    context->Yield(2);
}

RepoSnapshot RichSnapshot()
{
    RepoSnapshot snapshot;
    snapshot.generation = 1000;
    snapshot.root = Repository().Path().string();
    snapshot.working_copy = "merge";
    snapshot.head = "left";
    snapshot.revisions = {
        {"merge", {"left", "right", "third"}, {"change-merge"}, "Merge subject\nbody", "Merger", 5, true, true, false},
        {"left", {"base"}, {"change-left"}, "Left", "Left Author", 4, false, false, false},
        {"right", {"base"}, {"change-right"}, "Right", "Right Author", 3, false, false, true},
        {"third", {"base"}, {"change-third"}, "", "Third Author", 2, false, false, false, true},
        {"base", {}, {"change-base"}, "Base", "Base Author", 1, false, false, true},
    };
    snapshot.revisions.front().author_email = "merger@example.test";
    snapshot.refs = {
        {"coverage-bookmark", {}, "merge", GG_NAMED_REF_LOCAL_BOOKMARK, true, true},
        {"feature", {}, "left", GG_NAMED_REF_LOCAL_BOOKMARK, false, false},
        {"coverage-tag", {}, "left", GG_NAMED_REF_LOCAL_TAG, false, false},
        {"coverage-tag", "origin", "left", GG_NAMED_REF_REMOTE_TAG, true, false},
        {"remote-tag", "upstream", "base", GG_NAMED_REF_REMOTE_TAG, true, false},
        {"remote-bookmark", {}, "right", GG_NAMED_REF_LOCAL_BOOKMARK, true, false},
        {"remote-bookmark", "origin", "right", GG_NAMED_REF_REMOTE_BOOKMARK, true, false},
        {"remote-only", "upstream", "base", GG_NAMED_REF_REMOTE_BOOKMARK, true, false},
        {"diverged", {}, "left", GG_NAMED_REF_LOCAL_BOOKMARK, true, false},
        {"diverged", "origin", "right", GG_NAMED_REF_REMOTE_BOOKMARK, true, false},
    };
    snapshot.status = {
        {{}, "added.txt", GIT_DELTA_ADDED, false},
        {"deleted.txt", "deleted.txt", GIT_DELTA_DELETED, false},
        {"modified.txt", "modified.txt", GIT_DELTA_MODIFIED, false},
        {"old.txt", "renamed.txt", GIT_DELTA_RENAMED, false},
        {"copied-from.txt", "copied.txt", GIT_DELTA_COPIED, false},
        {"type.txt", "type.txt", GIT_DELTA_TYPECHANGE, false},
        {{}, "untracked.txt", GIT_DELTA_UNTRACKED, false},
        {{}, "ignored.txt", GIT_DELTA_IGNORED, false},
        {"conflict.txt", "conflict.txt", GIT_DELTA_CONFLICTED, true},
        {{}, "unchanged.txt", GIT_DELTA_UNMODIFIED, false},
    };
    snapshot.operations = {{"operation-id", "Undoable operation", 1}};
    snapshot.workspaces = {
        {"current", snapshot.root, "merge", false, true, true, false},
        {"stale", "/missing/workspace", "left", true}};
    snapshot.remotes = {{"origin", "https://example.test/repository.git", "ssh://example.test/repository.git"}};
    snapshot.conflicts = {{"conflict file.txt", 2, 3}};
    snapshot.can_undo = true;
    snapshot.can_redo = true;
    return snapshot;
}

void ApplyOpenDialog(ImGuiTestContext* context, const char* menu_path)
{
    context->MenuClick(menu_path);
    IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
    context->SetRef("ggui action");
}

std::vector<ImGuiID> GatherItems(ImGuiTestContext* context, const char* parent, std::string_view label)
{
    ImGuiTestItemList items;
    context->GatherItems(&items, parent);
    std::vector<ImGuiID> result;
    for (int index = 0; index < items.GetSize(); ++index)
    {
        const ImGuiTestItemInfo* item = items.GetByIndex(index);
        if (item != nullptr && item->DebugLabel == label)
            result.push_back(item->ID);
    }
    return result;
}

ImGuiID RecentRepositoryItem(ImGuiTestContext* context, const std::string& path)
{
    // Visible repository labels can exceed the engine's 31-character debug
    // label. Address the stable ID used by RenderRecentRepositories instead.
    const ImGuiWindow* window = context->GetWindowByRef(context->GetRef());
    return ImHashStr("###repository", 0, ImHashStr(path.c_str(), 0, window->ID));
}

} // namespace

void RegisterUiTests(ImGuiTestEngine* engine)
{
    ImGuiTest* test = IM_REGISTER_TEST(engine, "Application", "Welcome");
    test->TestFunc = [](ImGuiTestContext* context) {
        ImGuiWindow* welcome = WaitForWindow(context, "Welcome");
        IM_CHECK_NE(welcome, nullptr);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        IM_CHECK_EQ(welcome->Pos.x, viewport->WorkPos.x);
        IM_CHECK_EQ(welcome->Pos.y, viewport->WorkPos.y);
        IM_CHECK_EQ(welcome->Size.x, viewport->WorkSize.x);
        IM_CHECK_EQ(welcome->Size.y, viewport->WorkSize.y);
        IM_CHECK_EQ(welcome->DockNode, nullptr);
        IM_CHECK_EQ(ImGui::FindWindowByName("ggui dockspace"), nullptr);
        context->SetRef("Welcome");
        IM_CHECK(context->ItemExists("Open repository..."));
        IM_CHECK(context->ItemExists("Initialize repository..."));
        IM_CHECK(context->ItemExists("Clone repository..."));
        IM_CHECK(Application::Instance().UsesSdrSwapchainForTest());
        std::array<unsigned int, 4> pixels{};
        IM_CHECK(Application::Instance().CaptureFramebufferForTest(
            0, 0, 2, 2, pixels.data()));
    };

    test = IM_REGISTER_TEST(engine, "Application", "RestoresWindowOnScreen");
    test->TestFunc = [](ImGuiTestContext*) {
        const std::array displays{SDL_Rect{0, 0, 1600, 1000}, SDL_Rect{1600, 0, 1920, 1080}};
        const SDL_Rect resized = ApplicationInternal::FitWindowToDisplays({2500, 100, 2400, 1200}, displays);
        IM_CHECK_EQ(resized.x, 1600);
        IM_CHECK_EQ(resized.y, 0);
        IM_CHECK_EQ(resized.w, 1920);
        IM_CHECK_EQ(resized.h, 1080);

        const std::array single_display{SDL_Rect{0, 0, 1600, 1000}};
        const SDL_Rect recovered = ApplicationInternal::FitWindowToDisplays({2400, 1400, 1440, 900}, single_display);
        IM_CHECK_EQ(recovered.x, 160);
        IM_CHECK_EQ(recovered.y, 100);
        IM_CHECK_EQ(recovered.w, 1440);
        IM_CHECK_EQ(recovered.h, 900);
    };

    test = IM_REGISTER_TEST(engine, "Application", "OpenRepositoryAndPanels");
    test->TestFunc = [](ImGuiTestContext* context) {
        if (!OpenTestRepo(context)) return;
        for (const char* panel : {"History", "Changes", "Change information", "Diff"})
        {
            ImGuiWindow* window = WaitForWindow(context, panel);
            IM_CHECK_NE(window, nullptr);
            IM_CHECK(window->Active);
        }
        ImGuiWindow* operations = ImGui::FindWindowByName("Operations");
        IM_CHECK(operations == nullptr || !operations->Active);
        ImGuiWindow* bookmarks = WaitForWindow(context, "Bookmarks");
        IM_CHECK_NE(bookmarks, nullptr);
        ImGuiWindow* tags = WaitForWindow(context, "Tags");
        ImGuiWindow* workspaces = WaitForWindow(context, "Workspaces");
        ImGuiWindow* remotes = WaitForWindow(context, "Remotes");
        IM_CHECK_NE(tags, nullptr);
        IM_CHECK_NE(workspaces, nullptr);
        IM_CHECK_NE(remotes, nullptr);
        IM_CHECK_EQ(tags->DockNode, bookmarks->DockNode);
        IM_CHECK_EQ(remotes->DockNode, workspaces->DockNode);
        IM_CHECK_NE(workspaces->DockNode, bookmarks->DockNode);
        ImGuiWindow* changes = ImGui::FindWindowByName("Changes");
        ImGuiWindow* change_information = ImGui::FindWindowByName("Change information");
        ImGuiWindow* diff = ImGui::FindWindowByName("Diff");
        IM_CHECK_NE(changes->DockNode, diff->DockNode);
        IM_CHECK_NE(change_information->DockNode, changes->DockNode);
        IM_CHECK_EQ(change_information->DockNode->ParentNode, changes->DockNode->ParentNode);
        const float information_ratio = change_information->Size.y / ImGui::GetMainViewport()->WorkSize.y;
        IM_CHECK_GT(information_ratio, 0.14f);
        IM_CHECK_LT(information_ratio, 0.24f);
        IM_CHECK_EQ(ImGui::FindWindowByName("Navigator"), nullptr);
    };

    test = IM_REGISTER_TEST(engine, "Application", "RepositorySwitchFeedback");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        struct ResetSwitchState
        {
            Application& application;
            ~ResetSwitchState()
            {
                application.ApplyEventForTest(OperationFinished{"open"});
                application.ClearSnapshotForTest();
            }
        } reset{application};
        application.SetSnapshotForTest(RichSnapshot());
        application.AddRecentForTest("/tmp/ggui-switch-target");
        context->Yield(2);
        context->SetRef("ggui dockspace");
        context->ItemClick("Repository");
        context->Yield();
        context->SetRef("//$FOCUSED");
        context->ItemClick(RecentRepositoryItem(context, "/tmp/ggui-switch-target"));
        context->Yield(2);
        // Synthetic snapshots suppress engine commands; EnqueueAction releases
        // the immediate action lock when the command is rejected.
        IM_CHECK_NE(application.SnapshotForTest(), nullptr);
        IM_CHECK(application.ActiveOperationForTest().empty());
        for (int attempt = 0; attempt < 5 && !GImGui->OpenPopupStack.empty(); ++attempt)
            context->KeyPress(ImGuiKey_Escape);

        application.ApplyEventForTest(OperationStarted{"open"});
        context->Yield(2);
        IM_CHECK_EQ(application.SnapshotForTest(), nullptr);
        ImGuiWindow* welcome = ImGui::FindWindowByName("Welcome");
        IM_CHECK(welcome != nullptr && welcome->Active);
        IM_CHECK_EQ(application.ActiveOperationForTest(), "open");
        FocusWindow(context, "Welcome");
        IM_CHECK((context->ItemInfo("Open repository...").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("Cancel").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        application.ApplyEventForTest(OperationFinished{"open"});
        application.ClearSnapshotForTest();
        context->Yield(2);
        IM_CHECK(application.ActiveOperationForTest().empty());
    };

    test = IM_REGISTER_TEST(engine, "Workflow", "CreateEditAndInspectWorkingChange");
    test->TestFunc = [](ImGuiTestContext* context) {
        if (!OpenTestRepo(context)) return;
        const std::string previous_working_copy = Application::Instance().SnapshotForTest()->working_copy;
        context->SetRef("ggui dockspace");
        context->ItemClick("New");
        for (int attempt = 0; attempt < 1000
            && Application::Instance().SnapshotForTest()->working_copy == previous_working_copy; ++attempt)
        {
            context->Yield();
            std::this_thread::sleep_for(5ms);
        }
        IM_CHECK_NE(Application::Instance().SnapshotForTest()->working_copy, previous_working_copy);
        const std::string empty_working_copy = Application::Instance().SnapshotForTest()->working_copy;
        std::optional<Revision> empty_change;
        for (int attempt = 0; attempt < 1000 && !empty_change.has_value(); ++attempt)
        {
            context->Yield();
            const auto& revisions = Application::Instance().HistoryRevisionsForTest();
            const auto found = std::ranges::find(revisions, empty_working_copy, &Revision::oid);
            if (found != revisions.end()) empty_change = *found;
            else std::this_thread::sleep_for(5ms);
        }
        IM_CHECK(empty_change.has_value());
        IM_CHECK(empty_change->empty);
        const std::size_t revision_count = Application::Instance().HistoryRevisionsForTest().size();
        IM_CHECK(Application::Instance().ActiveOperationForTest().empty());
        IM_CHECK_EQ(Application::Instance().SelectedRevisionsForTest(),
            std::vector<std::string>{empty_working_copy});
        Application::Instance().CreateChangeForTest("@");
        for (int attempt = 0; attempt < 1000
            && Application::Instance().SnapshotForTest()->working_copy == empty_working_copy; ++attempt)
        {
            context->Yield();
            std::this_thread::sleep_for(5ms);
        }
        const auto next = Application::Instance().SnapshotForTest();
        IM_CHECK_NE(next->working_copy, empty_working_copy);
        std::optional<Revision> next_change;
        for (int attempt = 0; attempt < 1000 && !next_change.has_value(); ++attempt)
        {
            context->Yield();
            const auto& revisions = Application::Instance().HistoryRevisionsForTest();
            const auto found = std::ranges::find(revisions, next->working_copy, &Revision::oid);
            if (found != revisions.end()) next_change = *found;
            else std::this_thread::sleep_for(5ms);
        }
        IM_CHECK(next_change.has_value());
        IM_CHECK(next_change->empty);
        IM_CHECK_EQ(next_change->parents, std::vector<std::string>{empty_working_copy});
        IM_CHECK_EQ(Application::Instance().HistoryRevisionsForTest().size(), revision_count + 1);
        IM_CHECK(std::ranges::any_of(Application::Instance().HistoryRevisionsForTest(),
            [&](const Revision& revision) { return revision.oid == empty_working_copy; }));

        Repository().Write("tracked.txt", "changed\n");
        Application::Instance().RefreshForTest();
        IM_CHECK(WaitForStatus(context, "tracked.txt", GIT_DELTA_MODIFIED));
        IM_CHECK(WaitForItem(context, "Changes", "**/M  tracked.txt"));
        context->SetRef("Changes");
        context->ItemClick("**/M  tracked.txt");
        context->Yield(4);
        IM_CHECK_NE(WaitForWindow(context, "Diff"), nullptr);
    };

    test = IM_REGISTER_TEST(engine, "Application", "MenusAndDialogs");
    test->TestFunc = [](ImGuiTestContext* context) {
        if (!OpenTestRepo(context)) return;
        OpenAndCancel(context, "//##MainMenuBar/Repository/Clone...");
        context->MenuClick("//##MainMenuBar/Edit/Apply patch...");
        IM_CHECK_NE(WaitForWindow(context, "Apply Patch"), nullptr);
        context->SetRef("Apply Patch");
        IM_CHECK(context->ItemExists("Apply from Clipboard"));
        IM_CHECK(context->ItemExists("Apply from File"));
        context->ItemClick("Cancel");
        for (const char* action : {"Commit...", "Rebase...", "Squash...",
                 "Split...", "Restore...", "Abandon..."})
        {
            const std::string path = std::string("//##MainMenuBar/Change/") + action;
            OpenAndCancel(context, path.c_str());
        }

        for (const char* panel : {"Bookmarks", "Tags", "Workspaces", "Remotes", "History", "Changes",
                 "Change information", "Diff"})
        {
            const std::string path = std::string("//##MainMenuBar/View/") + panel;
            context->MenuClick(path.c_str());
            context->Yield();
            ImGuiWindow* hidden = ImGui::FindWindowByName(panel);
            IM_CHECK(hidden == nullptr || !hidden->Active);
            context->MenuClick(path.c_str());
            IM_CHECK_NE(WaitForWindow(context, panel), nullptr);
        }
        context->MenuClick("//##MainMenuBar/View/Operations");
        IM_CHECK_NE(WaitForWindow(context, "Operations"), nullptr);
        context->MenuClick("//##MainMenuBar/View/Operations");
        context->Yield();
        IM_CHECK(!ImGui::FindWindowByName("Operations")->Active);
        context->MenuClick("//##MainMenuBar/View/Reset layout");
        context->MenuClick("//##MainMenuBar/Repository/Refresh");
        context->Yield(3);

        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/Create bookmark");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");
        FocusWindow(context, "Tags");
        context->ItemClick("**/Create tag");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");
        FocusWindow(context, "Workspaces");
        context->ItemClick("**/Add workspace");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");
        context->Yield();
    };

    test = IM_REGISTER_TEST(engine, "Application", "SettingsWindow");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.ClearSnapshotForTest();
        context->Yield(2);
        context->MenuClick("//##MainMenuBar/Repository/Settings...");
        IM_CHECK_NE(WaitForWindow(context, "Settings"), nullptr);
        context->SetRef("Settings");
        IM_CHECK(context->ItemExists("**/User"));
        IM_CHECK(context->ItemExists("**/##Maximum new file size User"));
        IM_CHECK(context->ItemExists("**/##GUI editor User"));
        IM_CHECK(!context->ItemExists("**/##Maximum new file size Repository"));
        IM_CHECK((context->ItemInfo("**/Repository").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("**/Workspace").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK(RenderedTextContains(context, "Open a repository to configure Repository and Workspace overrides."));

        const int original_scale = static_cast<int>(std::lround(ImGui::GetStyle().FontSizeBase / 16.0f * 100.0f));
        context->ItemInputValue("UI scale", 125);
        context->Yield();
        IM_CHECK_LE(std::fabs(ImGui::GetStyle().FontSizeBase - 20.0f), 0.001f);
        IM_CHECK_LE(std::fabs(ImGui::GetStyle().FontScaleMain - 1.0f), 0.001f);
        IM_CHECK_LE(std::fabs(ImGui::GetFontBaked()->Size - ImGui::GetFontSize()), 0.001f);
        context->ItemInputValue("UI scale", original_scale);
        context->ItemInputValue("**/##Maximum new file size User", "invalid");
        context->Yield();
        IM_CHECK(RenderedTextContains(context, "Enter unsigned bytes or a binary size such as 1MiB."));
        FocusWindow(context, "Settings");
        context->WindowClose("//Settings");
        context->Yield();
        IM_CHECK(!ImGui::FindWindowByName("Settings")->Active);

        if (!OpenTestRepo(context)) return;
        WriteMaxNewFileSizeValue(Repository().Path(), ConfigScope::Repository, std::nullopt);
        application.RefreshForTest();
        context->Yield(3);
        const std::uint64_t generation = application.SnapshotForTest()->generation;
        context->MenuClick("//##MainMenuBar/Repository/Settings...");
        IM_CHECK_NE(WaitForWindow(context, "Settings"), nullptr);
        context->SetRef("Settings");
        IM_CHECK(context->ItemExists("**/##Maximum new file size User"));
        context->ItemClick("**/Repository");
        context->Yield();
        IM_CHECK(context->ItemExists("**/##Maximum new file size Repository"));
        IM_CHECK(context->ItemExists("**/##GUI editor Repository"));
        context->ItemInputValue("**/##Maximum new file size Repository", "2MiB");
        context->ItemInputValue("**/##GUI editor Repository", "code --wait");
        context->ItemClick("**/Workspace");
        for (int frame = 0; frame < 100 && application.SnapshotForTest()->generation <= generation; ++frame)
            context->Yield();
        IM_CHECK_GT(application.SnapshotForTest()->generation, generation);
        const MaxNewFileSizeValues written = ReadMaxNewFileSizeValues(Repository().Path());
        IM_CHECK_EQ(written[1], "2MiB");
        IM_CHECK_EQ(ReadEditorValues(Repository().Path())[1], "code --wait");

        context->SetRef("Settings");
        context->ItemClick("**/Repository");
        context->Yield();
        context->ItemInputValue("**/##Maximum new file size Repository", "");
        context->ItemInputValue("**/##GUI editor Repository", "");
        context->ItemClick("**/User");
        context->Yield(2);
        IM_CHECK(!ReadMaxNewFileSizeValues(Repository().Path())[1].has_value());
        IM_CHECK(!ReadEditorValues(Repository().Path())[1].has_value());
        FocusWindow(context, "Settings");
        context->WindowClose("//Settings");
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "ChangedFileDoubleClickOpensEditor");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        Repository().Write("modified.txt", "working copy\n");
        application.SetSnapshotForTest(RichSnapshot());
        context->Yield(3);

        FocusWindow(context, "Changes");
        context->ItemDoubleClick("**/M  modified.txt");
        const std::filesystem::path working_path = application.OpenedEditorPathForTest();
        IM_CHECK_EQ(std::filesystem::weakly_canonical(working_path),
            std::filesystem::weakly_canonical(Repository().Path() / "modified.txt"));

        RepoSnapshot changed_snapshot = RichSnapshot();
        changed_snapshot.revisions.front().parents = {"left"};
        application.SetSnapshotForTest(std::move(changed_snapshot));
        application.SelectRevisionForTest("left");
        application.ApplyEventForTest(DiffReady{{1000, "left", "modified.txt", "old\n", "historical\n", false,
            RichSnapshot().status}});
        context->Yield(3);
        FocusWindow(context, "Changes");
        context->ItemDoubleClick("**/M  modified.txt");
        IM_CHECK_EQ(application.PendingEditorRevisionForTest(), "left");
        application.ApplyEventForTest(FileContentReady{"left", "modified.txt", "historical\n"});
        const std::filesystem::path historical_path = application.OpenedEditorPathForTest();
        IM_CHECK_NE(historical_path, working_path);
        std::ifstream input(historical_path, std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        IM_CHECK_EQ(contents, "historical\n");

        RepoSnapshot unchanged_snapshot = RichSnapshot();
        unchanged_snapshot.revisions.front().parents = {"left"};
        std::erase_if(unchanged_snapshot.status, [](const StatusEntry& file) {
            return file.path == "modified.txt";
        });
        application.SetSnapshotForTest(std::move(unchanged_snapshot));
        application.SelectRevisionForTest("left");
        application.ApplyEventForTest(DiffReady{{1000, "left", "modified.txt", "old\n", "historical\n", false,
            RichSnapshot().status}});
        context->Yield(3);
        FocusWindow(context, "Changes");
        context->ItemDoubleClick("**/M  modified.txt");
        IM_CHECK(application.PendingEditorRevisionForTest().empty());
        IM_CHECK_EQ(std::filesystem::weakly_canonical(application.OpenedEditorPathForTest()),
            std::filesystem::weakly_canonical(working_path));
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "ConflictFileUsesMergeResolution");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        context->Yield(3);

        FocusWindow(context, "Changes");
        context->ItemInputValue("##changes filter", "conflict.txt");
        context->Yield(2);
        IM_CHECK(context->ItemExists("**/C  conflict.txt"));
        context->ItemClick("**/C  conflict.txt", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK(context->ItemExists("**/Resolve with merge tool"));
        IM_CHECK(context->ItemExists("**/Mark current file resolved"));
        context->KeyPress(ImGuiKey_Escape);
        context->ItemDoubleClick("**/C  conflict.txt");
        IM_CHECK_NE(WaitForWindow(context, "Resolve conflict"), nullptr);
        context->SetRef("Resolve conflict");
        IM_CHECK(context->ItemExists("Mark resolved"));
        IM_CHECK(context->ItemExists("Keep conflict"));
        context->ItemClick("Keep conflict");

        application.SetSnapshotForTest(RichSnapshot());
        application.SelectRevisionForTest("left");
        application.ApplyEventForTest(
            DiffReady{{1000, "left", "conflict.txt", "base\n", "<<<<<<< Conflict\n", false,
                RichSnapshot().status}});
        context->Yield(3);
        FocusWindow(context, "Changes");
        context->ItemClick("**/C  conflict.txt", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/External diff");
        context->ItemClick("**/vs @");
        IM_CHECK_NE(WaitForWindow(context, "Resolve conflict"), nullptr);
        context->SetRef("Resolve conflict");
        context->ItemClick("Keep conflict");

        FocusWindow(context, "Changes");
        context->ItemDoubleClick("**/C  conflict.txt");
        IM_CHECK_NE(WaitForWindow(context, "Resolve conflict"), nullptr);
        context->SetRef("Resolve conflict");
        context->ItemClick("Keep conflict");
        FocusWindow(context, "Changes");
        context->ItemInputValue("##changes filter", "");
    };

    test = IM_REGISTER_TEST(engine, "Application", "OperationAvailability");
    test->TestFunc = [](ImGuiTestContext* context) {
        RepoSnapshot snapshot = RichSnapshot();
        snapshot.can_undo = false;
        snapshot.can_redo = false;
        Application::Instance().SetSnapshotForTest(std::move(snapshot));
        context->Yield(2);
        context->SetRef("ggui dockspace");
        IM_CHECK((context->ItemInfo("Undo").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("Redo").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        for (const char* action : {"Pull", "Fetch", "Push", "Push to..."})
            IM_CHECK(!context->ItemExists(action));

        snapshot = RichSnapshot();
        snapshot.can_undo = true;
        snapshot.can_redo = false;
        snapshot.remotes.clear();
        Application::Instance().SetSnapshotForTest(std::move(snapshot));
        context->Yield(2);
        context->SetRef("ggui dockspace");
        IM_CHECK((context->ItemInfo("Undo").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK((context->ItemInfo("Redo").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        for (const char* action : {"Pull", "Fetch", "Push", "Push to..."})
            IM_CHECK(!context->ItemExists(action));
    };

    test = IM_REGISTER_TEST(engine, "Application", "ActionsLockDuringOperation");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        struct FinishOperation
        {
            Application& application;
            ~FinishOperation() { application.ApplyEventForTest(OperationFinished{"locked operation"}); }
        } finish{application};
        application.SetSnapshotForTest(RichSnapshot());
        application.ApplyEventForTest(OperationStarted{"locked operation"});
        context->Yield(3);

        context->SetRef("ggui dockspace");
        for (const char* action : {"New", "Commit", "Prev", "Next", "Undo", "Redo", "Refresh"})
        {
            IM_CHECK(context->ItemExists(action));
            IM_CHECK((context->ItemInfo(action).ItemFlags & ImGuiItemFlags_Disabled) != 0);
        }
        IM_CHECK((context->ItemInfo("Cancel").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK_GE(context->ItemInfo("Cancel").RectFull.GetHeight(),
            context->ItemInfo("Refresh").RectFull.GetHeight() - 1.0f);
        IM_CHECK(!context->ItemExists("Open repository folder"));
        IM_CHECK(context->ItemExists("Repository activity"));
        IM_CHECK_GT(context->ItemInfo("Cancel").RectFull.Min.x,
            context->ItemInfo("Repository activity").RectFull.Max.x);

        FocusWindow(context, "Changes");
        context->ItemClick("**/M  modified.txt", ImGuiMouseButton_Right);
        context->Yield();
        for (const char* action : {"Move to parent", "Move to child", "Commit only this file",
                 "Revert", "Track", "Untrack"})
        {
            const std::string item = std::string("**/") + action;
            IM_CHECK(!context->ItemExists(item.c_str())
                || (context->ItemInfo(item.c_str()).ItemFlags & ImGuiItemFlags_Disabled) != 0);
        }
        context->KeyPress(ImGuiKey_Escape);

        FocusWindow(context, "Bookmarks");
        IM_CHECK((context->ItemInfo("**/Create bookmark").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("##bookmark filter").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        context->ItemClick("**/feature");
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"merge"});
        context->ItemClick("**/feature", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK((context->ItemInfo("**/Reveal commit").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK((context->ItemInfo("**/Push").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("**/Delete").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->KeyPress(ImGuiKey_Escape);

        FocusWindow(context, "Tags");
        IM_CHECK((context->ItemInfo("**/Create tag").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("##tag filter").ItemFlags & ImGuiItemFlags_Disabled) == 0);

        FocusWindow(context, "Workspaces");
        IM_CHECK((context->ItemInfo("**/Add workspace").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->ItemClick("**/current", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK((context->ItemInfo("**/Open directory").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK((context->ItemInfo("**/Remove...").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->KeyPress(ImGuiKey_Escape);

        FocusWindow(context, "Remotes");
        IM_CHECK((context->ItemInfo("**/Add remote").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->ItemClick("**/origin", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK((context->ItemInfo("**/Pull").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("**/Fetch").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("**/Delete remote").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->KeyPress(ImGuiKey_Escape);

        application.ApplyEventForTest(CredentialRequest{
            "https://example.test/repository", "suggested", GIT_CREDENTIAL_USERPASS_PLAINTEXT});
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemInputValue("Token or passphrase", "secret");
        context->Yield();
        IM_CHECK((context->ItemInfo("Apply").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        context->ItemClick("Cancel");

        application.ApplyEventForTest(OperationFinished{"locked operation"});
        context->Yield(2);
        context->SetRef("ggui dockspace");
        IM_CHECK((context->ItemInfo("Commit").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        FocusWindow(context, "Bookmarks");
        IM_CHECK((context->ItemInfo("**/Create bookmark").ItemFlags & ImGuiItemFlags_Disabled) == 0);
    };

    test = IM_REGISTER_TEST(engine, "Application", "ShowsConcurrentBackgroundActivity");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.ApplyEventForTest(OperationStarted{"close"});
        application.ApplyEventForTest(OperationFinished{"close"});
        application.SetSnapshotForTest(RichSnapshot());
        application.ApplyEventForTest(BackgroundActivityStarted{41, "Scanning working copy"});
        application.ApplyEventForTest(BackgroundActivityStarted{42, "Refreshing repository metadata"});
        context->Yield(3);

        context->SetRef("ggui dockspace");
        IM_CHECK(context->ItemExists("Repository activity"));
        IM_CHECK(context->ItemExists("**/Refresh"));
        IM_CHECK_EQ(context->ItemInfo("Repository activity").RectFull.Min.y,
            context->ItemInfo("**/Refresh").RectFull.Min.y);
        context->MouseMove("Repository activity");
        context->Yield(2);
        const ImGuiWindow* tooltip = GImGui->TooltipPreviousWindow;
        IM_CHECK(tooltip != nullptr && (tooltip->Active || tooltip->WasActive));

        application.ApplyEventForTest(BackgroundActivityFinished{41});
        application.ApplyEventForTest(OperationStarted{"fetch"});
        context->Yield(2);
        context->SetRef("ggui dockspace");
        IM_CHECK(context->ItemExists("Repository activity"));
        IM_CHECK(context->ItemExists("**/Refresh"));
        IM_CHECK_EQ(context->ItemInfo("Repository activity").RectFull.GetHeight(), ImGui::GetFrameHeight());
        context->MouseMove("Repository activity");
        context->Yield(2);
        IM_CHECK(GImGui->TooltipPreviousWindow != nullptr);
        application.ApplyEventForTest(OperationFinished{"fetch"});
        application.ApplyEventForTest(BackgroundActivityFinished{42});
        context->Yield(2);
    };

    test = IM_REGISTER_TEST(engine, "Navigation", "VisibleBookmarkBranches");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        UiRepository repository;
        const std::string base = repository.RevisionId("HEAD");
        const std::string main_tip = NavigationCommit(repository, "Main tip", {base});
        const std::string main_child = NavigationCommit(repository, "Main child", {main_tip});
        const std::string feature_tip = NavigationCommit(repository, "Feature tip", {base});
        const std::string feature_child = NavigationCommit(repository, "Feature child", {feature_tip});
        const std::string other_tip = NavigationCommit(repository, "Other bookmarked branch", {main_tip});
        repository.Git("update-ref refs/heads/main " + main_tip);
        repository.Git("update-ref refs/heads/feature " + feature_tip);
        repository.Git("update-ref refs/heads/other " + other_tip);
        repository.Git("update-ref refs/gg/visible-heads/" + feature_child + " " + feature_child);
        repository.Git("checkout --detach " + main_tip);
        repository.Git("update-ref refs/gg/workspaces/default " + main_child);
        IM_CHECK(OpenNavigationRepository(context, repository));
        const auto wait_for_history = [&](std::vector<std::string> expected) {
            std::ranges::sort(expected);
            return WaitNavigation(context, [&] {
                auto actual = application.VisibleHistoryRevisionsForTest();
                std::ranges::sort(actual);
                return !application.HistoryLoadPendingForTest() && actual == expected;
            });
        };
        IM_CHECK_EQ(application.VisibleBookmarksForTest(), std::vector<std::string>{"main"});
        IM_CHECK(wait_for_history({base, main_tip, main_child}));
        FocusWindow(context, "Bookmarks");
        ImGuiWindow* bookmarks = ImGui::FindWindowByName("Bookmarks");
        IM_CHECK_NE(bookmarks, nullptr);
        const auto list = std::ranges::find_if(bookmarks->DC.ChildWindows, [](const ImGuiWindow* child) {
            return std::string_view(child->Name).find("bookmark list") != std::string_view::npos;
        });
        IM_CHECK(list != bookmarks->DC.ChildWindows.end());
        IM_CHECK_LE(context->ItemInfo("##bookmark filter").RectFull.Max.y, (*list)->Pos.y);
        context->ItemClick("**/feature");
        IM_CHECK_EQ(application.VisibleBookmarksForTest(), (std::vector<std::string>{"main", "feature"}));
        IM_CHECK(wait_for_history({base, main_tip, main_child, feature_tip, feature_child}));
        context->ItemClick("**/main");
        IM_CHECK_EQ(application.VisibleBookmarksForTest(), std::vector<std::string>{"feature"});
        // Working-copy ancestry remains visible independently of selected bookmarks.
        IM_CHECK(wait_for_history({base, main_tip, main_child, feature_tip, feature_child}));
        context->ItemClick("**/feature");
        IM_CHECK_EQ(application.VisibleBookmarksForTest(), std::vector<std::string>{"feature"});
        context->ItemClick("**/main");
        context->KeyDown(ImGuiMod_Ctrl);
        context->ItemClick("**/feature");
        context->KeyUp(ImGuiMod_Ctrl);
        IM_CHECK_EQ(application.VisibleBookmarksForTest(), std::vector<std::string>{"feature"});
        application.ClearSnapshotForTest();
    };

    test = IM_REGISTER_TEST(engine, "Navigation", "RemoteAndBookmarkSelection");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        UiRepository repository;
        const std::string base = repository.RevisionId("HEAD");
        const std::string origin_tip = NavigationCommit(repository, "Origin tip", {base});
        const std::string upstream_tip = NavigationCommit(repository, "Upstream tip", {base});
        const std::string merge = NavigationCommit(repository, "Merge", {origin_tip, upstream_tip});
        repository.Git("branch -m main local");
        repository.Git("remote add upstream " + Quote(repository.Path().string()));
        repository.Git("remote add origin " + Quote(repository.Path().string()));
        repository.Git("update-ref refs/remotes/origin/origin-only " + origin_tip);
        repository.Git("update-ref refs/remotes/upstream/upstream-only " + upstream_tip);
        repository.Git("checkout --detach " + origin_tip);
        repository.Git("update-ref refs/gg/workspaces/default " + merge);
        IM_CHECK(OpenNavigationRepository(context, repository));
        IM_CHECK_EQ(application.SelectedRemotesForTest(), std::vector<std::string>{"origin"});
        IM_CHECK_EQ(application.VisibleBookmarksForTest(), std::vector<std::string>{"origin-only"});
        IM_CHECK(WaitNavigation(context, [&] {
            const auto visible = application.VisibleHistoryRevisionsForTest();
            return std::ranges::find(visible, merge) != visible.end()
                && std::ranges::find(visible, origin_tip) != visible.end();
        }));
        FocusWindow(context, "Bookmarks");
        IM_CHECK(context->ItemExists("**/local"));
        IM_CHECK(context->ItemExists("**/origin-only"));
        IM_CHECK(!context->ItemExists("**/upstream-only"));
        FocusWindow(context, "Remotes");
        context->ItemClick("**/origin");
        IM_CHECK_EQ(application.SelectedRemotesForTest(), std::vector<std::string>{"origin"});
        context->ItemClick("**/upstream");
        IM_CHECK_EQ(application.SelectedRemotesForTest(), (std::vector<std::string>{"origin", "upstream"}));
        context->KeyDown(ImGuiMod_Ctrl);
        context->ItemClick("**/upstream");
        context->KeyUp(ImGuiMod_Ctrl);
        IM_CHECK_EQ(application.SelectedRemotesForTest(), std::vector<std::string>{"upstream"});
        context->ItemClick("**/upstream");
        IM_CHECK_EQ(application.SelectedRemotesForTest(), std::vector<std::string>{"upstream"});
        IM_CHECK_EQ(application.VisibleBookmarksForTest(), std::vector<std::string>{"upstream-only"});
        IM_CHECK(WaitNavigation(context, [&] {
            const auto visible = application.VisibleHistoryRevisionsForTest();
            return !application.HistoryLoadPendingForTest()
                && std::ranges::find(visible, merge) != visible.end()
                && std::ranges::find(visible, upstream_tip) != visible.end()
                && std::ranges::find(visible, origin_tip) != visible.end();
        }));
        FocusWindow(context, "Bookmarks");
        IM_CHECK(context->ItemExists("**/local"));
        IM_CHECK(!context->ItemExists("**/origin-only"));
        IM_CHECK(context->ItemExists("**/upstream-only"));
        UiRepository other;
        IM_CHECK(OpenNavigationRepository(context, other));
        IM_CHECK(OpenNavigationRepository(context, repository));
        IM_CHECK_EQ(application.SelectedRemotesForTest(), std::vector<std::string>{"upstream"});
        IM_CHECK_EQ(application.VisibleBookmarksForTest(), std::vector<std::string>{"upstream-only"});
        UiRepository no_origin;
        no_origin.Git("remote add first " + Quote(no_origin.Path().string()));
        no_origin.Git("remote add second " + Quote(no_origin.Path().string()));
        IM_CHECK(OpenNavigationRepository(context, no_origin));
        IM_CHECK_EQ(application.SelectedRemotesForTest(), std::vector<std::string>{"first"});
        application.ClearSnapshotForTest();
    };

    test = IM_REGISTER_TEST(engine, "Navigation", "TagSelection");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        UiRepository repository;
        std::vector<std::string> revisions(600);
        revisions.back() = repository.RevisionId("HEAD");
        for (int index = 598; index >= 0; --index)
            revisions[index] = NavigationCommit(repository, "Revision " + std::to_string(index), {revisions[index + 1]});
        const std::string tagged_base = NavigationCommit(repository, "Tagged branch base", {});
        const std::string tagged_tip = NavigationCommit(repository, "Tagged branch tip", {tagged_base});
        repository.Git("update-ref refs/heads/main " + revisions[0]);
        repository.Git("tag first-tag " + revisions[500]);
        repository.Git("tag second-tag " + tagged_tip);
        repository.Git("checkout --detach " + revisions[1]);
        repository.Git("update-ref refs/gg/workspaces/default " + revisions[0]);
        IM_CHECK(OpenNavigationRepository(context, repository));
        const auto wait_for_visibility = [&](const std::string& oid, bool expected) {
            return WaitNavigation(context, [&] {
                const auto visible = application.VisibleHistoryRevisionsForTest();
                return !application.HistoryLoadPendingForTest()
                    && (std::ranges::find(visible, oid) != visible.end()) == expected;
            });
        };
        IM_CHECK(application.SelectedTagsForTest().empty());
        IM_CHECK(wait_for_visibility(revisions[500], false));
        IM_CHECK(wait_for_visibility(tagged_tip, false));
        FocusWindow(context, "Tags");
        context->ItemClick("**/first-tag");
        IM_CHECK_EQ(application.SelectedTagsForTest(), std::vector<std::string>{"first-tag"});
        IM_CHECK(wait_for_visibility(revisions[500], true));
        context->ItemClick("**/second-tag");
        IM_CHECK_EQ(application.SelectedTagsForTest(), (std::vector<std::string>{"first-tag", "second-tag"}));
        IM_CHECK(wait_for_visibility(tagged_tip, true));
        context->KeyDown(ImGuiMod_Ctrl);
        context->ItemClick("**/second-tag");
        context->KeyUp(ImGuiMod_Ctrl);
        IM_CHECK_EQ(application.SelectedTagsForTest(), std::vector<std::string>{"second-tag"});
        IM_CHECK(wait_for_visibility(revisions[500], false));
        UiRepository other;
        IM_CHECK(OpenNavigationRepository(context, other));
        IM_CHECK(application.SelectedTagsForTest().empty());
        IM_CHECK(OpenNavigationRepository(context, repository));
        IM_CHECK_EQ(application.SelectedTagsForTest(), std::vector<std::string>{"second-tag"});
        FocusWindow(context, "Tags");
        context->ItemClick("**/second-tag");
        IM_CHECK(application.SelectedTagsForTest().empty());
        // Selecting a tag also starts an independent reveal-by-ID search.
        // Clear that search before testing whether the tag remains a head.
        application.CancelHistorySearchForTest();
        IM_CHECK(wait_for_visibility(tagged_tip, false));

        // Selecting a tag on a merge's side branch materializes that branch.
        UiRepository joining;
        const std::string base = joining.RevisionId("HEAD");
        const std::string main = NavigationCommit(joining, "Main parent", {base});
        const std::string tag_parent = NavigationCommit(joining, "Tag parent", {base});
        const std::string tag_tip = NavigationCommit(joining, "Tag tip", {tag_parent});
        const std::string tip = NavigationCommit(joining, "Tip", {main, tag_tip});
        joining.Git("update-ref refs/heads/main " + tip);
        joining.Git("tag joining-tag " + tag_tip);
        joining.Git("checkout --detach " + main);
        joining.Git("update-ref refs/gg/workspaces/default " + tip);
        IM_CHECK(OpenNavigationRepository(context, joining));
        FocusWindow(context, "Tags");
        context->ItemClick("**/joining-tag");
        IM_CHECK(WaitNavigation(context, [&] {
            const auto visible = application.VisibleHistoryRevisionsForTest();
            return !application.HistoryLoadPendingForTest()
                && std::ranges::find(visible, tag_tip) != visible.end()
                && std::ranges::find(visible, base) != visible.end();
        }));
        application.ClearSnapshotForTest();
    };

    test = IM_REGISTER_TEST(engine, "Navigation", "RevealCommit");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        UiRepository repository;
        std::vector<std::string> revisions(600);
        revisions.back() = repository.RevisionId("HEAD");
        for (int index = 598; index >= 0; --index)
            revisions[index] = NavigationCommit(repository, "Revision " + std::to_string(index), {revisions[index + 1]});
        repository.Git("update-ref refs/heads/main " + revisions[0]);
        repository.Git("update-ref refs/heads/deep-bookmark " + revisions[550]);
        repository.Git("tag deep-tag " + revisions[500]);
        repository.Git("checkout --detach " + revisions[1]);
        repository.Git("update-ref refs/gg/workspaces/default " + revisions[0]);
        IM_CHECK(OpenNavigationRepository(context, repository));
        FocusWindow(context, "History");
        context->ItemInputValue("##graph filter", "does-not-match");
        context->Yield(3);
        // Search highlights matches while retaining the bounded graph context.
        IM_CHECK(!application.VisibleHistoryRevisionsForTest().empty());
        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/deep-bookmark", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Reveal commit");
        IM_CHECK(WaitNavigation(context, [&] {
            return application.SelectedRevisionsForTest() == std::vector<std::string>{revisions[550]}
                && !application.HistoryLoadPendingForTest();
        }));
        const auto visible_after_reveal = application.VisibleHistoryRevisionsForTest();
        IM_CHECK(std::ranges::find(visible_after_reveal, revisions[550]) != visible_after_reveal.end());
        IM_CHECK_LT(visible_after_reveal.size(), revisions.size());
        FocusWindow(context, "Tags");
        context->ItemClick("**/deep-tag", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Reveal commit");
        IM_CHECK(WaitNavigation(context, [&] {
            return application.SelectedRevisionsForTest() == std::vector<std::string>{revisions[500]}
                && !application.HistoryLoadPendingForTest();
        }));
        ImGuiWindow* history = ImGui::FindWindowByName("History");
        IM_CHECK_NE(history, nullptr);
        const auto stable_graph = std::ranges::find_if(history->DC.ChildWindows, [](const ImGuiWindow* child) {
            return std::string_view(child->Name).find("graph scroll") != std::string_view::npos;
        });
        IM_CHECK(stable_graph != history->DC.ChildWindows.end());
        const ImRect stable_graph_rect = (*stable_graph)->Rect();

        RepoSnapshot partial = RichSnapshot();
        partial.generation++;
        partial.refs.push_back(
            {"unloaded", {}, "unloaded-revision", GG_NAMED_REF_LOCAL_TAG, false, false});
        application.SetSnapshotForTest(std::move(partial));
        context->Yield(2);
        FocusWindow(context, "Tags");
        context->ItemClick("**/unloaded", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Reveal commit");
        context->Yield(2);
        IM_CHECK(application.HistoryLoadPendingForTest());

        history = ImGui::FindWindowByName("History");
        IM_CHECK_NE(history, nullptr);
        const auto searching_graph = std::ranges::find_if(history->DC.ChildWindows, [](const ImGuiWindow* child) {
            return std::string_view(child->Name).find("graph scroll") != std::string_view::npos;
        });
        IM_CHECK(searching_graph != history->DC.ChildWindows.end());
        IM_CHECK_EQ((*searching_graph)->Rect().Min.y, stable_graph_rect.Min.y);
        IM_CHECK_EQ((*searching_graph)->Rect().Max.y, stable_graph_rect.Max.y);
        const ImGuiWindow* toolbar = ImGui::FindWindowByName("ggui dockspace");
        IM_CHECK_NE(toolbar, nullptr);
        context->SetRef("ggui dockspace");
        const ImGuiTestItemInfo cancel = context->ItemInfo("Cancel history search");
        // Transient loading UI belongs to the fixed toolbar, not above the
        // graph scroll viewport where appearing/disappearing would shake it.
        IM_CHECK_GE(cancel.RectFull.Min.y, toolbar->WorkRect.Min.y);
        IM_CHECK_LE(cancel.RectFull.Max.y, toolbar->WorkRect.Max.y);
        context->ItemClick("**/Cancel history search");
        IM_CHECK(!application.HistoryLoadPendingForTest());

        RepoSnapshot scrollable = RichSnapshot();
        scrollable.generation++;
        scrollable.refs.clear();
        scrollable.revisions.clear();
        scrollable.working_copy.clear();
        scrollable.head.clear();
        for (int index = 0; index < 20000; ++index)
        {
            Revision revision;
            revision.oid = "scroll-" + std::to_string(index);
            revision.description = "Scroll";
            revision.author = "Author";
            scrollable.revisions.push_back(std::move(revision));
        }
        application.SetSnapshotForTest(std::move(scrollable));
        FocusWindow(context, "History");
        context->ItemInputValue("##graph filter", "");
        for (int attempt = 0; attempt < 1000
            && (application.RenderedHistoryRowsForTest() == 0
                || application.VisibleHistoryRevisionsForTest().size() != 20000); ++attempt)
        {
            context->Yield();
            if (application.RenderedHistoryRowsForTest() == 0) std::this_thread::sleep_for(5ms);
        }
        IM_CHECK_GT(application.RenderedHistoryRowsForTest(), 0U);
        // The list clipper must submit only viewport rows even when tens of
        // thousands of commits are loaded.
        IM_CHECK_LT(application.RenderedHistoryRowsForTest(), 100U);
        history = ImGui::FindWindowByName("History");
        IM_CHECK_NE(history, nullptr);
        const auto scroll_graph = std::ranges::find_if(history->DC.ChildWindows, [](const ImGuiWindow* child) {
            return std::string_view(child->Name).find("graph scroll") != std::string_view::npos;
        });
        IM_CHECK(scroll_graph != history->DC.ChildWindows.end());
        ImGui::SetScrollY(*scroll_graph, (*scroll_graph)->ScrollMax.y);
        context->Yield(3);
        // Scrolling is inspection, not consent to fetch more repository
        // history. Only the explicit load controls may advance the cursor.
        IM_CHECK(!application.HistoryLoadPendingForTest());
    };

    test = IM_REGISTER_TEST(engine, "Application", "PushBookmarkDialog");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        application.SelectRevisionForTest("left");
        context->Yield(2);
        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/coverage-bookmark", ImGuiMouseButton_Right);
        context->Yield();
        context->SetRef("//$FOCUSED");
        context->ItemClick("**/Push to...");
        ImGuiWindow* dialog = WaitForWindow(context, "ggui action");
        IM_CHECK_NE(dialog, nullptr);
        context->SetRef("ggui action");
        IM_CHECK_EQ(GImGui->NavId, context->ItemInfo("Remote").ID);
        IM_CHECK(RenderedTextContains(context, "Push bookmark coverage-bookmark"));
        IM_CHECK(context->ItemExists("Force push"));
        IM_CHECK(!RenderedTextContains(context, "Warning: force push can overwrite remote history."));
        context->ItemClick("Force push");
        context->Yield();
        IM_CHECK(RenderedTextContains(context, "Warning: force push can overwrite remote history."));
        IM_CHECK(context->ItemExists("Push"));
        const float width = dialog->SizeFull.x;
        context->Yield(4);
        dialog = ImGui::FindWindowByName("ggui action");
        IM_CHECK_NE(dialog, nullptr);
        IM_CHECK_LE(std::fabs(dialog->SizeFull.x - width), 0.01f);
        IM_CHECK_LE(std::fabs(dialog->SizeFull.x - 560.0f), 0.01f);
        context->WindowClose("//$FOCUSED");
        context->Yield(3);
        dialog = ImGui::FindWindowByName("ggui action");
        IM_CHECK(dialog == nullptr || !dialog->Active);
    };

    test = IM_REGISTER_TEST(engine, "Application", "CreateBookmarkFromGraph");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        context->Yield(2);
        const std::vector<ImGuiID> rows = GatherItems(context, "//History", "row");
        IM_CHECK(!rows.empty());
        context->SetRef("History");
        context->ItemClick(rows.front(), ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Create bookmark...");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        IM_CHECK_EQ(application.SelectedRevisionsForTest().size(), 1U);
        IM_CHECK((context->ItemInfo("Apply").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->ItemInputValue("Name", "from-graph");
        context->Yield();
        IM_CHECK((context->ItemInfo("Apply").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        context->ItemClick("Cancel");
    };

    test = IM_REGISTER_TEST(engine, "Application", "DialogUsabilityAndCommitScope");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        context->Yield(3);

        ApplyOpenDialog(context, "//##MainMenuBar/Repository/Clone...");
        IM_CHECK_EQ(GImGui->NavId, context->ItemInfo("URL").ID);
        IM_CHECK((context->ItemInfo("Apply").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->MouseMove("Apply");
        context->Yield();
        context->KeyPress(ImGuiKey_Escape);
        context->Yield(2);
        IM_CHECK(!ActionDialogOpen());

        context->KeyPress(ImGuiMod_Ctrl | ImGuiKey_N);
        context->Yield(2);
        IM_CHECK(!ActionDialogOpen());

        FocusWindow(context, "Changes");
        context->ItemClick("**/M  modified.txt");
        context->SetRef("ggui dockspace");
        context->ItemClick("Commit");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        IM_CHECK(application.DialogFilesetsForTest().empty());
        context->SetRef("ggui action");
        context->ItemClick("Cancel");

        FocusWindow(context, "Changes");
        context->ItemClick("**/M  modified.txt", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Commit only this file");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        IM_CHECK_EQ(application.DialogFilesetsForTest(), std::vector<std::string>{"modified.txt"});
        context->SetRef("ggui action");
        context->ItemClick("Cancel");

        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/Create bookmark");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        IM_CHECK((context->ItemInfo("Apply").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->ItemInputValue("Name", "validated");
        context->Yield();
        IM_CHECK((context->ItemInfo("Apply").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        context->ItemClick("Cancel");

        FocusWindow(context, "Remotes");
        context->ItemClick("**/Add remote");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        IM_CHECK((context->ItemInfo("Apply").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->ItemInputValue("Name", "backup");
        context->ItemInputValue("URL", "https://example.test/backup.git");
        context->Yield();
        IM_CHECK((context->ItemInfo("Apply").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        context->ItemClick("Cancel");

        application.ApplyEventForTest(OperationStarted{"visible operation"});
        context->Yield();
        application.ApplyEventForTest(OperationProgress{"preparing", 0, 0});
        context->Yield();
        application.ApplyEventForTest(OperationProgress{"writing", 1, 2});
        context->Yield();
        application.ApplyEventForTest(OperationFinished{"visible operation"});
        context->Yield();
        application.ApplyEventForTest(ErrorEvent{"visible operation", "visible failure"});
        context->Yield();
        context->SetRef("ggui dockspace");
        IM_CHECK(context->ItemExists("**/Dismiss error"));
        context->ItemClick("**/Dismiss error");
        context->Yield();
        IM_CHECK(!context->ItemExists("**/Dismiss error"));

        context->MouseMove("Prev");
        context->Yield();
        context->MouseMove("Next");
        context->Yield();
        const std::string repository_name = Repository().Path().filename().string();
        context->MouseMove("Repository");
        context->Yield();
        const ImGuiTestItemInfo repository_combo = context->ItemInfo("Repository");
        const float expected_repository_width = ImGui::CalcTextSize(repository_name.c_str()).x
            + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();
        IM_CHECK_LE(std::fabs(repository_combo.RectFull.GetWidth() - expected_repository_width), 1.0f);
        IM_CHECK_GT(context->ItemInfo("Open repository folder").RectFull.Min.x, repository_combo.RectFull.Max.x);
    };

    test = IM_REGISTER_TEST(engine, "Application", "WindowSettingsRoundTrip");
    test->TestFunc = [](ImGuiTestContext* context) {
        IM_CHECK(ImGui::GetIO().IniFilename == nullptr);
        std::size_t original_size = 0;
        const char* original_data = ImGui::SaveIniSettingsToMemory(&original_size);
        const std::string original(original_data, original_size);
        constexpr std::string_view ignored = "[Ggui][OtherWindow]\nSize=400,300\n\n";
        ImGui::LoadIniSettingsFromMemory(ignored.data(), ignored.size());
        constexpr std::string_view maximized =
            "[Ggui][MainWindow]\nPos=101,102\nSize=901,602\nMaximized=1\n\n";
        ImGui::LoadIniSettingsFromMemory(maximized.data(), maximized.size());
        context->Yield(2);
        constexpr std::string_view settings =
            "[Ggui][MainWindow]\nPos=101,102\nSize=901,602\nMaximized=0\n\n";
        ImGui::LoadIniSettingsFromMemory(settings.data(), settings.size());
        context->Yield(2);
        std::size_t saved_size = 0;
        const char* saved_data = ImGui::SaveIniSettingsToMemory(&saved_size);
        const std::string_view saved(saved_data, saved_size);
        IM_CHECK(saved.find("[Ggui][MainWindow]") != std::string_view::npos);
        IM_CHECK(saved.find("Size=901,602") != std::string_view::npos);
        IM_CHECK(saved.find("Maximized=0") != std::string_view::npos);
        ImGui::LoadIniSettingsFromMemory(original.data(), original.size());
        context->Yield(2);
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "MultiParentNewChange");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        context->Yield(3);
        const std::vector<ImGuiID> rows = GatherItems(context, "//History", "row");
        IM_CHECK_GE(rows.size(), 3U);

        context->KeyDown(ImGuiMod_Ctrl);
        context->ItemClick(rows[0]);
        context->KeyUp(ImGuiMod_Ctrl);
        IM_CHECK(application.SelectedRevisionsForTest().empty());
        context->SetRef("ggui dockspace");
        IM_CHECK((context->ItemInfo("New").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        context->MenuClick("//##MainMenuBar/Change");
        context->SetRef("//$FOCUSED");
        IM_CHECK((context->ItemInfo("New change").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->KeyPress(ImGuiKey_Escape);
        context->Yield();

        context->KeyDown(ImGuiMod_Ctrl);
        context->ItemClick(rows[1]);
        context->ItemClick(rows[2]);
        context->KeyUp(ImGuiMod_Ctrl);
        IM_CHECK_EQ(application.SelectedRevisionsForTest().size(), 2U);
        IM_CHECK_EQ(application.SelectedRevisionsForTest()[0], "left");
        IM_CHECK_EQ(application.SelectedRevisionsForTest()[1], "right");
        const std::vector<std::string> parents = application.SelectedParentsForTest();
        IM_CHECK_EQ(parents.size(), 2U);
        IM_CHECK_EQ(parents[0], "left");
        IM_CHECK_EQ(parents[1], "right");
        context->SetRef("ggui dockspace");
        IM_CHECK((context->ItemInfo("New").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        context->MenuClick("//##MainMenuBar/Change/New change");
        context->Yield(2);
        IM_CHECK(!ActionDialogOpen());
    };

    test = IM_REGISTER_TEST(engine, "Application", "PureHelpers");
    test->TestFunc = [](ImGuiTestContext*) {
        const auto lines = Application::SplitLinesForTest(" first, second\n\n third , ");
        IM_CHECK_EQ(lines.size(), 3U);
        IM_CHECK_EQ(lines[0], "first");
        IM_CHECK_EQ(lines[1], "second");
        IM_CHECK_EQ(lines[2], "third");
        IM_CHECK(Application::SplitLinesForTest({}).empty());
        const std::array<std::pair<git_delta_t, const char*>, 10> names{{
            {GIT_DELTA_ADDED, "A"}, {GIT_DELTA_DELETED, "D"}, {GIT_DELTA_MODIFIED, "M"},
            {GIT_DELTA_RENAMED, "R"}, {GIT_DELTA_COPIED, "C"}, {GIT_DELTA_TYPECHANGE, "T"},
            {GIT_DELTA_UNTRACKED, "?"}, {GIT_DELTA_IGNORED, "I"}, {GIT_DELTA_CONFLICTED, "C"},
            {GIT_DELTA_UNMODIFIED, " "},
        }};
        for (const auto& [status, name] : names)
            IM_CHECK_EQ(Application::DeltaNameForTest(status), name);
        IM_CHECK(Application::ContainsInsensitiveForTest("Graph First", "gRaPh"));
        IM_CHECK(!Application::ContainsInsensitiveForTest("Graph First", "missing"));
        IM_CHECK_NE(Application::IdColorForTest(true), Application::IdColorForTest(false));
        IM_CHECK(Application::SupportsDiffLanguageForTest("source.cpp"));
        IM_CHECK(Application::SupportsDiffLanguageForTest("shader.glsl"));
        const std::string limited = Application::LimitLinesForTest(
            "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17", 16);
        IM_CHECK_EQ(static_cast<std::size_t>(std::ranges::count(limited, '\n')), 15U);
        IM_CHECK_EQ(limited, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16...");
        IM_CHECK(!Application::SupportsDiffLanguageForTest("README"));
        IM_CHECK_EQ(Application::FileUrlForTest("/tmp/a b\\c#d"), "file:///tmp/a%20b/c%23d");
        IM_CHECK_EQ(Application::ReferenceLabelForTest(
                        {"feature", {}, "left", GG_NAMED_REF_LOCAL_BOOKMARK}),
            "feature");
        IM_CHECK_EQ(Application::ReferenceLabelForTest(
                        {"feature", "origin", "left", GG_NAMED_REF_REMOTE_BOOKMARK}),
            "origin/feature");
        const RepoSnapshot snapshot = RichSnapshot();
        IM_CHECK_EQ(Application::ReferenceBadgeLabelForTest(snapshot.refs[5], snapshot.refs),
            std::make_pair(std::string("remote-bookmark"), std::size_t{0}));
        IM_CHECK(Application::ReferenceBadgeLabelForTest(snapshot.refs[6], snapshot.refs).first.empty());
        IM_CHECK_EQ(Application::ReferenceBadgeLabelForTest(snapshot.refs[8], snapshot.refs).first, "diverged");
        IM_CHECK_EQ(Application::ReferenceBadgeLabelForTest(snapshot.refs[9], snapshot.refs).first, "diverged");
        IM_CHECK_EQ(Application::ReferenceBadgeLabelForTest(snapshot.refs[2], snapshot.refs).first, "coverage-tag");
        IM_CHECK(Application::ReferenceBadgeLabelForTest(snapshot.refs[3], snapshot.refs).first.empty());
        IM_CHECK_EQ(Application::ReferenceBadgeLabelForTest(snapshot.refs[4], snapshot.refs).first, "remote-tag");
        const std::array bookmark_colors{
            Application::BookmarkColorForTest("coverage-bookmark", snapshot.refs),
            Application::BookmarkColorForTest("remote-only", snapshot.refs),
            Application::BookmarkColorForTest("remote-bookmark", snapshot.refs),
            Application::BookmarkColorForTest("diverged", snapshot.refs)};
        for (std::size_t left = 0; left < bookmark_colors.size(); ++left)
            for (std::size_t right = left + 1; right < bookmark_colors.size(); ++right)
                IM_CHECK_NE(bookmark_colors[left], bookmark_colors[right]);
        std::vector<NamedRef> desynchronized{
            {"feature", {}, "local", GG_NAMED_REF_LOCAL_BOOKMARK},
            {"feature", "origin", "remote", GG_NAMED_REF_REMOTE_BOOKMARK, true, false, 3, 2, true}};
        IM_CHECK_EQ(ApplicationInternal::RefRemotes(
                        desynchronized, "feature", GG_NAMED_REF_REMOTE_BOOKMARK),
            "origin -2 +3");
        desynchronized[1].remote_commits = 0;
        IM_CHECK_EQ(ApplicationInternal::RefRemotes(
                        desynchronized, "feature", GG_NAMED_REF_REMOTE_BOOKMARK),
            "origin +3");
        desynchronized[1].local_commits = 0;
        IM_CHECK_EQ(ApplicationInternal::RefRemotes(
                        desynchronized, "feature", GG_NAMED_REF_REMOTE_BOOKMARK),
            "origin");
        IM_CHECK_EQ(Application::FormatTimestampForTest(0), "Unknown date");
        IM_CHECK_EQ(Application::FormatTimestampForTest(1'700'000'000).size(), 16U);
        IM_CHECK_EQ(Application::DropPlacementForTest(0), GG_REORDER_AFTER);
        IM_CHECK_EQ(Application::DropPlacementForTest(1), GG_REORDER_BEFORE);
        IM_CHECK_EQ(Application::DropTooltipForTest(0), "Move as child of");
        IM_CHECK_EQ(Application::DropTooltipForTest(1), "Move as parent of");
        IM_CHECK_EQ(Application::DropTooltipForTest(0, false, true), "Copy as child of");
        IM_CHECK_EQ(Application::DropTooltipForTest(1, false, true), "Copy as parent of");
        IM_CHECK_EQ(Application::DropTooltipForTest(2), "Squash change into");
        IM_CHECK_EQ(Application::DropTooltipForTest(2, true), "Squash entire branch into");
        IM_CHECK_EQ(Application::DropTooltipForTest(3), "Rebase change onto");
        IM_CHECK_EQ(Application::DropTooltipForTest(3, true), "Rebase entire branch onto");
        IM_CHECK_NE(ImGui::GetFontBaked()->FindGlyphNoFallback(0xf097), nullptr);
        IM_CHECK(std::string_view(ICON_MS_EDIT).size() > 1);
    };

    test = IM_REGISTER_TEST(engine, "Application", "SelectionSurvivesMetadataRefresh");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        struct ResetSnapshot
        {
            Application& application;
            ~ResetSnapshot() { application.ClearSnapshotForTest(); }
        } reset{application};
        const auto reset_snapshot = [&] {
            // Each independent scenario starts a fresh request sequence;
            // SetSnapshotForTest itself advances the applied history request.
            application.ClearSnapshotForTest();
            application.SetSnapshotForTest(RichSnapshot());
        };
        std::uint64_t history_request = 1000000;
        const auto refresh_history = [&](std::vector<Revision> revisions) {
            auto view = std::make_shared<HistoryView>();
            view->repository_generation = application.SnapshotForTest()->repository_generation;
            view->request = ++history_request;
            for (Revision& revision : revisions)
            {
                HistoryItem item;
                item.id = revision.oid;
                item.kind = HistoryItemKind::Commit;
                item.revision = std::move(revision);
                view->items.push_back(std::move(item));
            }
            application.ApplyEventForTest(HistoryReady{std::move(view)});
        };

        reset_snapshot();
        application.SelectRevisionForTest("left");
        application.SelectRevisionForTest("right", true);
        // Metadata refreshes do not carry the bounded history view. A selected
        // change may be outside the current view, so preserve its identity.
        RepoSnapshot without_right = RichSnapshot();
        std::erase_if(without_right.revisions, [](const Revision& revision) { return revision.oid == "right"; });
        application.ApplyEventForTest(SnapshotReady{std::make_shared<RepoSnapshot>(std::move(without_right))});
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), (std::vector<std::string>{"left", "right"}));
        Revision visible_left;
        visible_left.oid = "left";
        refresh_history({visible_left});
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), (std::vector<std::string>{"left", "right"}));

        reset_snapshot();
        application.SelectRevisionForTest("left");
        RepoSnapshot without_left = RichSnapshot();
        std::erase_if(without_left.revisions, [](const Revision& revision) { return revision.oid == "left"; });
        application.ApplyEventForTest(SnapshotReady{std::make_shared<RepoSnapshot>(std::move(without_left))});
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"left"});

        reset_snapshot();
        RepoSnapshot without_working_copy = RichSnapshot();
        without_working_copy.working_copy.clear();
        for (Revision& revision : without_working_copy.revisions)
            revision.working_copy = false;
        application.ApplyEventForTest(
            SnapshotReady{std::make_shared<RepoSnapshot>(std::move(without_working_copy))});
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"left"});

        application.SelectRevisionForTest("left", true);
        IM_CHECK(application.SelectedRevisionsForTest().empty());

        reset_snapshot();
        application.SelectRevisionForTest("left");
        application.ApplyEventForTest(
            DiffReady{{1000, "left", "modified.txt", "old\n", "new\n", false, RichSnapshot().status}});
        RepoSnapshot rewritten = RichSnapshot();
        rewritten.generation++;
        for (Revision& revision : rewritten.revisions)
            if (revision.oid == "left")
            {
                revision.oid = "left-rewritten";
                revision.aliases.push_back("left");
            }
        for (NamedRef& ref : rewritten.refs)
            if (ref.target == "left") ref.target = "left-rewritten";
        application.ApplyEventForTest(SnapshotReady{std::make_shared<RepoSnapshot>(std::move(rewritten))});
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"left"});
        IM_CHECK_EQ(application.SelectedFileForTest(), "modified.txt");
        Revision rewritten_left;
        rewritten_left.oid = "left-rewritten";
        rewritten_left.aliases = {"left"};
        refresh_history({rewritten_left});
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"left-rewritten"});
        IM_CHECK_EQ(application.SelectedFileForTest(), "modified.txt");
        application.ApplyEventForTest(
            DiffReady{{1001, "left", "stale.txt", "old\n", "new\n", false, RichSnapshot().status}});
        IM_CHECK_EQ(application.SelectedFileForTest(), "modified.txt");
        application.ApplyEventForTest(
            DiffReady{{1001, "left-rewritten", "fallback.txt", "old\n", "new\n", false, RichSnapshot().status}});
        IM_CHECK_EQ(application.SelectedFileForTest(), "fallback.txt");

        reset_snapshot();
        application.SelectRevisionForTest("left");
        application.SelectRevisionForTest("right", true);
        application.SelectRevisionForTest("third", true);
        Revision combined;
        combined.oid = "combined";
        combined.aliases = {"left", "right"};
        refresh_history({combined});
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), (std::vector<std::string>{"combined", "third"}));

        reset_snapshot();
        application.SelectRevisionForTest("left");
        application.ToggleFileComparisonForTest();
        IM_CHECK_EQ(application.CompareToForTest(), "merge");
        Revision current;
        current.oid = "merge";
        current.aliases = {"left"};
        refresh_history({current});
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"merge"});
        IM_CHECK(application.CompareToForTest().empty());
        IM_CHECK(!application.FileComparisonForTest());

        reset_snapshot();
        RepoSnapshot empty = RichSnapshot();
        empty.working_copy.clear();
        empty.head.clear();
        empty.revisions.clear();
        application.ApplyEventForTest(SnapshotReady{std::make_shared<RepoSnapshot>(std::move(empty))});
        IM_CHECK(application.SelectedRevisionsForTest().empty());
        context->Yield(2);
    };

    test = IM_REGISTER_TEST(engine, "Application", "LockedChangesAndChangeInformation");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        application.SelectRevisionForTest("merge");
        context->Yield(2);
        FocusWindow(context, "Change information");
        ImGuiWindow* change_information = ImGui::FindWindowByName("Change information");
        IM_CHECK_NE(change_information, nullptr);
        IM_CHECK(RenderedTextContains(context, "Merger"));
        const float metadata_y = change_information->DC.CursorStartPos.y;
        for (const char* parent : {"left", "right", "third"})
            IM_CHECK_LE(std::fabs(context->ItemInfo((std::string("**/") + parent).c_str()).RectFull.Min.y
                - metadata_y), 1.0f);
        context->ItemClick("**/left", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK(context->ItemExists("**/Short commit ID"));
        IM_CHECK(context->ItemExists("**/Full commit ID"));
        context->ItemClick("**/Short commit ID");
        IM_CHECK_STR_EQ(ImGui::GetClipboardText(), "left");
        context->SetRef("Change information");
        context->ItemClick("**/left");
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"left"});
        application.SelectRevisionForTest("merge");
        context->Yield(2);
        HoverRichSnapshotAuthor(context);
        IM_CHECK(RenderedTextContains(context, "merger@example.test"));
        context->MouseClick(ImGuiMouseButton_Right);
        context->Yield();
        context->SetRef("//$FOCUSED");
        for (const char* action : {"Copy author", "Copy email", "Edit author"})
            IM_CHECK(context->ItemExists((std::string("**/") + action).c_str()));
        context->ItemClick("**/Copy email");
        IM_CHECK_STR_EQ(ImGui::GetClipboardText(), "merger@example.test");
        HoverRichSnapshotAuthor(context);
        context->MouseClick(ImGuiMouseButton_Right);
        context->Yield();
        context->SetRef("//$FOCUSED");
        context->ItemClick("**/Edit author");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        const ImGuiID author_input = context->ItemInfo("Author").ID;
        IM_CHECK_EQ(GImGui->NavId, author_input);
        const ImGuiInputTextState* author_state = ImGui::GetInputTextState(author_input);
        IM_CHECK_NE(author_state, nullptr);
        IM_CHECK_STR_EQ(author_state->TextA.Data, "Merger <merger@example.test>");
        context->ItemClick("Cancel");

        application.SelectRevisionForTest("right");
        context->Yield(3);

        FocusWindow(context, "Change information");
        context->ItemInputValue("**/##commit message", "updated locked message");
        context->ItemClick("Save message");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        IM_CHECK(context->ItemExists("Confirm"));
        IM_CHECK_EQ(GImGui->NavId, context->ItemInfo("Cancel").ID);
        context->ItemClick("Cancel");

        application.SelectRevisionForTest("third");
        context->SetRef("ggui dockspace");
        context->ItemClick("New");
        context->Yield(2);
        IM_CHECK(!ActionDialogOpen());
        FocusWindow(context, "History");
        context->KeyPress(ImGuiKey_A);
        context->Yield(2);
        IM_CHECK(!ActionDialogOpen());

        RepoSnapshot empty_working_copy = RichSnapshot();
        empty_working_copy.working_copy = "third";
        application.SetSnapshotForTest(std::move(empty_working_copy));
        application.SelectRevisionForTest("third");
        FocusWindow(context, "History");
        context->KeyPress(ImGuiKey_A);
        context->Yield(2);
        IM_CHECK(!ActionDialogOpen());

        application.SetSnapshotForTest(RichSnapshot());

        application.SelectRevisionForTest("right");
        FocusWindow(context, "History");
        context->KeyPress(ImGuiKey_A);
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        IM_CHECK(context->ItemExists("Apply"));
        IM_CHECK(context->ItemExists("Also abandon all descendants (full branch)"));
        IM_CHECK(context->ItemExists("Also delete bookmark from remote"));
        IM_CHECK(context->ItemIsChecked("Retain bookmarks"));
        IM_CHECK_EQ(GImGui->NavId, context->ItemInfo("Cancel").ID);
        context->ItemCheck("Also delete bookmark from remote");
        context->ItemClick("Cancel");

        application.SelectRevisionForTest("left");
        FocusWindow(context, "History");
        context->KeyPress(ImGuiKey_A);
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        IM_CHECK(!context->ItemExists("Also delete bookmark from remote"));
        IM_CHECK(context->ItemIsChecked("Retain bookmarks"));
        context->ItemClick("Cancel");

        application.SelectRevisionForTest("base");
        FocusWindow(context, "History");
        context->KeyPress(ImGuiKey_A);
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemCheck("Also abandon all descendants (full branch)");
        IM_CHECK(RenderedTextContains(context, "5 changes will be abandoned."));
        context->ItemClick("Cancel");

        application.SelectRevisionForTest("right");
        application.ShowDropConfirmationForTest("left", "right", 0);
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        IM_CHECK(context->ItemExists("Confirm"));
        IM_CHECK_EQ(GImGui->NavId, context->ItemInfo("Confirm").ID);
        context->ItemClick("Cancel");
    };

    test = IM_REGISTER_TEST(engine, "Presentation", "RecentRepositoryMenu");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        const std::vector<std::string> paths{
            "/north/shared/codex-same", "/south/shared/codex-same",
            "/east/unique/codex-same", "/secret-parent/visible/codex-solo"};
        const auto labels = Application::RecentRepositoryLabelsForTest(paths);
        IM_CHECK_EQ(labels, (std::vector<std::pair<std::string, std::string>>{
                                {"north/shared/", "codex-same"},
                                {"south/shared/", "codex-same"},
                                {"east/unique/", "codex-same"},
                                {{}, "codex-solo"}}));

        application.SetSnapshotForTest(RichSnapshot());
        for (const std::string& path : paths) application.AddRecentForTest(path);
        context->Yield(3);
        context->SetRef("ggui dockspace");
        context->ItemClick("Repository");
        context->Yield();
        context->SetRef("//$FOCUSED");
        IM_CHECK(context->ItemExists("##recent repository filter"));
        context->ItemInputValue("##recent repository filter", "secret-parent");
        context->Yield();
        IM_CHECK(RenderedTextContains(context, "No matching repositories."));
        context->ItemInputValue("##recent repository filter", "east/unique");
        context->Yield();
        IM_CHECK(!RenderedTextContains(context, "No matching repositories."));
        IM_CHECK(context->ItemExists(RecentRepositoryItem(context, paths[2])));
        for (const std::size_t index : {0U, 1U, 3U})
            IM_CHECK(!context->ItemExists(RecentRepositoryItem(context, paths[index])));
        context->ItemInputValue("##recent repository filter", "");
        context->KeyPress(ImGuiKey_Escape);

        context->MenuAction(ImGuiTestAction_Open, "//##MainMenuBar/Repository/Recent");
        context->Yield(2);
        context->SetRef("//###Menu_01");
        IM_CHECK(context->ItemExists("##recent repository filter"));
        const ImGuiID removed_id = RecentRepositoryItem(context, paths[3]);
        const ImGuiTestItemInfo removed = context->ItemInfo(removed_id);
        context->MouseMoveToPos(removed.RectFull.GetCenter());
        context->Yield();
        context->KeyPress(ImGuiKey_Delete);
        context->Yield();
        IM_CHECK(!context->ItemExists(removed_id));
        context->KeyPress(ImGuiKey_Escape);
        context->KeyPress(ImGuiKey_Escape);
    };

    test = IM_REGISTER_TEST(engine, "Presentation", "RichRepositoryStates");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        const auto rendered_id = [context](std::string_view expected) {
            std::string text = CaptureRenderedText(context);
            std::erase_if(text, [](char value) { return value == ' ' || value == '\n' || value == '\r' || value == '\t'; });
            return text.find(expected) != std::string::npos;
        };
        RepoSnapshot rich_snapshot = RichSnapshot();
        rich_snapshot.remotes.push_back({"upstream", "https://example.test/upstream.git", ""});
        application.SetSnapshotForTest(rich_snapshot);
        for (int index = 0; index < 12; ++index)
            application.AddRecentForTest("recent-" + std::to_string(index));
        context->Yield(4);
        context->SetRef("ggui dockspace");
        const ImGuiTestItemInfo repository_combo = context->ItemInfo("Repository");
        const ImGuiTestItemInfo folder_button = context->ItemInfo("Open repository folder");
        IM_CHECK_GT(folder_button.RectFull.Min.x, repository_combo.RectFull.Max.x);
        IM_CHECK_LT(folder_button.RectFull.GetWidth(), ImGui::CalcTextSize("Open repository folder").x);
        IM_CHECK(rendered_id("@merge"));
        RepoSnapshot without_working_copy = RichSnapshot();
        without_working_copy.working_copy.clear();
        application.SetSnapshotForTest(std::move(without_working_copy));
        context->Yield(2);
        context->SetRef("ggui dockspace");
        IM_CHECK(rendered_id("HEADleft"));
        // Text capture renders a logging frame. Let normal item rectangles and
        // mouse navigation settle before opening the width-fit toolbar picker.
        FocusWindow(context, "ggui dockspace");
        context->Yield(2);
        IM_CHECK((context->ItemInfo("Repository").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        context->ItemClick("Repository");
        context->Yield();
        context->SetRef("//$FOCUSED");
        context->ItemInputValue("##recent repository filter", "recent-11");
        context->Yield();
        ImGuiID recent_item = RecentRepositoryItem(context, "recent-11");
        IM_CHECK(context->ItemExists(recent_item));
        IM_CHECK((context->ItemInfo(recent_item).ItemFlags & ImGuiItemFlags_Disabled) == 0);
        context->ItemClick(recent_item);
        context->Yield();
        if (std::getenv("GGUI_CAPTURE_MANUAL") != nullptr)
        {
            context->CaptureReset();
            IM_CHECK(context->CaptureScreenshot(ImGuiCaptureFlags_HideMouseCursor));
        }
        application.SetDarkThemeForTest(false);
        IM_CHECK_EQ(ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_TabActive]),
            ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_WindowBg]));
        context->Yield(2);
        application.SetDarkThemeForTest(true);
        IM_CHECK_EQ(ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_TabUnfocusedActive]),
            ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_WindowBg]));
        context->Yield(2);

        context->MenuAction(ImGuiTestAction_Open, "//##MainMenuBar/Repository/Recent");
        context->Yield(2);
        context->SetRef("//###Menu_01");
        context->ItemInputValue("##recent repository filter", "recent-11");
        context->Yield();
        recent_item = RecentRepositoryItem(context, "recent-11");
        IM_CHECK(context->ItemExists(recent_item));
        context->ItemClick(recent_item);
        application.ApplyEventForTest(ErrorEvent{"coverage", "welcome error"});
        application.ClearSnapshotForTest();
        context->Yield(3);
        context->SetRef("Welcome");
        context->ItemClick("**/recent-11");
        application.SetSnapshotForTest(rich_snapshot);
        context->Yield(3);

        context->SetRef("History");
        context->ItemInputValue("##graph filter", "COVERAGE-BOOKMARK");
        context->Yield(2);
        context->ItemInputValue("##graph filter", "change-RIGHT");
        context->Yield(2);
        context->ItemInputValue("##graph filter", "does-not-match");
        context->Yield(2);
        context->ItemInputValue("##graph filter", "");
        context->Yield(2);

        FocusWindow(context, "Remotes");
        context->KeyDown(ImGuiMod_Ctrl);
        context->ItemClick("**/upstream");
        context->KeyUp(ImGuiMod_Ctrl);
        context->Yield(2);
        FocusWindow(context, "Tags");
        IM_CHECK(context->ItemExists("**/coverage-tag"));
        IM_CHECK(context->ItemExists("**/remote-tag"));
        context->ItemClick("**/coverage-tag");
        context->Yield(2);
        context->ItemClick("**/coverage-tag", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK(context->ItemExists("**/Reveal commit"));
        context->ItemClick("**/Copy");
        context->Yield();
        IM_CHECK(context->ItemExists("**/Short commit ID"));
        IM_CHECK(context->ItemExists("**/Full commit ID"));
        context->KeyPress(ImGuiKey_Escape);
        context->KeyPress(ImGuiKey_Escape);
        FocusWindow(context, "Workspaces");
        IM_CHECK(context->ItemExists("**/Add workspace"));
        context->ItemClick("**/current", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK(context->ItemExists("**/Open directory"));
        context->KeyPress(ImGuiKey_Escape);
        FocusWindow(context, "Remotes");
        IM_CHECK(context->ItemExists("**/Add remote"));
        IM_CHECK(context->ItemExists("**/origin"));
        context->ItemClick("**/origin", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK(context->ItemExists("**/Pull"));
        IM_CHECK(context->ItemExists("**/Fetch"));
        IM_CHECK(context->ItemExists("**/Delete remote"));
        context->KeyPress(ImGuiKey_Escape);
        FocusWindow(context, "Bookmarks");
        IM_CHECK(context->ItemExists("**/remote-only"));
        IM_CHECK(context->ItemExists("**/diverged"));
        context->ItemClick("**/feature", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK(context->ItemExists("**/Reveal commit"));
        context->ItemClick("**/Reveal commit");
        context->Yield(2);
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"left"});
        context->ItemClick("**/coverage-bookmark");
        context->Yield(2);

        context->MenuClick("//##MainMenuBar/View/Operations");
        FocusWindow(context, "Operations");
        application.ApplyEventForTest(OperationStarted{"coverage operation"});
        context->Yield(2);
        application.ApplyEventForTest(OperationProgress{"preparing", 0, 0});
        context->Yield(2);
        application.ApplyEventForTest(OperationProgress{"writing", 2, 4});
        context->Yield(2);
        application.ApplyEventForTest(OperationFinished{"coverage operation"});
        application.ApplyEventForTest(ErrorEvent{"coverage", "visible error"});
        context->Yield(2);

        application.SelectRevisionForTest("merge");
        application.ApplyEventForTest(
            DiffReady{{1000, "merge", "image.bin", {}, {}, true, RichSnapshot().status}});
        FocusWindow(context, "Diff");
        context->Yield();
        application.SelectRevisionForTest("merge");
        application.ApplyEventForTest(
            DiffReady{{1000, "merge", "modified.txt", "old\n", "new\n", false,
                RichSnapshot().status}});
        context->Yield(2);
        application.ApplyEventForTest(DiffReady{{0, {}, {}, {}, {}, false, RichSnapshot().status}});
        context->Yield(2);

        context->SetRef("Changes");
        context->ItemClick("**/M  modified.txt");
        context->Yield(2);
        RepoSnapshot moved = RichSnapshot();
        moved.generation++;
        moved.working_copy = "left";
        application.ApplyEventForTest(SnapshotReady{std::make_shared<RepoSnapshot>(std::move(moved))});
        context->Yield(2);
        application.SetSnapshotForTest(rich_snapshot);
        context->Yield(2);
        context->SetRef("Changes");
        context->ItemClick("**/M  modified.txt", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Commit only this file");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");
        context->Yield(2);

        FocusWindow(context, "Operations");
        context->ItemClick("**/Restore");
        context->Yield(2);

        application.ApplyEventForTest(
            CredentialRequest{"https://example.test/repository", "suggested", GIT_CREDENTIAL_USERPASS_PLAINTEXT});
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        if (std::getenv("GGUI_CAPTURE_MANUAL") != nullptr)
        {
            context->CaptureReset();
            IM_CHECK(context->CaptureScreenshot(ImGuiCaptureFlags_HideMouseCursor));
        }
        context->SetRef("ggui action");
        context->ItemInputValue("Username", "user");
        context->ItemInputValue("Token or passphrase", "secret");
        context->ItemClick("Apply");
        context->Yield(2);

        application.ApplyEventForTest(
            CredentialRequest{"ssh://example.test/repository", "git", GIT_CREDENTIAL_SSH_KEY});
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ComboClick("Method/SSH key");
        context->Yield();
        context->ItemInputValue("Private key", "/tmp/key");
        context->ItemInputValue("Public key", "/tmp/key.pub");
        context->ItemClick("Apply");
        context->Yield(2);

        application.ApplyEventForTest(
            CredentialRequest{"ssh://example.test/repository", "git", GIT_CREDENTIAL_SSH_KEY});
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ComboClick("Method/SSH agent");
        context->Yield();
        context->ItemClick("Apply");
        context->Yield(2);

        application.ApplyEventForTest(
            CredentialRequest{"ssh://example.test/repository", "git", GIT_CREDENTIAL_SSH_KEY});
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");
        context->Yield(2);
    };

    test = IM_REGISTER_TEST(engine, "Presentation", "ListElisionTooltips");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        const std::string long_text(80, 'x');
        const std::string bookmark = "bookmark-" + long_text;
        const std::string tag = "tag-" + long_text;
        const std::string workspace = "workspace-" + long_text;
        const std::string remote = "remote-" + long_text;
        const std::string path = "directory/" + long_text + "/file.cpp";
        const std::string long_revision_id(40, 'a');
        RepoSnapshot snapshot = RichSnapshot();
        snapshot.refs.push_back({bookmark, {}, long_revision_id, GG_NAMED_REF_LOCAL_BOOKMARK, false, false});
        snapshot.refs.push_back({tag, {}, "left", GG_NAMED_REF_LOCAL_TAG, false, false});
        snapshot.workspaces.push_back({workspace, "/root/" + long_text, "merge", false});
        snapshot.remotes.push_back({remote, "https://fetch.example/" + long_text,
            "ssh://push.example/" + long_text});
        snapshot.status = {{{}, path, GIT_DELTA_MODIFIED, false}};
        application.SetSnapshotForTest(std::move(snapshot));
        context->Yield(3);

        const ImGuiWindow* bookmark_tooltip = nullptr;
        const auto check_tooltip = [&](const char* window, const std::string& item) {
            FocusWindow(context, window);
            context->MouseMove(("**/" + item).c_str());
            context->Yield(2);
            const ImGuiWindow* tooltip = GImGui->TooltipPreviousWindow;
            IM_CHECK(tooltip != nullptr && (tooltip->Active || tooltip->WasActive));
            if (item == bookmark)
                bookmark_tooltip = tooltip;
        };
        check_tooltip("Bookmarks", bookmark);
        IM_CHECK_NE(bookmark_tooltip, nullptr);
        ImGuiTestItemList tooltip_items;
        const std::string tooltip_path = std::string("//") + bookmark_tooltip->Name;
        context->GatherItems(&tooltip_items, tooltip_path.c_str());
        for (int index = 0; index < tooltip_items.GetSize(); ++index)
            IM_CHECK_EQ(std::string(tooltip_items.GetByIndex(index)->DebugLabel).find(long_revision_id),
                std::string::npos);
        bool colored_id = false;
        for (const ImDrawVert& vertex : bookmark_tooltip->DrawList->VtxBuffer)
            colored_id |= vertex.col == IM_COL32(47, 129, 247, 255);
        IM_CHECK(colored_id);
        check_tooltip("Tags", tag);
        check_tooltip("Workspaces", workspace);
        check_tooltip("Remotes", remote);
        FocusWindow(context, "Changes");
        ImGuiTestItemList change_items;
        context->GatherItems(&change_items, "//Changes");
        const ImGuiTestItemInfo* change_row = nullptr;
        for (int index = 0; index < change_items.GetSize(); ++index)
        {
            const ImGuiTestItemInfo* item = change_items.GetByIndex(index);
            if (std::string_view(item->DebugLabel).starts_with("M  "))
            {
                change_row = item;
                break;
            }
        }
        IM_CHECK_NE(change_row, nullptr);
        context->MouseMove(change_row->ID);
        context->Yield(2);
        const ImGuiWindow* tooltip = GImGui->TooltipPreviousWindow;
        IM_CHECK(tooltip != nullptr && (tooltip->Active || tooltip->WasActive));
    };

    test = IM_REGISTER_TEST(engine, "Presentation", "ExistingGitWorktree");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        RepoSnapshot snapshot = RichSnapshot();
        snapshot.conflicts.clear();
        snapshot.workspaces.push_back(
            {"existing-git-worktree", "/tmp/existing-git-worktree", "base", false, false});
        application.SetSnapshotForTest(std::move(snapshot));
        context->Yield(3);

        FocusWindow(context, "Workspaces");
        IM_CHECK(context->ItemExists("**/existing-git-worktree"));
        const std::string selected = application.SelectedRevisionsForTest().front();
        context->ItemClick("**/existing-git-worktree");
        context->Yield();
        IM_CHECK_EQ(application.SelectedRevisionsForTest().front(), selected);
        context->ItemClick("**/existing-git-worktree", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK((context->ItemInfo("**/Rename...").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("**/Open here").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK((context->ItemInfo("**/Open in new window").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK((context->ItemInfo("**/Remove...").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        context->KeyPress(ImGuiKey_Escape);
    };

    test = IM_REGISTER_TEST(engine, "Presentation", "ReferenceFilters");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application::Instance().SetSnapshotForTest(RichSnapshot());
        context->Yield(2);

        FocusWindow(context, "Bookmarks");
        IM_CHECK_GE(context->ItemInfo("##bookmark filter").RectFull.GetWidth(),
            context->ItemInfo("**/Create bookmark").RectFull.GetWidth() - 1.0f);
        context->ItemInputValue("##bookmark filter", "FEATURE");
        context->Yield(2);
        IM_CHECK(context->ItemExists("**/feature"));
        IM_CHECK(!context->ItemExists("**/coverage-bookmark"));
        context->ItemInputValue("##bookmark filter", "");

        FocusWindow(context, "Tags");
        IM_CHECK_GE(context->ItemInfo("##tag filter").RectFull.GetWidth(),
            context->ItemInfo("**/Create tag").RectFull.GetWidth() - 1.0f);
        context->ItemInputValue("##tag filter", "missing");
        context->Yield(2);
        IM_CHECK(!context->ItemExists("**/coverage-tag"));
        context->ItemInputValue("##tag filter", "COVERAGE");
        context->Yield(2);
        IM_CHECK(context->ItemExists("**/coverage-tag"));
        context->ItemInputValue("##tag filter", "");
    };

    test = IM_REGISTER_TEST(engine, "Presentation", "ChangesFilter");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application::Instance().SetSnapshotForTest(RichSnapshot());
        context->Yield(2);
        FocusWindow(context, "Changes");

        IM_CHECK_LE(std::fabs(context->ItemInfo("##changes filter").RectFull.Max.x
                            - ImGui::FindWindowByName("Changes")->WorkRect.Max.x),
            1.0f);

        context->ItemInputValue("##changes filter", "MODIFIED.TXT");
        context->Yield(2);
        IM_CHECK(context->ItemExists("**/M  modified.txt"));
        IM_CHECK(!context->ItemExists("**/A  added.txt"));

        context->ItemInputValue("##changes filter", "old.txt");
        context->Yield(2);
        IM_CHECK(context->ItemExists("**/R  renamed.txt"));
        IM_CHECK(!context->ItemExists("**/M  modified.txt"));

        context->ItemInputValue("##changes filter", "");
        context->Yield(2);
        IM_CHECK(context->ItemExists("**/A  added.txt"));
        IM_CHECK(context->ItemExists("**/M  modified.txt"));
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "ComparisonModeAndChangedFileNavigation");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        application.SelectRevisionForTest("left");
        application.ToggleComparisonForTest();
        IM_CHECK_EQ(application.CompareToForTest(), "merge");
        application.ApplyEventForTest(DiffReady{{1000, "left", "modified.txt", "old\n", "new\n", false,
            RichSnapshot().status, "merge"}});
        context->Yield(2);
        FocusWindow(context, "Changes");
        IM_CHECK(context->ItemIsChecked("Compare with @"));
        FocusWindow(context, "Diff");
        IM_CHECK(context->ItemExists("Compare with @"));
        IM_CHECK(!context->ItemIsChecked("Compare with @"));
        IM_CHECK_EQ(application.SelectedFileForTest(), "modified.txt");

        application.SelectRevisionForTest("right");
        IM_CHECK_EQ(application.CompareToForTest(), "merge");
        application.ApplyEventForTest(DiffReady{{1000, "right", "stale.txt", {}, "stale\n", false,
            {{{}, "stale.txt", GIT_DELTA_ADDED, false}}, "old-working-copy"}});
        IM_CHECK_EQ(application.SelectedFileForTest(), "modified.txt");

        RepoSnapshot rewritten = RichSnapshot();
        rewritten.generation++;
        rewritten.working_copy = "merge-rewritten";
        for (Revision& revision : rewritten.revisions)
            if (revision.oid == "merge") revision.oid = "merge-rewritten";
        for (NamedRef& ref : rewritten.refs)
            if (ref.target == "merge") ref.target = "merge-rewritten";
        application.ApplyEventForTest(SnapshotReady{std::make_shared<RepoSnapshot>(std::move(rewritten))});
        IM_CHECK_EQ(application.CompareToForTest(), "merge-rewritten");
        application.ApplyEventForTest(DiffReady{{1001, "right", "added.txt", {}, "fresh\n", false,
            {{{}, "added.txt", GIT_DELTA_ADDED, false}}, "merge-rewritten"}});
        IM_CHECK_EQ(application.SelectedFileForTest(), "added.txt");
        application.SelectRevisionForTest("merge-rewritten");
        IM_CHECK(application.CompareToForTest().empty());
        context->Yield(2);
        FocusWindow(context, "Changes");
        IM_CHECK(!context->ItemIsChecked("Compare with @"));
        FocusWindow(context, "Diff");
        IM_CHECK(!context->ItemIsChecked("Compare with @"));

        application.SetSnapshotForTest(RichSnapshot());
        application.SelectRevisionForTest("left");
        application.ToggleFileComparisonForTest();
        IM_CHECK(application.FileComparisonForTest());
        application.ApplyEventForTest(DiffReady{{1000, "left", "stale.txt", {}, "stale\n", false,
            {{{}, "stale.txt", GIT_DELTA_ADDED, false}}, "merge"}});
        IM_CHECK(application.SelectedFileForTest().empty());
        DiffResult file_comparison{1000, "left", "added.txt", "old\n", "file compare\n", false,
            RichSnapshot().status, "merge", true};
        file_comparison.selected_status = GIT_DELTA_MODIFIED;
        application.ApplyEventForTest(DiffReady{std::move(file_comparison)});
        context->Yield(2);
        FocusWindow(context, "Changes");
        IM_CHECK(!context->ItemIsChecked("Compare with @"));
        IM_CHECK(context->ItemExists("**/A  added.txt"));
        FocusWindow(context, "Diff");
        IM_CHECK(context->ItemIsChecked("Compare with @"));
        IM_CHECK(context->ItemExists("##diff view"));
        IM_CHECK(!context->ItemExists("##file view"));
        DiffResult identical{1000, "left", "added.txt", "same\n", "same\n", false,
            RichSnapshot().status, "merge", true};
        identical.selected_status = GIT_DELTA_UNMODIFIED;
        application.ApplyEventForTest(DiffReady{std::move(identical)});
        context->Yield(2);
        FocusWindow(context, "Diff");
        IM_CHECK(context->ItemIsChecked("Compare with @"));
        application.ToggleComparisonForTest();
        IM_CHECK(!application.FileComparisonForTest());
        IM_CHECK_EQ(application.CompareToForTest(), "merge");

        application.SetSnapshotForTest(RichSnapshot());
        IM_CHECK(application.CanNavigateChangedFileForTest(1));
        IM_CHECK(application.CanNavigateChangedFileForTest(-1));
        application.NavigateChangedFileForTest(1);
        IM_CHECK_EQ(application.SelectedFileForTest(), "added.txt");
        IM_CHECK(!application.CanNavigateChangedFileForTest(-1));
        FocusWindow(context, "Changes");
        context->KeyPress(ImGuiKey_DownArrow);
        context->Yield();
        IM_CHECK_EQ(application.SelectedFileForTest(), "deleted.txt");
        context->KeyPress(ImGuiKey_UpArrow);
        context->Yield();
        IM_CHECK_EQ(application.SelectedFileForTest(), "added.txt");
        context->ItemInputValue("##changes filter", "modified.txt");
        context->Yield(2);
        context->KeyPress(ImGuiKey_F6);
        IM_CHECK_EQ(application.SelectedFileForTest(), "modified.txt");
        IM_CHECK(!application.CanNavigateChangedFileForTest(-1));
        IM_CHECK(!application.CanNavigateChangedFileForTest(1));
        context->KeyPress(ImGuiMod_Shift | ImGuiKey_F6);
        IM_CHECK_EQ(application.SelectedFileForTest(), "modified.txt");
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "ComparisonLocksFileActionsAndPortableCopies");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        application.SelectRevisionForTest("left");
        application.ToggleComparisonForTest();
        application.ApplyEventForTest(DiffReady{{1000, "left", "modified.txt", "old\n", "new\n", false,
            RichSnapshot().status, "merge"}});
        context->Yield(3);

        context->SetRef("ggui dockspace");
        IM_CHECK((context->ItemInfo("Commit").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        FocusWindow(context, "Diff");
        IM_CHECK(!context->ItemExists("Apply Patch..."));

        FocusWindow(context, "Changes");
        context->ItemClick("**/M  modified.txt", ImGuiMouseButton_Right);
        context->Yield();
        for (const char* action : {"Move to parent", "Move to child", "Commit only this file",
                 "Revert", "Track", "Untrack"})
        {
            const std::string item = std::string("**/") + action;
            IM_CHECK(!context->ItemExists(item.c_str())
                || (context->ItemInfo(item.c_str()).ItemFlags & ImGuiItemFlags_Disabled) != 0);
        }
        const bool working_file_exists = std::filesystem::is_regular_file(Repository().Path() / "modified.txt");
        IM_CHECK_EQ((context->ItemInfo("**/Open working-copy file").ItemFlags & ImGuiItemFlags_Disabled) == 0,
            working_file_exists);
        IM_CHECK(context->ItemExists("**/Open containing folder"));
        context->ItemClick("**/Copy");
        context->Yield();
        context->ItemClick("**/Relative path");
        IM_CHECK_EQ(std::string(ImGui::GetClipboardText()), "modified.txt");
        const auto copy_file_value = [&](const char* action) {
            FocusWindow(context, "Changes");
            context->ItemClick("**/M  modified.txt", ImGuiMouseButton_Right);
            context->Yield();
            context->ItemClick("**/Copy");
            context->Yield();
            context->ItemClick((std::string("**/") + action).c_str());
            return std::string(ImGui::GetClipboardText());
        };
        IM_CHECK_EQ(copy_file_value("Name"), "modified.txt");
        IM_CHECK_EQ(copy_file_value("Absolute path"),
            std::filesystem::weakly_canonical(Repository().Path() / "modified.txt").string());

        const auto safe = Application::WorkingCopyPathForTest(Repository().Path().string(), "tracked.txt");
        IM_CHECK(safe.has_value());
        IM_CHECK_EQ(*safe, std::filesystem::weakly_canonical(Repository().Path() / "tracked.txt"));
        IM_CHECK(!Application::WorkingCopyPathForTest(Repository().Path().string(), "../outside.txt").has_value());

        context->MenuClick("//##MainMenuBar/Repository/Copy path");
        IM_CHECK_EQ(std::string(ImGui::GetClipboardText()), Repository().Path().string());

        const auto copy_reference_name = [&](const char* panel, const char* item) {
            FocusWindow(context, panel);
            context->ItemClick((std::string("**/") + item).c_str(), ImGuiMouseButton_Right);
            context->Yield();
            context->ItemClick("**/Copy name");
            return std::string(ImGui::GetClipboardText());
        };
        IM_CHECK_EQ(copy_reference_name("Bookmarks", "coverage-bookmark"), "coverage-bookmark");
        IM_CHECK_EQ(copy_reference_name("Tags", "coverage-tag"), "coverage-tag");
        IM_CHECK_EQ(copy_reference_name("Remotes", "origin"), "origin");
        IM_CHECK_EQ(copy_reference_name("Workspaces", "current"), "current");

        FocusWindow(context, "Workspaces");
        context->ItemClick("**/current", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Copy path");
        IM_CHECK_EQ(std::string(ImGui::GetClipboardText()), Repository().Path().string());

        FocusWindow(context, "History");
        const std::vector<ImGuiID> rows = GatherItems(context, "//History", "row");
        IM_CHECK(!rows.empty());
        const ImGuiID top_row = *std::ranges::min_element(rows, {}, [&](ImGuiID row) {
            return context->ItemInfo(row).RectFull.Min.y;
        });
        context->ItemClick(top_row, ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Full commit ID");
        IM_CHECK_EQ(std::string(ImGui::GetClipboardText()), "merge");
    };

    test = IM_REGISTER_TEST(engine, "Application", "CloseRepositoryAndRenameBookmark");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        RepoSnapshot snapshot = RichSnapshot();
        snapshot.remotes.push_back({"upstream", "https://example.test/upstream.git", ""});
        Application::Instance().SetSnapshotForTest(std::move(snapshot));
        application.AddRecentForTest("retained-repository");
        context->Yield(2);

        FocusWindow(context, "Remotes");
        context->KeyDown(ImGuiMod_Ctrl);
        context->ItemClick("**/upstream");
        context->KeyUp(ImGuiMod_Ctrl);
        context->Yield(2);
        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/feature", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Rename...");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        IM_CHECK((context->ItemInfo("Apply").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->ItemInputValue("New name", "coverage-bookmark");
        context->Yield();
        IM_CHECK((context->ItemInfo("Apply").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->ItemInputValue("New name", "feature-renamed");
        context->Yield();
        IM_CHECK((context->ItemInfo("Apply").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        context->ItemClick("Cancel");

        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/remote-only", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK((context->ItemInfo("**/Merge into @").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("**/Rebase @ onto bookmark").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("**/Rename...").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->KeyPress(ImGuiKey_Escape);

        application.ApplyEventForTest(OperationStarted{"close"});
        application.ApplyEventForTest(OperationFinished{"close"});
        context->Yield(3);
        IM_CHECK_EQ(application.SnapshotForTest(), nullptr);
        ImGuiWindow* welcome = ImGui::FindWindowByName("Welcome");
        IM_CHECK(welcome != nullptr && welcome->Active);
        context->SetRef("Welcome");
        IM_CHECK(context->ItemExists("**/retained-repository"));
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "BookmarkContextMenus");
    test->TestFunc = [](ImGuiTestContext* context) {
        RepoSnapshot snapshot = RichSnapshot();
        snapshot.remotes.push_back({"upstream", "https://example.test/upstream.git", ""});
        Application::Instance().SetSnapshotForTest(std::move(snapshot));
        context->Yield(2);
        FocusWindow(context, "Remotes");
        context->KeyDown(ImGuiMod_Ctrl);
        context->ItemClick("**/upstream");
        context->KeyUp(ImGuiMod_Ctrl);
        context->Yield(2);
        FocusWindow(context, "Bookmarks");

        context->ItemClick("**/remote-only", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK((context->ItemInfo("**/Push").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("**/Push to...").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("**/Delete").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        context->ItemClick("**/Delete");
        context->Yield();
        IM_CHECK((context->ItemInfo("**/Local").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("**/upstream").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        context->KeyPress(ImGuiKey_Escape);
        context->KeyPress(ImGuiKey_Escape);
        FocusWindow(context, "Bookmarks");

        context->ItemClick("**/remote-bookmark", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK((context->ItemInfo("**/Merge into @").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK((context->ItemInfo("**/Rebase @ onto bookmark").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("**/Push").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK((context->ItemInfo("**/Push to...").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK(!context->ItemExists("**/reconcile-origin"));
        context->ItemClick("**/Delete");
        context->Yield();
        IM_CHECK((context->ItemInfo("**/Local").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK((context->ItemInfo("**/origin").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        context->KeyPress(ImGuiKey_Escape);
        context->KeyPress(ImGuiKey_Escape);
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "DivergedBookmarkReconciliation");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        UiRepository repository;
        const std::string base = repository.RevisionId("HEAD");
        repository.Git("checkout -b left");
        repository.Git("commit --allow-empty -m left");
        const std::string left = repository.RevisionId("HEAD");
        repository.Git("checkout -b right main");
        repository.Git("commit --allow-empty -m right");
        const std::string right = repository.RevisionId("HEAD");
        repository.Git("checkout -b third main");
        repository.Git("commit --allow-empty -m third");
        const std::string third = repository.RevisionId("HEAD");
        repository.Git("checkout left");
        repository.Git("merge --no-ff right third -m merge");
        const std::string merge = repository.RevisionId("HEAD");
        const auto real_id = [&](const std::string& id) -> std::string {
            if (id == "base") return base;
            if (id == "left") return left;
            if (id == "right") return right;
            if (id == "third") return third;
            if (id == "merge") return merge;
            return id;
        };
        RepoSnapshot snapshot = RichSnapshot();
        snapshot.root = repository.Path().string();
        snapshot.head = left;
        snapshot.working_copy = merge;
        for (Revision& revision : snapshot.revisions)
        {
            revision.oid = real_id(revision.oid);
            for (std::string& parent : revision.parents) parent = real_id(parent);
        }
        for (NamedRef& ref : snapshot.refs) ref.target = real_id(ref.target);
        snapshot.remotes.push_back({"upstream", "https://example.test/upstream.git", ""});
        snapshot.refs.push_back(
            {"diverged", "upstream", third, GG_NAMED_REF_REMOTE_BOOKMARK, true, false});
        IM_CHECK_EQ(
            ClassifyBookmarkRelation(snapshot, left, right), BookmarkRelation::Diverged);
        application.SetSnapshotForTest(snapshot);
        context->Yield(2);
        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/diverged", ImGuiMouseButton_Right);
        context->Yield(2);
        IM_CHECK(context->ItemExists("**/Push"));
        IM_CHECK(context->ItemExists("**/reconcile-origin"));
        IM_CHECK(context->ItemExists("**/reconcile-upstream"));
        context->ItemClick("**/reconcile-origin");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        IM_CHECK((context->ItemInfo("Reconcile").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        context->ItemClick("Reconcile");

        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/diverged", ImGuiMouseButton_Right);
        context->Yield(2);
        context->ItemClick("**/reconcile-origin");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");

        ++snapshot.generation;
        application.ApplyEventForTest(SnapshotReady{std::make_shared<RepoSnapshot>(snapshot)});
        context->Yield(2);
        IM_CHECK((context->ItemInfo("Reconcile").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->ItemClick("Cancel");

        application.ApplyEventForTest(OperationStarted{"busy"});
        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/diverged", ImGuiMouseButton_Right);
        context->Yield(2);
        IM_CHECK((context->ItemInfo("**/reconcile-origin").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->KeyPress(ImGuiKey_Escape);
        application.ApplyEventForTest(OperationFinished{"busy"});
        application.ClearSnapshotForTest();
    };

    test = IM_REGISTER_TEST(engine, "Navigation", "HistorySearchRevealsPinnedMatch");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        RepoSnapshot snapshot;
        snapshot.generation = 2270;
        snapshot.repository_generation = 2270;
        snapshot.root = "/tmp/ggui-search-reveal";
        snapshot.head = "search-0";
        std::vector<Revision> revisions;
        for (int index = 0; index < 180; ++index)
        {
            Revision revision;
            revision.oid = "search-" + std::to_string(index);
            if (index + 1 < 180) revision.parents.push_back("search-" + std::to_string(index + 1));
            revision.description = "Search row " + std::to_string(index);
            revisions.push_back(revision);
        }
        snapshot.revisions = revisions;
        application.SetSnapshotForTest(std::move(snapshot));
        context->Yield(3);
        FocusWindow(context, "History");
        context->ItemInputValue("##graph filter", "needle");

        auto view = std::make_shared<HistoryView>();
        view->repository_generation = 2270;
        view->request = 1000000;
        view->search = "needle";
        for (std::size_t index = 0; index < revisions.size(); ++index)
        {
            HistoryItem item;
            item.id = revisions[index].oid;
            item.kind = HistoryItemKind::Commit;
            item.revision = revisions[index];
            item.parents = revisions[index].parents;
            item.search_match = index == 165;
            view->items.push_back(std::move(item));
        }
        auto repeated = std::make_shared<HistoryView>(*view);
        auto preview = std::make_shared<HistoryView>();
        preview->repository_generation = 2270;
        preview->request = 1000000;
        preview->skeleton = true;
        preview->search = "needle";
        preview->items.push_back(view->items.front());
        preview->items.push_back(view->items[165]);
        preview->items.front().parents.clear();
        preview->items.back().parents.clear();
        application.ApplyEventForTest(HistoryReady{std::move(preview)});
        context->Yield(2);
        application.ApplyEventForTest(HistoryReady{std::move(view)});
        context->Yield(5);

        ImGuiWindow* history = ImGui::FindWindowByName("History");
        IM_CHECK_NE(history, nullptr);
        const auto graph = std::ranges::find_if(history->DC.ChildWindows, [](const ImGuiWindow* child) {
            return std::string_view(child->Name).find("graph scroll") != std::string_view::npos;
        });
        IM_CHECK(graph != history->DC.ChildWindows.end());
        IM_CHECK_GT((*graph)->Scroll.y, 4000.0f);
        const std::vector<ImGuiID> visible_rows = GatherItems(context, "//History", "row");
        IM_CHECK(!visible_rows.empty());

        ImGui::SetScrollY(*graph, 1000.0f);
        context->Yield(2);
        application.ApplyEventForTest(HistoryReady{std::move(repeated)});
        context->Yield(2);
        IM_CHECK_LE(std::fabs((*graph)->Scroll.y - 1000.0f), 1.0f);
    };

    test = IM_REGISTER_TEST(engine, "Navigation", "HistoryExpansionPreservesViewport");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        RepoSnapshot snapshot;
        snapshot.generation = 2280;
        snapshot.repository_generation = 2280;
        snapshot.root = "/tmp/ggui-history-expansion";
        snapshot.head = "c0";
        snapshot.refs = {{"main", {}, "c12", GG_NAMED_REF_LOCAL_BOOKMARK},
            {"main", "origin", "c11", GG_NAMED_REF_REMOTE_BOOKMARK},
            {"v1", {}, "c13", GG_NAMED_REF_LOCAL_TAG}};
        application.SetSnapshotForTest(std::move(snapshot));

        const auto commit = [](std::string id, std::string parent) {
            HistoryItem item;
            item.id = id;
            item.kind = HistoryItemKind::Commit;
            item.revision.oid = std::move(id);
            item.revision.description = "Commit " + item.revision.oid;
            if (!parent.empty()) item.revision.parents.push_back(parent);
            return item;
        };
        auto initial = std::make_shared<HistoryView>();
        initial->repository_generation = 2280;
        initial->request = 1000100;
        for (int index = 0; index < 15; ++index)
        {
            HistoryItem item = commit("c" + std::to_string(index),
                index == 14 ? "hidden-0" : "c" + std::to_string(index + 1));
            item.parents = {index == 14 ? "region:hidden-0:old" : "c" + std::to_string(index + 1)};
            initial->items.push_back(std::move(item));
        }
        initial->items[12].revision.parents.push_back("merge-side");
        HistoryItem region;
        region.id = "region:hidden-0:old";
        region.kind = HistoryItemKind::CollapsedRegion;
        region.parents = {"old"};
        initial->items.push_back(region);
        HistoryItem old = commit("old", "tail-0");
        old.parents = {"tail-0"};
        initial->items.push_back(std::move(old));
        for (int index = 0; index < 25; ++index)
        {
            const std::string parent = index == 24 ? "" : "tail-" + std::to_string(index + 1);
            HistoryItem item = commit("tail-" + std::to_string(index), parent);
            if (!parent.empty()) item.parents = {parent};
            initial->items.push_back(std::move(item));
        }
        application.ApplyEventForTest(HistoryReady{initial});
        context->Yield(4);
        FocusWindow(context, "History");
        ImGuiWindow* history = ImGui::FindWindowByName("History");
        IM_CHECK_NE(history, nullptr);
        const auto graph = std::ranges::find_if(history->DC.ChildWindows, [](const ImGuiWindow* child) {
            return std::string_view(child->Name).find("graph scroll") != std::string_view::npos;
        });
        IM_CHECK(graph != history->DC.ChildWindows.end());
        ImGui::SetScrollY(*graph, 10.0f * ApplicationInternal::kRowHeight);
        context->Yield(3);
        const float scroll_before = (*graph)->Scroll.y;
        IM_CHECK(context->ItemExists("//History/**/main"));
        IM_CHECK(context->ItemExists("//History/**/v1"));
        const ImGuiTestItemInfo message = context->ItemInfo("//History/**/Commit c12");
        const ImGuiTestItemInfo bookmark = context->ItemInfo("//History/**/main");
        IM_CHECK(message.ID != 0);
        IM_CHECK(bookmark.ID != 0);
        IM_CHECK_GE(bookmark.RectFull.Min.x, message.RectFull.Max.x);
        const ImGuiID region_row = context->ItemInfo("**/...").ID;
        context->MouseMove("**/...");
        context->Yield();
        IM_CHECK(context->ItemExists("**/Show more"));
        context->MouseMove("##graph filter");
        context->Yield();
        context->MouseMove(region_row);
        context->MouseClick();
        context->Yield(2);
        IM_CHECK(application.HistoryExpansionPendingForTest());
        IM_CHECK(context->ItemExists("**/Loading..."));
        application.ApplyEventForTest(BackgroundActivityStarted{84, "Scanning working copy"});
        context->Yield(2);
        context->SetRef("ggui dockspace");
        IM_CHECK(context->ItemExists("Repository activity"));
        IM_CHECK(context->ItemExists("**/Refresh"));
        context->MouseMove("Repository activity");
        context->Yield(2);
        IM_CHECK(GImGui->TooltipPreviousWindow != nullptr);

        auto expanded = std::make_shared<HistoryView>(*initial);
        expanded->request++;
        expanded->items.erase(expanded->items.begin() + 15);
        std::vector<HistoryItem> inserted;
        for (int index = 0; index < 4; ++index)
        {
            const std::string parent = index == 3 ? "hidden-4" : "hidden-" + std::to_string(index + 1);
            HistoryItem item = commit("hidden-" + std::to_string(index), parent);
            item.parents = {index == 3 ? "region:hidden-4:old" : parent};
            inserted.push_back(std::move(item));
        }
        HistoryItem remainder;
        remainder.id = "region:hidden-4:old";
        remainder.kind = HistoryItemKind::CollapsedRegion;
        remainder.parents = {"old"};
        inserted.push_back(std::move(remainder));
        expanded->items.insert(expanded->items.begin() + 15,
            std::make_move_iterator(inserted.begin()), std::make_move_iterator(inserted.end()));
        expanded->items[14].parents = {"hidden-0"};
        application.ApplyEventForTest(HistoryReady{std::move(expanded)});
        context->Yield(5);
        IM_CHECK(!application.HistoryExpansionPendingForTest());
        IM_CHECK(application.HistoryExpansionFeedbackForTest());
        IM_CHECK(!context->ItemExists("**/Commits loaded"));
        IM_CHECK(context->ItemExists("Repository activity"));
        application.ApplyEventForTest(BackgroundActivityFinished{84});
        IM_CHECK_LE(std::fabs((*graph)->Scroll.y - scroll_before), 0.01f);

        context->Yield(2);
        context->SetRef("History");
        IM_CHECK(context->ItemExists("**/Expand merge"));
        context->MouseMoveToPos(context->ItemInfo("**/Expand merge").RectFull.GetCenter());
        context->MouseClick();
        context->Yield(2);
        IM_CHECK(application.HistoryExpansionPendingForTest());
        context->SetRef("ggui dockspace");
        IM_CHECK(context->ItemExists("Repository activity"));
        IM_CHECK(context->ItemExists("**/Refresh"));
    };

    test = IM_REGISTER_TEST(engine, "Workflow", "SubmitEveryDialog");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        context->Yield(3);

        ApplyOpenDialog(context, "//##MainMenuBar/Repository/Clone...");
        context->ItemInputValue("URL", "https://example.test/repository.git");
        context->ItemInputValue("Destination", "/tmp/ggui-submitted-clone");
        context->ItemClick("Apply");
        context->Yield(2);

        ApplyOpenDialog(context, "//##MainMenuBar/Change/Commit...");
        context->ItemInputValue("Description", "submitted commit");
        context->ItemInputValue("Filesets", " modified.txt, added.txt\n");
        context->ItemClick("Apply");
        context->Yield(2);

        for (const char* action : {"Squash...", "Restore...", "Abandon..."})
        {
            const std::string path = std::string("//##MainMenuBar/Change/") + action;
            ApplyOpenDialog(context, path.c_str());
            context->ItemClick("Apply");
            context->Yield(2);
        }
        ApplyOpenDialog(context, "//##MainMenuBar/Change/Rebase...");
        context->ItemInputValue("Destination", "base");
        context->ItemClick("Apply");
        context->Yield(2);
        ApplyOpenDialog(context, "//##MainMenuBar/Change/Split...");
        context->ItemInputValue("Selected filesets", "modified.txt");
        context->ItemClick("Apply");
        context->Yield(2);

        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/Create bookmark");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemInputValue("Name", "submitted-bookmark");
        context->ItemClick("Apply");
        context->Yield(2);

        FocusWindow(context, "Tags");
        context->ItemClick("**/Create tag");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemInputValue("Name", "submitted-tag");
        context->ItemCheck("Allow move");
        context->ItemClick("Apply");
        context->Yield(2);

        FocusWindow(context, "Workspaces");
        context->ItemClick("**/Add workspace");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemInputValue("Destination", "/tmp/ggui-submitted-workspace");
        context->ItemInputValue("Name", "submitted-workspace");
        context->ItemClick("Apply");
        context->Yield(2);

        application.ShowWorkspaceRenameForTest();
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemInputValue("New name", "renamed-workspace");
        context->ItemClick("Apply");
        context->Yield(2);

        for (int action = 0; action < 4; ++action)
        {
            application.ShowDropConfirmationForTest("left", "right", action);
            IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
            context->SetRef("ggui action");
            context->ItemClick("Confirm");
            context->Yield(2);
        }
    };

    test = IM_REGISTER_TEST(engine, "Workflow", "NewPreservesLockedHistoricalEmptyParentAndItsChild");
    test->TestFunc = [](ImGuiTestContext* context) {
        UiRepository repository;
        repository.Git("commit --allow-empty -m 'locked empty parent'");
        repository.Write("child.txt", "existing child\n");
        repository.Git("add child.txt");
        repository.Git("commit -m 'existing child'");
        repository.Git("remote add origin .");
        repository.Git("update-ref refs/remotes/origin/main HEAD^");
        Application& application = Application::Instance();
        application.OpenTestRepository(repository.Path().string());
        std::shared_ptr<const RepoSnapshot> opened;
        for (int attempt = 0; attempt < 200 && opened == nullptr; ++attempt)
        {
            context->Yield();
            const auto snapshot = application.SnapshotForTest();
            if (snapshot != nullptr && std::filesystem::equivalent(snapshot->root, repository.Path()))
                opened = snapshot;
            else
                std::this_thread::sleep_for(5ms);
        }
        IM_CHECK_NE(opened, nullptr);
        if (opened == nullptr)
            return;
        std::vector<Revision> opened_revisions;
        for (int attempt = 0; attempt < 1000; ++attempt)
        {
            opened_revisions = application.HistoryRevisionsForTest();
            if (std::ranges::any_of(opened_revisions, [](const Revision& revision) {
                    return revision.description.starts_with("locked empty parent"); })
                && std::ranges::any_of(opened_revisions, [](const Revision& revision) {
                    return revision.description.starts_with("existing child"); }))
                break;
            context->Yield();
            std::this_thread::sleep_for(5ms);
        }
        const auto parent = std::ranges::find_if(opened_revisions,
            [](const Revision& revision) { return revision.description.starts_with("locked empty parent"); });
        const auto child = std::ranges::find_if(opened_revisions,
            [](const Revision& revision) { return revision.description.starts_with("existing child"); });
        IM_CHECK_NE(parent, opened_revisions.end());
        IM_CHECK_NE(child, opened_revisions.end());
        if (parent == opened_revisions.end() || child == opened_revisions.end()) return;
        IM_CHECK(parent->empty);
        IM_CHECK(parent->pushed);
        const std::string parent_oid = parent->oid;
        const std::string child_oid = child->oid;
        application.SelectRevisionForTest(parent_oid);
        context->MenuClick("//##MainMenuBar/Change/New change");
        std::shared_ptr<const RepoSnapshot> created;
        for (int attempt = 0; attempt < 200 && created == nullptr; ++attempt)
        {
            context->Yield();
            const auto snapshot = application.SnapshotForTest();
            if (snapshot != nullptr && snapshot->generation > opened->generation
                && snapshot->working_copy != opened->working_copy)
                created = snapshot;
            else
                std::this_thread::sleep_for(5ms);
        }
        IM_CHECK(!ActionDialogOpen());
        IM_CHECK_NE(created, nullptr);
        if (created != nullptr)
        {
            IM_CHECK_EQ(created->operations.size(), opened->operations.size() + 1);
            std::vector<Revision> created_revisions;
            for (int attempt = 0; attempt < 1000; ++attempt)
            {
                created_revisions = application.HistoryRevisionsForTest();
                if (std::ranges::any_of(created_revisions, [&](const Revision& revision) {
                        return revision.oid == created->working_copy; }))
                    break;
                context->Yield();
                std::this_thread::sleep_for(5ms);
            }
            IM_CHECK_EQ(created_revisions.size(), opened_revisions.size() + 1);
            const auto preserved_parent = std::ranges::find(created_revisions, parent_oid, &Revision::oid);
            const auto preserved_child = std::ranges::find(created_revisions, child_oid, &Revision::oid);
            const auto added = std::ranges::find(created_revisions, created->working_copy, &Revision::oid);
            IM_CHECK_NE(preserved_parent, created_revisions.end());
            IM_CHECK_NE(preserved_child, created_revisions.end());
            IM_CHECK_NE(added, created_revisions.end());
            IM_CHECK_EQ(preserved_child->parents, std::vector<std::string>{parent_oid});
            IM_CHECK_EQ(added->parents, std::vector<std::string>{parent_oid});
        }
        // Drain the temporary repository's requests before destroying its files.
        context->MenuClick("//##MainMenuBar/Repository/Close repository");
        for (int attempt = 0; attempt < 200 && application.SnapshotForTest() != nullptr; ++attempt)
        {
            context->Yield();
            std::this_thread::sleep_for(5ms);
        }
        context->Yield(2);
        IM_CHECK_EQ(application.SnapshotForTest(), nullptr);
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "StaleMutationDialogsCannotSubmit");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        for (const char* action : {"Metaedit", "Split...", "Squash...", "Abandon...", "ConfirmLocked"})
        {
            RepoSnapshot snapshot = RichSnapshot();
            application.SetSnapshotForTest(snapshot);
            context->Yield(2);
            const bool confirmation = std::string_view(action) == "ConfirmLocked";
            if (confirmation)
            {
                application.SelectRevisionForTest("right");
                context->Yield(2);
                FocusWindow(context, "Change information");
                context->ItemInputValue("**/##commit message", "pending metadata update");
                context->ItemClick("Save message");
            }
            else if (std::string_view(action) == "Metaedit")
            {
                HoverRichSnapshotAuthor(context);
                context->MouseClick(ImGuiMouseButton_Right);
                context->Yield();
                context->SetRef("//$FOCUSED");
                context->ItemClick("**/Edit author");
            }
            else
                context->MenuClick((std::string("//##MainMenuBar/Change/") + action).c_str());
            IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
            context->SetRef("ggui action");
            if (std::string_view(action) == "Metaedit")
                IM_CHECK(context->ItemExists("Author"));
            if (std::string_view(action) == "Split...")
                context->ItemInputValue("Selected filesets", "modified.txt");
            const char* submit = confirmation ? "Confirm" : "Apply";
            context->Yield();
            IM_CHECK((context->ItemInfo(submit).ItemFlags & ImGuiItemFlags_Disabled) == 0);
            ++snapshot.generation;
            application.ApplyEventForTest(SnapshotReady{std::make_shared<RepoSnapshot>(snapshot)});
            context->Yield(2);
            context->SetRef("ggui action");
            IM_CHECK((context->ItemInfo(submit).ItemFlags & ImGuiItemFlags_Disabled) != 0);
            IM_CHECK(ActionDialogOpen());
            context->ItemClick("Cancel");
        }
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "GraphWarningsFollowRewrittenHistory");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        RepoSnapshot snapshot = RichSnapshot();
        snapshot.working_copy = "tip";
        snapshot.head = "tip";
        snapshot.revisions = {
            {"tip", {"left"}, {"change-tip"}, "Tip", "Author", 4, true, false, false},
            {"left", {"base"}, {"change-left"}, "Left", "Author", 3, false, false, false},
            {"right", {"base"}, {"change-right"}, "Right", "Author", 2, false, false, true},
            {"base", {}, {"change-base"}, "Base", "Author", 1, false, false, true},
        };
        application.SetSnapshotForTest(snapshot);
        context->Yield(2);
        application.SelectRevisionForTest("left");
        ApplyOpenDialog(context, "//##MainMenuBar/Change/Restore...");
        context->ItemInputValue("From", "right");
        IM_CHECK(!application.DialogModifiesLockedCommitForTest());
        application.SelectRevisionForTest("right");
        IM_CHECK(!application.DialogModifiesLockedCommitForTest());
        context->ItemClick("Cancel");

        const auto check_drop = [&](const char* source, const char* target, int action, bool warning,
                                    bool entire_branch = false) {
            application.ShowDropConfirmationForTest(source, target, action, entire_branch);
            IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
            context->SetRef("ggui action");
            IM_CHECK_EQ(application.DialogModifiesLockedCommitForTest(), warning);
            context->ItemClick("Cancel");
        };
        check_drop("left", "right", 3, false); // rebase only reads the locked destination
        check_drop("left", "right", 2, true); // squash rewrites its destination
        check_drop("tip", "base", 0, false); // reorder retains the locked base
        check_drop("left", "base", 0, false); // already adjacent in the requested order
        snapshot.revisions.front().pushed = true;
        application.SetSnapshotForTest(snapshot);
        check_drop("left", "right", 3, true); // the locked tip is restacked
        check_drop("left", "base", 0, false); // no-op does not rewrite descendants
        snapshot.revisions.front().pushed = false;
        snapshot.revisions[1].pushed = true;
        snapshot.revisions[2].pushed = false;
        application.SetSnapshotForTest(snapshot);
        check_drop("tip", "right", 2, false);
        check_drop("tip", "right", 2, true, true); // branch squash consumes the locked ancestor
        application.SelectRevisionForTest("right");
        ApplyOpenDialog(context, "//##MainMenuBar/Change/Squash...");
        for (const char* destination : {"@-", "parents(@)", " parents ( @ ) ", "lef"})
        {
            context->ItemInputValue("Into", destination);
            IM_CHECK(application.DialogModifiesLockedCommitForTest());
        }
        context->ItemInputValue("Into", "unsupported-revision");
        context->Yield();
        IM_CHECK((context->ItemInfo("Apply").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->ItemClick("Cancel");
        application.SelectRevisionForTest("left");
        ApplyOpenDialog(context, "//##MainMenuBar/Change/Restore...");
        context->ItemInputValue("From", "right");
        IM_CHECK(application.DialogModifiesLockedCommitForTest());
        ++snapshot.generation;
        application.ApplyEventForTest(SnapshotReady{std::make_shared<RepoSnapshot>(snapshot)});
        context->Yield(2);
        context->SetRef("ggui action");
        IM_CHECK((context->ItemInfo("Apply").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->ItemClick("Cancel");
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "RebaseCurrentPreview");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        RepoSnapshot snapshot = RichSnapshot();
        for (Revision& revision : snapshot.revisions)
            if (revision.oid == "left")
            {
                revision.description = "First source-side change with a deliberately long description line\nbody";
                revision.pushed = true;
            }
        snapshot.revisions.insert(snapshot.revisions.begin(),
            {"tip", {"left"}, {"change-tip"}, "Current source tip", "Tip Author", 6, false, false, false});
        snapshot.working_copy.clear();
        snapshot.head = "tip";
        for (Revision& revision : snapshot.revisions)
            revision.working_copy = false;
        application.SetSnapshotForTest(std::move(snapshot));
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"tip"});
        context->Yield(2);

        ApplyOpenDialog(context, "//##MainMenuBar/Change/Rebase...");
        IM_CHECK_EQ(application.RebaseSourceForTest(), "tip");
        context->ItemInputValue("Destination", "right");
        context->Yield();
        IM_CHECK_EQ(application.RebaseSourceForTest(), "tip");
        IM_CHECK(!application.DialogModifiesLockedCommitForTest());
        IM_CHECK(!context->ItemExists("Rebase entire branch"));
        application.SelectRevisionForTest("left");
        context->Yield();
        IM_CHECK_EQ(application.RebaseSourceForTest(), "tip");
        IM_CHECK(!application.DialogModifiesLockedCommitForTest());
        context->ItemClick("Cancel");
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "HistoryDragTooltipDescribesAction");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        RepoSnapshot snapshot;
        snapshot.generation = 2290;
        snapshot.repository_generation = 2290;
        snapshot.root = "/tmp/ggui-history-drag";
        snapshot.head = "drag-source";
        application.SetSnapshotForTest(std::move(snapshot));
        auto view = std::make_shared<HistoryView>();
        view->repository_generation = 2290;
        view->request = 1000200;
        HistoryItem source_item;
        source_item.id = "drag-source";
        source_item.kind = HistoryItemKind::Commit;
        source_item.parents = {"drop-target"};
        source_item.revision = {"drag-source", {"drop-target"}, {}, "Source description", "Author"};
        HistoryItem target_item;
        target_item.id = "drop-target";
        target_item.kind = HistoryItemKind::Commit;
        target_item.revision = {"drop-target", {}, {}, "Target description", "Author"};
        view->items = {std::move(source_item), std::move(target_item)};
        application.ApplyEventForTest(HistoryReady{std::move(view)});
        context->Yield(4);
        FocusWindow(context, "History");
        context->Yield(2);
        const std::vector<ImGuiID> rows = GatherItems(context, "//History", "row");
        IM_CHECK_GE(rows.size(), 2U);
        if (rows.size() < 2) return;

        const ImGuiTestItemInfo source = context->ItemInfo(rows[0]);
        const ImGuiTestItemInfo target = context->ItemInfo(rows[1]);
        ImGuiWindow* history = ImGui::FindWindowByName("History");
        IM_CHECK_NE(history, nullptr);
        const auto graph = std::ranges::find_if(history->DC.ChildWindows, [](const ImGuiWindow* child) {
            return std::string_view(child->Name).find("graph scroll") != std::string_view::npos;
        });
        IM_CHECK(graph != history->DC.ChildWindows.end());

        float dot_minimum_x = FLT_MAX;
        float dot_maximum_x = -FLT_MAX;
        if (graph != history->DC.ChildWindows.end())
            for (const ImDrawVert& vertex : (*graph)->DrawList->VtxBuffer)
                if (vertex.col == ApplicationInternal::kStatusUnpushed
                    && vertex.pos.y >= source.RectFull.Min.y && vertex.pos.y <= source.RectFull.Max.y)
                {
                    dot_minimum_x = std::min(dot_minimum_x, vertex.pos.x);
                    dot_maximum_x = std::max(dot_maximum_x, vertex.pos.x);
                }
        IM_CHECK(dot_maximum_x >= dot_minimum_x);
        const ImVec2 lane_position{(dot_minimum_x + dot_maximum_x) * 0.5f, source.RectFull.GetCenter().y};
        context->MouseMoveToPos(lane_position);
        context->Yield(2);
        int lane_highlight_vertices = 0;
        int lane_dot_highlight_vertices = 0;
        if (graph != history->DC.ChildWindows.end())
            for (const ImDrawVert& vertex : (*graph)->DrawList->VtxBuffer)
            {
                lane_highlight_vertices += vertex.col == IM_COL32(255, 255, 255, 115);
                lane_dot_highlight_vertices += vertex.col == IM_COL32(255, 255, 255, 180);
            }
        IM_CHECK_GT(lane_highlight_vertices, 0);
        IM_CHECK_EQ(lane_dot_highlight_vertices, 0);

        context->MouseMove(rows[0]);
        context->Yield(2);
        float highlight_minimum_y = FLT_MAX;
        float highlight_maximum_y = -FLT_MAX;
        float dot_highlight_minimum_y = FLT_MAX;
        float dot_highlight_maximum_y = -FLT_MAX;
        if (graph != history->DC.ChildWindows.end())
            for (const ImDrawVert& vertex : (*graph)->DrawList->VtxBuffer)
            {
                if (vertex.col == IM_COL32(255, 255, 255, 115))
                {
                    highlight_minimum_y = std::min(highlight_minimum_y, vertex.pos.y);
                    highlight_maximum_y = std::max(highlight_maximum_y, vertex.pos.y);
                }
                if (vertex.col == IM_COL32(255, 255, 255, 180))
                {
                    dot_highlight_minimum_y = std::min(dot_highlight_minimum_y, vertex.pos.y);
                    dot_highlight_maximum_y = std::max(dot_highlight_maximum_y, vertex.pos.y);
                }
            }
        IM_CHECK_GE(highlight_maximum_y - highlight_minimum_y, ApplicationInternal::kRowHeight);
        IM_CHECK_LT(dot_highlight_maximum_y - dot_highlight_minimum_y, ApplicationInternal::kRowHeight);

        context->MouseMove(rows[0]);
        context->MouseDown();
        context->MouseMoveToPos(source.RectFull.GetCenter() + ImVec2(12.0f, 0.0f));
        context->Yield(2);
        context->MouseMoveToPos(target.RectFull.GetCenter());
        context->Yield(2);

        const ImGuiWindow* tooltip = GImGui->TooltipPreviousWindow;
        IM_CHECK(tooltip != nullptr && tooltip->Active);

        float border_minimum_x = FLT_MAX;
        int border_vertices = 0;
        if (graph != history->DC.ChildWindows.end())
            for (const ImDrawVert& vertex : (*graph)->DrawList->VtxBuffer)
                if (vertex.col == IM_COL32(220, 170, 70, 230))
                {
                    border_minimum_x = std::min(border_minimum_x, vertex.pos.x);
                    ++border_vertices;
                }
        IM_CHECK_GT(border_vertices, 0);
        IM_CHECK_GT(border_minimum_x, target.RectFull.Min.x);
        IM_CHECK_LT(border_minimum_x, target.RectFull.Min.x + 30.0f);

        const std::vector<ImGuiID> dragging_rows = GatherItems(context, "//History", "row");
        for (std::size_t row = 0; row + 1 < dragging_rows.size(); ++row)
            IM_CHECK_LE(std::fabs(context->ItemInfo(dragging_rows[row]).RectFull.Max.y
                - context->ItemInfo(dragging_rows[row + 1]).RectFull.Min.y), 0.01f);

        context->MouseUp();
        IM_CHECK_EQ(application.PendingDropActionForTest(), std::pair(2, false));
        IM_CHECK(!application.PendingDropCopyForTest());
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");

        FocusWindow(context, "History");
        context->MouseMove(rows[0]);
        context->MouseDown();
        context->MouseMoveToPos(source.RectFull.GetCenter() + ImVec2(12.0f, 0.0f));
        context->Yield(2);
        context->MouseMoveToPos(ImVec2(target.RectFull.GetCenter().x, target.RectFull.Max.y - 2.0f));
        context->Yield(2);
        std::size_t last_background_vertex = 0;
        std::size_t first_line_vertex = std::numeric_limits<std::size_t>::max();
        if (graph != history->DC.ChildWindows.end())
            for (int index = 0; index < (*graph)->DrawList->VtxBuffer.Size; ++index)
            {
                const ImU32 color = (*graph)->DrawList->VtxBuffer[index].col;
                if (color == ApplicationInternal::kRowBackground
                    || color == ApplicationInternal::kRowHover
                    || color == ApplicationInternal::kRowSelected)
                    last_background_vertex = static_cast<std::size_t>(index);
                if (color == IM_COL32(100, 175, 255, 255))
                    first_line_vertex = std::min(first_line_vertex, static_cast<std::size_t>(index));
            }
        IM_CHECK(first_line_vertex != std::numeric_limits<std::size_t>::max());
        IM_CHECK_GT(first_line_vertex, last_background_vertex);
        context->MouseUp();
        IM_CHECK_EQ(application.PendingDropActionForTest(), std::pair(1, false));
        IM_CHECK(!application.PendingDropCopyForTest());
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");

        FocusWindow(context, "History");
        context->MouseMove(rows[0]);
        context->MouseDown();
        context->MouseMoveToPos(source.RectFull.GetCenter() + ImVec2(12.0f, 0.0f));
        context->Yield(2);
        context->MouseMoveToPos(ImVec2(target.RectFull.GetCenter().x, target.RectFull.Min.y + 2.0f));
        context->Yield(2);
        float line_minimum_x = FLT_MAX;
        int line_vertices = 0;
        if (graph != history->DC.ChildWindows.end())
            for (const ImDrawVert& vertex : (*graph)->DrawList->VtxBuffer)
                if (vertex.col == IM_COL32(100, 175, 255, 255))
                {
                    line_minimum_x = std::min(line_minimum_x, vertex.pos.x);
                    ++line_vertices;
                }
        IM_CHECK_GT(line_vertices, 0);
        IM_CHECK_LE(line_minimum_x, target.RectFull.Min.x + 20.0f);
        context->MouseUp();
        IM_CHECK_EQ(application.PendingDropActionForTest(), std::pair(0, false));
        IM_CHECK(!application.PendingDropCopyForTest());
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");

        FocusWindow(context, "History");
        context->KeyDown(ImGuiMod_Ctrl);
        context->MouseMove(rows[0]);
        context->MouseDown();
        context->MouseMoveToPos(source.RectFull.GetCenter() + ImVec2(12.0f, 0.0f));
        context->Yield(2);
        context->MouseMoveToPos(ImVec2(target.RectFull.GetCenter().x, target.RectFull.Min.y + 2.0f));
        context->Yield(2);
        IM_CHECK(std::ranges::find(application.SelectedRevisionsForTest(), "drag-source")
            != application.SelectedRevisionsForTest().end());
        context->MouseUp();
        context->KeyUp(ImGuiMod_Ctrl);
        IM_CHECK_EQ(application.PendingDropActionForTest(), std::pair(0, false));
        IM_CHECK(application.PendingDropCopyForTest());
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "GraphAndContextMenus");
    test->TestFunc = [](ImGuiTestContext* context) {
        const float dense_lane_width = ApplicationInternal::HistoryLaneWidth(680.0f, 40);
        IM_CHECK_EQ(dense_lane_width, ApplicationInternal::kLaneWidth);
        IM_CHECK_EQ(ApplicationInternal::HistoryLaneWidth(680.0f, 3),
            ApplicationInternal::kLaneWidth);
        IM_CHECK_LT(ApplicationInternal::HistoryContentOffset(dense_lane_width, 3),
            ApplicationInternal::HistoryContentOffset(dense_lane_width, 40));

        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        context->Yield(4);

        IM_CHECK_EQ(GatherItems(context, "//History", "remote-bookmark").size(), 1U);
        IM_CHECK_EQ(GatherItems(context, "//History", "diverged").size(), 2U);
        IM_CHECK_EQ(GatherItems(context, "//History", "coverage-tag").size(), 1U);
        IM_CHECK_EQ(GatherItems(context, "//History", "remote-tag").size(), 1U);
        IM_CHECK(GatherItems(context, "//History", "origin/remote-bookmark").empty());

        const std::vector<ImGuiID> rows = GatherItems(context, "//History", "row");
        IM_CHECK_GE(rows.size(), 2U);
        for (std::size_t row = 0; row + 1 < rows.size(); ++row)
        {
            const ImGuiTestItemInfo current = context->ItemInfo(rows[row]);
            const ImGuiTestItemInfo next = context->ItemInfo(rows[row + 1]);
            IM_CHECK_LE(current.RectFull.GetHeight(), 36.0f);
            IM_CHECK_LE(std::fabs(current.RectFull.Max.y - next.RectFull.Min.y), 0.01f);
        }
        if (rows.size() >= 3)
        {
            application.SelectRevisionForTest("left");
            context->Yield(2);
            context->SetRef("History");
            context->ItemClick(rows[2], ImGuiMouseButton_Right);
            context->Yield();
            IM_CHECK((context->ItemInfo("**/Rebase...").ItemFlags & ImGuiItemFlags_Disabled) == 0);
            context->ItemClick("**/Rebase...");
            IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
            context->SetRef("ggui action");
            IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"left"});
            IM_CHECK_EQ(application.DialogDestinationForTest(), "right");
            IM_CHECK_EQ(application.RebaseSourceForTest(), "merge");
            IM_CHECK(!context->ItemExists("Rebase entire branch"));
            context->ItemClick("Cancel");
            application.SelectRevisionForTest("merge");
            context->Yield(2);
        }
        if (rows.size() >= 2)
        {
            context->SetRef("Changes");
            context->ItemDragAndDrop("**/M  modified.txt", rows[1]);
            context->Yield(2);

            struct CenterDropCase
            {
                bool alt;
                bool shift;
                int action;
                bool entire_branch;
            };
            constexpr std::array center_drop_cases{
                CenterDropCase{false, false, 2, false},
                CenterDropCase{false, true, 2, true},
                CenterDropCase{true, false, 3, false},
                CenterDropCase{true, true, 3, true},
            };
            for (const CenterDropCase& drop : center_drop_cases)
            {
                if (drop.alt) context->KeyDown(ImGuiMod_Alt);
                if (drop.shift) context->KeyDown(ImGuiMod_Shift);
                context->ItemDragAndDrop(rows[0], rows[1]);
                if (drop.shift) context->KeyUp(ImGuiMod_Shift);
                if (drop.alt) context->KeyUp(ImGuiMod_Alt);
                IM_CHECK_EQ(
                    application.PendingDropActionForTest(), std::pair(drop.action, drop.entire_branch));
                IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
                context->SetRef("ggui action");
                context->ItemClick("Cancel");
                context->Yield(2);
            }

            const ImGuiTestItemInfo source = context->ItemInfo(rows[0]);
            const ImGuiTestItemInfo target = context->ItemInfo(rows[1]);
            context->MouseMove(rows[0]);
            context->MouseDown();
            context->MouseMoveToPos(source.RectFull.GetCenter() + ImVec2(12.0f, 0.0f));
            context->Yield(2);
            context->MouseMoveToPos(ImVec2(target.RectFull.GetCenter().x, target.RectFull.Max.y - 1.0f));
            context->Yield(2);
            context->MouseUp();
            IM_CHECK_EQ(application.PendingDropActionForTest(), std::pair(1, false));
            IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
            context->SetRef("ggui action");
            context->ItemClick("Cancel");
            context->Yield(2);

            context->SetRef("History");
            context->ItemClick(rows[0], ImGuiMouseButton_Right);
            context->Yield();
            IM_CHECK(context->ItemExists("**/New"));
            for (const char* action : {"Create bookmark...", "Short commit ID", "Full commit ID"})
                IM_CHECK(context->ItemExists((std::string("**/") + action).c_str()));
            IM_CHECK(!context->ItemExists("**/Describe..."));
            IM_CHECK(!context->ItemExists("**/Metaedit..."));
            constexpr std::array change_actions{
                "Edit", "Duplicate", "Rebase...", "Squash...", "Abandon...", "Simplify parents"};
            float previous_y = -FLT_MAX;
            for (const char* action : change_actions)
            {
                const ImGuiTestItemInfo item = context->ItemInfo((std::string("**/") + action).c_str());
                IM_CHECK(item.ID != 0);
                IM_CHECK_GT(item.RectFull.Min.y, previous_y);
                previous_y = item.RectFull.Min.y;
            }
            IM_CHECK(!context->ItemExists("**/Abandon branch..."));
            context->KeyDown(ImGuiMod_Shift);
            context->Yield();
            IM_CHECK(!context->ItemExists("**/Squash..."));
            IM_CHECK(context->ItemExists("**/Squash with descendants..."));
            IM_CHECK(!context->ItemExists("**/Abandon..."));
            IM_CHECK(context->ItemExists("**/Abandon branch..."));
            context->KeyUp(ImGuiMod_Shift);
            context->Yield();
            IM_CHECK_LT(context->ItemInfo("**/Simplify parents").RectFull.Min.y,
                context->ItemInfo("**/Create bookmark...").RectFull.Min.y);
            IM_CHECK_LT(context->ItemInfo("**/Create bookmark...").RectFull.Min.y,
                context->ItemInfo("**/Short commit ID").RectFull.Min.y);
            context->ItemClick("**/Duplicate");
            context->Yield();
            IM_CHECK(context->ItemExists("**/Change"));
            IM_CHECK(context->ItemExists("**/Branch"));
            context->KeyPress(ImGuiKey_Escape);
            context->ItemClick("**/Short commit ID");
            context->Yield();
            IM_CHECK_STR_EQ(ImGui::GetClipboardText(), "merge");

            context->SetRef("History");
            context->ItemClick(rows[0], ImGuiMouseButton_Right);
            context->Yield();
            context->ItemClick("**/Create bookmark...");
            IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
            context->SetRef("ggui action");
            context->ItemClick("Cancel");
            context->Yield(2);

            for (const char* action : {"Edit", "Squash...", "Abandon..."})
            {
                context->SetRef("History");
                context->ItemClick(rows[0], ImGuiMouseButton_Right);
                context->Yield();
                const std::string path = std::string("**/") + action;
                context->ItemClick(path.c_str());
                if (std::string_view(action) != "Edit")
                {
                    IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
                    context->SetRef("ggui action");
                    context->ItemClick("Cancel");
                }
                context->Yield(2);
            }

            context->SetRef("History");
            context->KeyDown(ImGuiMod_Shift);
            context->ItemClick(rows[0], ImGuiMouseButton_Right);
            context->Yield();
            IM_CHECK(!context->ItemExists("**/Abandon..."));
            context->ItemClick("**/Abandon branch...");
            IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
            context->SetRef("ggui action");
            IM_CHECK(context->ItemIsChecked("Also abandon all descendants (full branch)"));
            context->ItemClick("Cancel");
            context->KeyUp(ImGuiMod_Shift);
            context->Yield(2);

            context->SetRef("History");
            context->ItemClick(rows[0], ImGuiMouseButton_Right);
            context->Yield();
            IM_CHECK(!context->ItemExists("**/Split..."));
            IM_CHECK(!context->ItemExists("**/Restore..."));
            context->KeyPress(ImGuiKey_Escape);
        }

        context->SetRef("Changes");
        context->ItemClick("**/M  modified.txt", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK(context->ItemExists("**/Delete file"));
        IM_CHECK(context->ItemExists("**/Move to parent"));
        IM_CHECK(context->ItemExists("**/Move to child"));
        IM_CHECK_LT(context->ItemInfo("**/Move to child").RectFull.Min.y,
            context->ItemInfo("**/Move to parent").RectFull.Min.y);
        context->KeyPress(ImGuiKey_Escape);

        application.SelectRevisionForTest("right");
        DiffResult right_diff{1000, "right", "modified.txt", "old\n", "new\n", false, RichSnapshot().status};
        right_diff.patch = "right patch\n";
        application.ApplyEventForTest(DiffReady{std::move(right_diff)});
        context->Yield(2);
        context->SetRef("Changes");
        context->ItemClick("**/M  modified.txt", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK(!context->ItemExists("**/Delete file"));
        IM_CHECK((context->ItemInfo("**/Copy patch").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK((context->ItemInfo("**/Save patch...").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK(context->ItemExists("**/External diff"));
        for (const char* action : {"Revert", "Move to parent", "Move to child"})
            IM_CHECK((context->ItemInfo((std::string("**/") + action).c_str()).ItemFlags
                & ImGuiItemFlags_Disabled) == 0);
        context->ItemClick("**/Copy patch");
        IM_CHECK_STR_EQ(ImGui::GetClipboardText(), "right patch\n");

        context->SetRef("Changes");
        context->ItemClick("**/M  modified.txt", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Save patch...");
        IM_CHECK_NE(WaitForWindow(context, "Save Patch"), nullptr);
        context->SetRef("Save Patch");
        context->ItemClick("Cancel");

        application.SetSnapshotForTest(RichSnapshot());
        application.ApplyEventForTest(
            DiffReady{{1000, "merge", "modified.txt", "old\n", "new\n", false, RichSnapshot().status}});
        context->Yield(2);
        context->SetRef("Changes");
        for (const char* action : {"Revert", "Track", "Untrack"})
        {
            context->ItemClick("**/M  modified.txt", ImGuiMouseButton_Right);
            context->Yield();
            const std::string path = std::string("**/") + action;
            context->ItemClick(path.c_str());
            context->Yield(2);
        }
        context->ItemClick("**/M  modified.txt", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK(!context->ItemExists("**/Mark executable"));
        IM_CHECK(!context->ItemExists("**/Mark non-executable"));
        context->KeyPress(ImGuiKey_Escape);

        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/coverage-bookmark", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK(context->ItemExists("**/Push"));
        IM_CHECK(context->ItemExists("**/Push to..."));
        context->ItemClick("**/Delete");
        context->Yield();
        context->ItemClick("**/Local");
        context->Yield(2);

        FocusWindow(context, "Tags");
        context->ItemClick("**/coverage-tag", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Delete");
        context->Yield(2);

        FocusWindow(context, "Workspaces");
        context->ItemClick("**/stale", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Remove...");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Remove");
        context->Yield(2);
        FocusWindow(context, "Workspaces");
        context->ItemClick("**/current");
        context->ItemClick("**/current", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Rename...");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");
        context->Yield(2);

        context->SetRef("ggui dockspace");
        context->ItemClick("New");
        context->Yield(2);
        IM_CHECK(!ActionDialogOpen());
        context->SetRef("ggui dockspace");
        context->ItemClick("Commit");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");
        context->SetRef("ggui dockspace");
        for (const char* action : {"Prev", "Next", "Undo", "Redo", "Refresh"})
            context->ItemClick(action);

        context->MenuClick("//##MainMenuBar/Change/Edit");
        context->MenuClick("//##MainMenuBar/Change/Simplify parents");
        context->MenuClick("//##MainMenuBar/Edit/Undo");
        context->MenuClick("//##MainMenuBar/Edit/Redo");
        context->KeyPress(ImGuiMod_Ctrl | ImGuiKey_N);
        context->Yield(2);
        IM_CHECK(!ActionDialogOpen());
        context->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
        context->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Y);
        context->KeyPress(ImGuiKey_F5);

        application.SetSnapshotForTest(RichSnapshot());
        application.SelectRevisionForTest("right");
        application.ApplyEventForTest(
            DiffReady{{1000, "right", "modified.txt", "old\n", "new\n", false, RichSnapshot().status}});
        context->Yield(2);
        context->SetRef("Changes");
        context->ItemClick("**/M  modified.txt", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/External diff");
        context->Yield();
        IM_CHECK((context->ItemInfo("**/vs @").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK((context->ItemInfo("**/vs parent").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        ImGui::ClosePopupToLevel(0, true);
        context->Yield();

        SDL_Event event{};
        event.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
        application.ProcessEventForTest(event);
        event = {};
        event.type = SDL_EVENT_DROP_FILE;
        event.drop.data = "dropped-repository";
        application.ProcessEventForTest(event);
        event = {};
        event.type = SDL_EVENT_QUIT;
        application.ProcessEventForTest(event);
        context->Yield(2);
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "StickyFileSelection");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        application.SelectRevisionForTest("merge");
        application.ApplyEventForTest(
            DiffReady{{1000, "merge", "added.txt", {}, "added\n", false, RichSnapshot().status}});
        context->Yield(2);

        context->SetRef("Changes");
        IM_CHECK_EQ(application.SelectedFileForTest(), "added.txt");
        context->ItemClick("**/M  modified.txt");
        context->Yield();
        IM_CHECK_EQ(application.SelectedFileForTest(), "modified.txt");

        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/feature");
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"merge"});
        IM_CHECK_EQ(application.SelectedFileForTest(), "modified.txt");
        // Bookmark toggles filter the graph. Changing the selected commit is
        // a separate action and retains the file until its diff arrives.
        application.SelectRevisionForTest("left");
        IM_CHECK_EQ(application.SelectedFileForTest(), "modified.txt");
        application.ApplyEventForTest(DiffReady{{1000, "left", "fallback.txt", {}, "fallback\n", false,
            {{{}, "fallback.txt", GIT_DELTA_ADDED, false}}}});
        context->Yield(2);
        context->SetRef("Changes");
        IM_CHECK_EQ(application.SelectedFileForTest(), "fallback.txt");

        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/coverage-bookmark");
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"left"});
        IM_CHECK_EQ(application.SelectedFileForTest(), "fallback.txt");
        application.SelectRevisionForTest("merge");
        IM_CHECK_EQ(application.SelectedFileForTest(), "fallback.txt");
        application.ApplyEventForTest(DiffReady{{1000, "merge", "modified.txt", "old\n", "new\n", false,
            {{{}, "fallback.txt", GIT_DELTA_ADDED, false},
                {"modified.txt", "modified.txt", GIT_DELTA_MODIFIED, false}}}});
        context->Yield(2);
        context->SetRef("Changes");
        IM_CHECK_EQ(application.SelectedFileForTest(), "modified.txt");
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "DiffRemainsVisibleWhileLoading");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        application.SelectRevisionForTest("left");
        application.ApplyEventForTest(
            DiffReady{{1000, "left", "modified.txt", "old\n", "new\n", false, RichSnapshot().status}});
        context->Yield(2);

        application.SelectRevisionForTest("right");
        context->Yield(2);
        FocusWindow(context, "Changes");
        context->ItemClick("**/M  modified.txt", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK((context->ItemInfo("**/Revert").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        context->KeyPress(ImGuiKey_Escape);
        FocusWindow(context, "Diff");
        IM_CHECK(context->ItemExists("##diff view"));

        application.ApplyEventForTest(DiffReady{{1000, "right", {}, {}, {}, false, {}}});
        context->Yield(2);
        FocusWindow(context, "Diff");
        IM_CHECK(!context->ItemExists("##diff view"));
        IM_CHECK(!context->ItemExists("Compare with @"));
        IM_CHECK(application.SelectedFileForTest().empty());
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "DiffOptionsAreGlobal");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        application.ApplyEventForTest(
            DiffReady{{1000, "merge", "first.cpp", "old\n", "new\n", false, RichSnapshot().status}});
        context->Yield(2);
        FocusWindow(context, "Diff");
        IM_CHECK_LE(std::fabs(context->ItemInfo("Compare with @").RectFull.GetCenter().y
                            - context->ItemInfo("View").RectFull.GetCenter().y),
            1.0f);
        IM_CHECK_GT(context->ItemInfo("Compare with @").RectFull.Min.x,
            context->ItemInfo("External Diff").RectFull.Max.x);
        const auto expected_combo_width = [](const char* longest_entry) {
            return ImGui::CalcTextSize(longest_entry).x + ImGui::GetStyle().FramePadding.x * 2.0f
                + ImGui::GetFrameHeight();
        };
        IM_CHECK_GE(context->ItemInfo("View").RectFull.GetWidth() + 0.5f,
            expected_combo_width("Side by Side"));
        IM_CHECK_GE(context->ItemInfo("##whitespace mode").RectFull.GetWidth() + 0.5f,
            expected_combo_width("Ignore All Whitespace"));
        IM_CHECK_GE(context->ItemInfo("##context lines").RectFull.GetWidth() + 0.5f,
            expected_combo_width("25 lines"));
        context->SetRef("Diff");
        context->ItemClick("##context lines");
        context->Yield();
        IM_CHECK(context->ItemExists("**/Full"));
        context->KeyPress(ImGuiKey_Escape);
        context->ComboClick("View/Unified");
        context->Yield();
        IM_CHECK(!application.DiffSideBySideForTest());
        application.ApplyEventForTest(
            DiffReady{{1000, "merge", "second.cpp", "before\n", "after\n", false, RichSnapshot().status}});
        context->Yield(2);
        context->SetRef("Diff");
        IM_CHECK(!application.DiffSideBySideForTest());
        context->ComboClick("View/Side by Side");
        context->Yield();
        IM_CHECK(application.DiffSideBySideForTest());

        const auto wheel = [&](const char* item, float amount) {
            context->MouseMove(item);
            context->MouseWheelY(amount);
        };
        wheel("View", -1.0f);
        IM_CHECK(!application.DiffSideBySideForTest());
        wheel("View", 1.0f);
        IM_CHECK(application.DiffSideBySideForTest());

        context->ComboClick("##whitespace mode/Normal");
        context->Yield();
        wheel("##whitespace mode", 1.0f);
        IM_CHECK_EQ(application.DiffWhitespaceModeForTest(), DiffWhitespaceMode::IgnoreAllWhitespace);
        wheel("##whitespace mode", -1.0f);
        IM_CHECK_EQ(application.DiffWhitespaceModeForTest(), DiffWhitespaceMode::Normal);

        context->ComboClick("##context lines/Full");
        context->Yield();
        wheel("##context lines", -1.0f);
        IM_CHECK_EQ(application.DiffContextLinesForTest(), 0);
        wheel("##context lines", 1.0f);
        IM_CHECK_EQ(application.DiffContextLinesForTest(), -1);
        context->ComboClick("##context lines/3 lines");
        context->Yield();
        IM_CHECK_EQ(application.DiffContextLinesForTest(), 3);
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "DiffGapExpansion");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        application.SelectRevisionForTest("merge");
        const int edge = std::max(application.DiffContextLinesForTest(), 1);
        const int hidden = edge * 2 + 100;
        constexpr int tail = 200;
        constexpr int compact = tail + 14;
        const int second = 6 + hidden;
        const auto make_diff = [&] {
            std::string before;
            std::string after;
            for (int line = 0; line < second + tail + 4; ++line)
            {
                const std::string unchanged = "line" + std::to_string(line) + "\n";
                before += line == 2 ? "old-first\n" : line == second + 3 ? "old-second\n" : unchanged;
                after += line == 2 ? "new-first\n" : line == second + 3 ? "new-second\n" : unchanged;
            }
            DiffResult result{1000, "merge", "modified.txt", {}, {}, false, RichSnapshot().status};
            result.options = {application.DiffWhitespaceModeForTest(), application.DiffContextLinesForTest()};
            result.full_before = std::move(before);
            result.full_after = std::move(after);
            for (int line : {0, 1})
                result.lines.push_back({DiffLineKind::Context, line, line, 0});
            result.lines.push_back({DiffLineKind::Deletion, 2, -1, 0});
            result.lines.push_back({DiffLineKind::Addition, -1, 2, 0});
            for (int line : {3, 4, 5})
                result.lines.push_back({DiffLineKind::Context, line, line, 0});
            for (int line : {second, second + 1, second + 2})
                result.lines.push_back({DiffLineKind::Context, line, line, 1});
            result.lines.push_back({DiffLineKind::Deletion, second + 3, -1, 1});
            result.lines.push_back({DiffLineKind::Addition, -1, second + 3, 1});
            for (int line = second + 4; line < second + tail + 4; ++line)
                result.lines.push_back({DiffLineKind::Context, line, line, 1});
            return result;
        };
        application.ApplyEventForTest(DiffReady{make_diff()});
        context->Yield(3);
        FocusWindow(context, "Diff");
        const auto diff_view = [&]() -> ImGuiWindow* {
            const ImGuiTestItemInfo view = context->ItemInfo("##diff view");
            ImGuiWindow* window = ImGui::FindWindowByName("Diff");
            const auto child = std::ranges::find_if(window->DC.ChildWindows,
                [&](const ImGuiWindow* candidate) { return candidate->ChildId == view.ID; });
            return child == window->DC.ChildWindows.end() ? nullptr : *child;
        };
        const auto click_gap = [&](int row, int count, bool shift) {
            if (shift)
            {
                context->KeyDown(ImGuiMod_Shift);
                context->Yield();
            }
            ImGuiWindow* view = diff_view();
            const float line_height = ImGui::GetTextLineHeightWithSpacing();
            const std::string info = std::to_string(count) + " lines hidden";
            const std::string action = "Reveal "
                + std::to_string(shift ? count : std::min(count, edge * 2));
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float info_width = ImGui::CalcTextSize(info.c_str()).x;
            const float button_width = ImGui::CalcTextSize(action.c_str()).x
                + ImGui::GetStyle().FramePadding.x * 2.0f;
            const float left = view->InnerClipRect.GetCenter().x
                - (info_width + spacing + button_width) * 0.5f;
            context->MouseMoveToPos(ImVec2(left + info_width + spacing + button_width * 0.5f,
                view->DC.CursorStartPos.y + (row + 0.5f) * line_height));
            context->MouseClick();
            if (shift)
                context->KeyUp(ImGuiMod_Shift);
            context->Yield(2);
        };
        const float line_height = ImGui::GetTextLineHeightWithSpacing();
        IM_CHECK_LE(std::fabs(diff_view()->ContentSizeExplicit.y - compact * line_height), 0.01f);
        ImGui::SetScrollY(diff_view(), 3.5f * line_height);
        context->Yield(2);
        const float initial_scroll = diff_view()->Scroll.y;
        IM_CHECK_GT(initial_scroll, 0.0f);
        IM_CHECK_GT(std::fmod(initial_scroll, line_height), 0.01f);
        click_gap(7, hidden, true);
        IM_CHECK_LE(std::fabs(diff_view()->ContentSizeExplicit.y - (compact - 1.0f + hidden) * line_height), 0.01f);
        IM_CHECK_LE(std::fabs(diff_view()->Scroll.y - initial_scroll), 0.01f);

        application.NavigateChangedFileForTest(-1);
        context->Yield(2);
        IM_CHECK_LE(std::fabs(diff_view()->ContentSizeExplicit.y - compact * line_height), 0.01f);
        application.NavigateChangedFileForTest(1);
        application.ApplyEventForTest(DiffReady{make_diff()});
        context->Yield(3);
        FocusWindow(context, "Diff");
        IM_CHECK_LE(std::fabs(diff_view()->ContentSizeExplicit.y - compact * line_height), 0.01f);

        click_gap(7, hidden, false);
        IM_CHECK_LE(std::fabs(diff_view()->ContentSizeExplicit.y - (compact + edge * 2.0f) * line_height), 0.01f);
        click_gap(7 + edge, hidden - edge * 2, true);
        IM_CHECK_LE(std::fabs(diff_view()->ContentSizeExplicit.y - (compact - 1.0f + hidden) * line_height), 0.01f);

        application.NavigateChangedFileForTest(-1);
        context->Yield(2);
        IM_CHECK_LE(std::fabs(diff_view()->ContentSizeExplicit.y - compact * line_height), 0.01f);
        application.NavigateChangedFileForTest(1);
        application.ApplyEventForTest(DiffReady{make_diff()});
        context->Yield(3);
        FocusWindow(context, "Diff");
        IM_CHECK_LE(std::fabs(diff_view()->ContentSizeExplicit.y - compact * line_height), 0.01f);
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "DiffLineMoveContextMenu");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        RepoSnapshot snapshot = RichSnapshot();
        snapshot.working_copy = "child";
        snapshot.revisions = {
            {"child", {"source"}, {"change-child"}, "Child", {}, 3, true, false, true},
            {"source", {"base"}, {"change-source"}, "Source", {}, 2, false, false, false},
            {"base", {}, {"change-base"}, "Base", {}, 1, false, false, false},
        };
        snapshot.status = {{"file.txt", "file.txt", GIT_DELTA_MODIFIED, false}};
        application.SetSnapshotForTest(std::move(snapshot));
        application.SelectRevisionForTest("source");
        DiffResult result{1000, "source", "file.txt", "zero\nold\nsame\n", "zero\nnew\nsame\n", false,
            {{"file.txt", "file.txt", GIT_DELTA_MODIFIED, false}}};
        result.old_mode = result.new_mode = GIT_FILEMODE_BLOB;
        result.lines = {
            {DiffLineKind::Context, 39, 49, 0},
            {DiffLineKind::Deletion, 40, -1, 0},
            {DiffLineKind::Addition, -1, 50, 0},
            {DiffLineKind::Context, 41, 51, 0},
            {},
        };
        application.ApplyEventForTest(DiffReady{std::move(result)});
        context->Yield(3);
        FocusWindow(context, "Diff");
        if (application.DiffSideBySideForTest())
        {
            context->ComboClick("View/Unified");
            context->Yield();
        }
        const ImGuiTestItemInfo view = context->ItemInfo("##diff view");
        const float line_height = ImGui::GetTextLineHeightWithSpacing();
        const auto line_position = [&](int line) {
            return ImVec2(view.RectFull.Min.x + 150.0f,
                view.RectFull.Min.y + ImGui::GetStyle().WindowPadding.y + (line + 0.5f) * line_height);
        };
        const auto open_line = [&](int line) {
            context->MouseMoveToPos(line_position(line));
            context->MouseClick(ImGuiMouseButton_Right);
            context->Yield();
            context->SetRef("//$FOCUSED");
        };
        const auto diff_view = [&]() -> ImGuiWindow* {
            ImGuiWindow* window = ImGui::FindWindowByName("Diff");
            const auto child = std::ranges::find_if(window->DC.ChildWindows,
                [&](const ImGuiWindow* candidate) { return candidate->ChildId == view.ID; });
            return child == window->DC.ChildWindows.end() ? nullptr : *child;
        };
        const float glyph_width = ImGui::CalcTextSize("#").x;
        IM_CHECK_GE(diff_view()->ContentSizeExplicit.x, glyph_width * 14.0f);
        IM_CHECK_LT(diff_view()->ContentSizeExplicit.x, glyph_width * 15.0f);
        const auto line_highlight = [&] {
            ImRect bounds(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
            ImGuiWindow* child = diff_view();
            const ImU32 color = ImGui::GetColorU32(ImGuiCol_NavHighlight, 0.28f);
            if (child != nullptr)
                for (const ImDrawVert& vertex : child->DrawList->VtxBuffer)
                    if (vertex.col == color)
                        bounds.Add(vertex.pos);
            return bounds;
        };
        const auto diff_highlight = [&](bool added) {
            ImRect bounds(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
            ImGuiWindow* child = diff_view();
            const std::array colors = added
                ? std::array{IM_COL32(46, 160, 67, 55), IM_COL32(46, 160, 67, 38)}
                : std::array{IM_COL32(248, 81, 73, 55), IM_COL32(248, 81, 73, 38)};
            if (child != nullptr)
                for (const ImDrawVert& vertex : child->DrawList->VtxBuffer)
                    if (std::ranges::find(colors, vertex.col) != colors.end())
                        bounds.Add(vertex.pos);
            return bounds;
        };
        const ImU32 selection_color = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
        IM_CHECK_LT(selection_color >> IM_COL32_A_SHIFT, 128U);
        const auto selection_highlight = [&] {
            ImRect bounds(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
            ImGuiWindow* child = diff_view();
            if (child != nullptr)
                for (const ImDrawVert& vertex : child->DrawList->VtxBuffer)
                    if (vertex.col == selection_color)
                        bounds.Add(vertex.pos);
            return bounds;
        };
        const auto check_selection_highlight = [&] {
            const ImRect bounds = selection_highlight();
            const float offset = (line_height - ImGui::GetTextLineHeight()) * 0.5f;
            IM_CHECK(!bounds.IsInverted());
            IM_CHECK_LE(std::fabs(bounds.Min.y
                                  - (diff_view()->DC.CursorStartPos.y + line_height - offset)),
                0.01f);
            IM_CHECK_LE(std::fabs(bounds.Max.y
                                  - (diff_view()->DC.CursorStartPos.y + 4.0f * line_height - offset)),
                0.01f);
        };
        const auto check_diff_highlight = [&](bool added, int row) {
            const ImRect bounds = diff_highlight(added);
            IM_CHECK(!bounds.IsInverted());
            IM_CHECK_LE(std::fabs(bounds.GetHeight() - line_height), 0.01f);
            IM_CHECK_LE(std::fabs(bounds.GetCenter().y
                                  - (diff_view()->DC.CursorStartPos.y + row * line_height
                                      + ImGui::GetTextLineHeight() * 0.5f)),
                0.01f);
        };

        check_diff_highlight(false, 1);
        check_diff_highlight(true, 2);

        open_line(1);
        IM_CHECK(context->ItemExists("**/Copy"));
        IM_CHECK((context->ItemInfo("**/Copy").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        ImRect highlight = line_highlight();
        IM_CHECK(!highlight.IsInverted());
        IM_CHECK_LE(std::fabs(highlight.GetHeight() - line_height), 0.01f);
        IM_CHECK_LE(std::fabs(highlight.GetCenter().y
                              - (diff_view()->DC.CursorStartPos.y + line_height + ImGui::GetTextLineHeight() * 0.5f)),
            0.01f);
        for (const char* action : {"Move line to child", "Move line to parent", "Move hunk to child",
                 "Move hunk to parent"})
        {
            const std::string path = std::string("**/") + action;
            IM_CHECK(context->ItemExists(path.c_str()));
            IM_CHECK((context->ItemInfo(path.c_str()).ItemFlags & ImGuiItemFlags_Disabled) == 0);
        }
        IM_CHECK(!context->ItemExists("**/Revert line"));
        IM_CHECK(context->ItemExists("**/Revert hunk"));
        IM_CHECK((context->ItemInfo("**/Revert hunk").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK(!context->ItemExists("**/Move lines to child"));
        IM_CHECK(!context->ItemExists("**/Move selection to child"));
        ImGui::ClosePopupToLevel(0, true);
        context->Yield();

        open_line(0);
        IM_CHECK(line_highlight().IsInverted());
        IM_CHECK((context->ItemInfo("**/Move line to child").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("**/Move line to parent").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("**/Move hunk to child").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK((context->ItemInfo("**/Move hunk to parent").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        ImGui::ClosePopupToLevel(0, true);
        context->Yield();

        context->MouseMoveToPos(line_position(1));
        context->MouseDown();
        context->Yield();
        context->MouseMoveToPos(line_position(3));
        context->Yield();
        context->MouseUp();
        context->Yield();
        check_selection_highlight();
        open_line(2);
        IM_CHECK(context->ItemExists("**/Move lines to child"));
        IM_CHECK(context->ItemExists("**/Move lines to parent"));
        IM_CHECK(!context->ItemExists("**/Move line to child"));
        IM_CHECK(!context->ItemExists("**/Move hunk to child"));
        IM_CHECK(!context->ItemExists("**/Move selection to child"));
        IM_CHECK((context->ItemInfo("**/Copy").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        ImGui::SetClipboardText("unchanged");
        context->ItemClick("**/Copy");
        context->Yield();
        IM_CHECK_NE(std::string(ImGui::GetClipboardText()), "unchanged");
        IM_CHECK(std::string_view(ImGui::GetClipboardText()).find("new") != std::string_view::npos);
        open_line(2);
        context->ItemClick("**/Move lines to child");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        const MoveDiffLines& move = application.PendingMoveDiffLinesForTest();
        IM_CHECK_EQ(move.source, "source");
        IM_CHECK_EQ(move.destination, "child");
        IM_CHECK_EQ(move.path, "file.txt");
        IM_CHECK_EQ(move.lines.size(), 2U);
        IM_CHECK_EQ(move.lines[0].kind, DiffLineKind::Deletion);
        IM_CHECK_EQ(move.lines[0].old_line, 40);
        IM_CHECK_EQ(move.lines[0].new_line, -1);
        IM_CHECK_EQ(move.lines[1].kind, DiffLineKind::Addition);
        IM_CHECK_EQ(move.lines[1].old_line, -1);
        IM_CHECK_EQ(move.lines[1].new_line, 50);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");

        context->SetRef("Diff");
        context->ComboClick("View/Side by Side");
        context->Yield();
        check_diff_highlight(false, 1);
        check_diff_highlight(true, 2);
        open_line(1);
        IM_CHECK((context->ItemInfo("**/Copy").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK(context->ItemExists("**/Move hunk to child"));
        IM_CHECK(!context->ItemExists("**/Move lines to child"));
        IM_CHECK(!context->ItemExists("**/Move selection to child"));
        highlight = line_highlight();
        const float split_x = diff_view()->DC.CursorStartPos.x + diff_view()->Size.x * 0.5f;
        IM_CHECK_LE(std::fabs(highlight.Max.x - split_x), 0.01f);
        IM_CHECK_LT(highlight.Min.x, split_x);
        ImGui::ClosePopupToLevel(0, true);
        context->Yield();

        open_line(2);
        highlight = line_highlight();
        IM_CHECK_LE(std::fabs(highlight.Min.x - split_x), 0.01f);
        IM_CHECK_GT(highlight.Max.x, split_x);
        ImGui::ClosePopupToLevel(0, true);
        context->Yield();

        context->MouseMoveToPos(line_position(1));
        context->MouseDown();
        context->Yield();
        context->MouseMoveToPos(line_position(3));
        context->Yield();
        context->MouseUp();
        context->Yield();
        check_selection_highlight();
        open_line(2);
        IM_CHECK(context->ItemExists("**/Move lines to child"));
        IM_CHECK(context->ItemExists("**/Move lines to parent"));
        IM_CHECK(!context->ItemExists("**/Move line to child"));
        IM_CHECK(!context->ItemExists("**/Move hunk to child"));
        IM_CHECK(!context->ItemExists("**/Move selection to child"));
        IM_CHECK((context->ItemInfo("**/Copy").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        ImGui::SetClipboardText("unchanged");
        context->ItemClick("**/Copy");
        context->Yield();
        IM_CHECK_NE(std::string(ImGui::GetClipboardText()), "unchanged");
        IM_CHECK(std::string_view(ImGui::GetClipboardText()).find("same") != std::string_view::npos);
        FocusWindow(context, "Diff");
        ImGui::SetClipboardText("unchanged");
        context->KeyPress(ImGuiMod_Ctrl | ImGuiKey_C);
        context->Yield();
        IM_CHECK_NE(std::string(ImGui::GetClipboardText()), "unchanged");

        application.SelectRevisionForTest("child");
        DiffResult working_result{1000, "child", "file.txt", "zero\nold\nsame\n", "zero\nnew\nsame\n", false,
            {{"file.txt", "file.txt", GIT_DELTA_MODIFIED, false}}};
        working_result.old_mode = working_result.new_mode = GIT_FILEMODE_BLOB;
        working_result.lines = {
            {DiffLineKind::Context, 39, 49, 0},
            {DiffLineKind::Deletion, 40, -1, 0},
            {DiffLineKind::Addition, -1, 50, 0},
            {DiffLineKind::Context, 41, 51, 0},
        };
        application.ApplyEventForTest(DiffReady{std::move(working_result)});
        context->Yield(3);
        context->SetRef("Diff");
        if (application.DiffSideBySideForTest())
        {
            context->ComboClick("View/Unified");
            context->Yield();
        }
        open_line(1);
        for (const char* action : {"Revert line", "Revert hunk"})
        {
            const std::string path = std::string("**/") + action;
            IM_CHECK(context->ItemExists(path.c_str()));
            IM_CHECK((context->ItemInfo(path.c_str()).ItemFlags & ImGuiItemFlags_Disabled) == 0);
        }
        ImGui::ClosePopupToLevel(0, true);
        context->Yield();

        open_line(0);
        IM_CHECK((context->ItemInfo("**/Revert line").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("**/Revert hunk").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        ImGui::ClosePopupToLevel(0, true);
        context->Yield();
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "HistoryHotkeys");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        RepoSnapshot snapshot = RichSnapshot();
        for (int index = 0; index < 40; ++index)
        {
            Revision revision;
            revision.oid = "extra-" + std::to_string(index);
            // Unrelated rows provide scroll range without moving the tested
            // merge/left/right group below a chain of descendants.
            revision.aliases = {"extra-change-" + std::to_string(index)};
            revision.description = "Extra revision " + std::to_string(index);
            snapshot.revisions.push_back(std::move(revision));
        }
        application.SetSnapshotForTest(std::move(snapshot));
        context->Yield(2);
        application.SelectRevisionForTest("left");
        FocusWindow(context, "History");
        ImGuiWindow* history = ImGui::FindWindowByName("History");
        IM_CHECK_NE(history, nullptr);
        const auto graph = std::ranges::find_if(history->DC.ChildWindows, [](const ImGuiWindow* child) {
            return std::string_view(child->Name).find("graph scroll") != std::string_view::npos;
        });
        IM_CHECK(graph != history->DC.ChildWindows.end());
        for (int attempt = 0; attempt < 200 && (*graph)->ScrollMax.y <= 0.0f; ++attempt)
        {
            context->Yield();
            std::this_thread::sleep_for(5ms);
        }
        IM_CHECK_GT((*graph)->ScrollMax.y, 0.0f);
        ImGui::SetScrollY(*graph, 0.0f);
        context->Yield(2);
        const float initial_scroll = (*graph)->Scroll.y;

        context->KeyPress(ImGuiKey_DownArrow);
        context->Yield();
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"right"});
        IM_CHECK_LE(std::fabs((*graph)->Scroll.y - initial_scroll), 0.01f);
        context->KeyPress(ImGuiKey_UpArrow);
        context->Yield();
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"left"});
        IM_CHECK_LE(std::fabs((*graph)->Scroll.y - initial_scroll), 0.01f);
        context->KeyPress(ImGuiKey_UpArrow);
        context->Yield();
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"merge"});
        IM_CHECK_LE(std::fabs((*graph)->Scroll.y - initial_scroll), 0.01f);
        context->KeyPress(ImGuiKey_UpArrow);
        context->Yield();
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"merge"});
        IM_CHECK_LE(std::fabs((*graph)->Scroll.y - initial_scroll), 0.01f);

        application.SelectRevisionForTest("left");
        IM_CHECK_EQ(application.SelectedParentsForTest(), std::vector<std::string>{"left"});

        context->KeyPress(ImGuiKey_E);
        context->KeyPress(ImGuiKey_N);
        context->KeyPress(ImGuiKey_D);
        context->KeyPress(ImGuiMod_Shift | ImGuiKey_D);
        context->Yield(2);
        IM_CHECK(!ActionDialogOpen());

        context->KeyPress(ImGuiKey_S);
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        IM_CHECK(context->ItemExists("Into"));
        IM_CHECK(!RenderedTextContains(context, "Squash this change and all descendants into its parent."));
        IM_CHECK_EQ(application.DialogDescriptionForTest(), "Base\n\nLeft");
        context->ItemClick("Cancel");

        context->KeyPress(ImGuiMod_Shift | ImGuiKey_S);
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        IM_CHECK(!context->ItemExists("Into"));
        IM_CHECK(RenderedTextContains(context, "Squash this change and all descendants into its parent."));
        IM_CHECK(!context->ItemExists("Selected filesets"));
        IM_CHECK_EQ(application.DialogDescriptionForTest(), "Base\n\nLeft\n\nMerge subject\nbody");
        context->ItemClick("Cancel");

        context->KeyPress(ImGuiMod_Alt | ImGuiKey_S);
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        IM_CHECK(context->ItemExists("Selected filesets"));
        IM_CHECK(!context->ItemExists("Into"));
        context->ItemClick("Cancel");

        context->KeyPress(ImGuiKey_A);
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");

        context->KeyPress(ImGuiMod_Shift | ImGuiKey_A);
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        IM_CHECK(context->ItemIsChecked("Also abandon all descendants (full branch)"));
        context->ItemClick("Cancel");

        FocusWindow(context, "Bookmarks");
        context->KeyPress(ImGuiMod_Shift | ImGuiKey_S);
        context->Yield(2);
        IM_CHECK(!ActionDialogOpen());
    };

    test = IM_REGISTER_TEST(engine, "Workflow", "BranchAbandonExcludesRetainedOperationHistory");
    test->TestFunc = [](ImGuiTestContext* context) {
        UiRepository repository;
        const std::string base = repository.RevisionId("HEAD");
        git_repository* raw_git = nullptr;
        IM_CHECK_EQ(git_repository_open(&raw_git, repository.Path().string().c_str()), GIT_OK);
        std::unique_ptr<git_repository, decltype(&git_repository_free)> git(raw_git, git_repository_free);
        gg_repository* raw_gg = nullptr;
        IM_CHECK_EQ(gg_repository_attach(&raw_gg, git.get()), GIT_OK);
        std::unique_ptr<gg_repository, decltype(&gg_repository_free)> gg(raw_gg, gg_repository_free);
        struct Mutation
        {
            gg_mutation_result value{};
            ~Mutation() { gg_mutation_result_dispose(&value); }
        } created, rewritten;
        const char* parents[]{base.c_str()};
        gg_new_options create = GG_NEW_OPTIONS_INIT;
        create.parents = {parents, 1};
        create.message = "Before metadata rewrite";
        IM_CHECK_EQ(gg_repository_new_change(&created.value, gg.get(), &create, nullptr), GIT_OK);
        IM_CHECK(created.value.has_working_copy);
        const std::string old_child = git_oid_tostr_s(&created.value.working_copy);
        const char* selected[]{"@"};
        gg_describe_options describe = GG_DESCRIBE_OPTIONS_INIT;
        describe.revisions = {selected, 1};
        describe.message = "After metadata rewrite";
        IM_CHECK_EQ(gg_repository_describe(&rewritten.value, gg.get(), &describe, nullptr), GIT_OK);
        IM_CHECK(rewritten.value.has_working_copy && rewritten.value.has_operation);
        const std::string current_child = git_oid_tostr_s(&rewritten.value.working_copy);
        IM_CHECK_NE(old_child, current_child);
        // Undo retains the old child beneath operation metadata. The previous
        // refs/* walk included both even though neither belongs to this branch.
        IM_CHECK_EQ(git_graph_descendant_of(git.get(), &rewritten.value.operation,
            &created.value.working_copy), 1);

        Application& application = Application::Instance();
        struct ResetSnapshot
        {
            Application& application;
            ~ResetSnapshot() { application.ClearSnapshotForTest(); }
        } reset{application};
        RepoSnapshot snapshot;
        snapshot.generation = 5900;
        snapshot.root = repository.Path().string();
        snapshot.head = base;
        snapshot.working_copy = current_child;
        snapshot.revisions = {
            {current_child, {base}, {}, "After metadata rewrite", "Author"},
            {base, {}, {}, "Base", "Author"}};
        application.SetSnapshotForTest(std::move(snapshot));
        context->Yield(2);
        auto actual = application.AbandonRevisionsForTest(base);
        std::ranges::sort(actual);
        std::vector<std::string> expected{base, current_child};
        std::ranges::sort(expected);
        IM_CHECK_EQ(actual, expected);
        IM_CHECK_EQ(application.AbandonRevisionsForTest(current_child),
            std::vector<std::string>{current_child});
    };


}

} // namespace Ggui
