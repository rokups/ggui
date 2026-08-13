// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "Application/Application.hpp"

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

private:
    void Git(const std::string& arguments)
    {
        const std::string command = "git -C " + Quote(_path.string()) + " " + arguments + " >/dev/null 2>&1";
        if (std::system(command.c_str()) != 0)
            throw std::runtime_error("could not prepare UI test repository");
    }

    std::filesystem::path _path;
};

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

void OpenTestRepo(ImGuiTestContext* context)
{
    Application::Instance().OpenTestRepository(Repository().Path().string());
    IM_CHECK_NE(WaitForWindow(context, "History"), nullptr);
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
        {"current", snapshot.root, "merge", false}, {"stale", "/missing/workspace", "left", true}};
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
    };

    test = IM_REGISTER_TEST(engine, "Application", "OpenRepositoryAndPanels");
    test->TestFunc = [](ImGuiTestContext* context) {
        OpenTestRepo(context);
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
        application.SetSnapshotForTest(RichSnapshot());
        context->Yield(2);
        application.ApplyEventForTest(OperationStarted{"open"});
        context->Yield(2);
        IM_CHECK_EQ(application.SnapshotForTest(), nullptr);
        ImGuiWindow* welcome = ImGui::FindWindowByName("Welcome");
        IM_CHECK(welcome != nullptr && welcome->Active);
        context->SetRef("Welcome");
        IM_CHECK((context->ItemInfo("Open repository...").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("Cancel").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        application.ApplyEventForTest(OperationFinished{"open"});
        context->Yield(2);
    };

    test = IM_REGISTER_TEST(engine, "Workflow", "CreateEditAndInspectWorkingChange");
    test->TestFunc = [](ImGuiTestContext* context) {
        OpenTestRepo(context);
        const std::string previous_working_copy = Application::Instance().SnapshotForTest()->working_copy;
        context->MenuClick("//##MainMenuBar/Change/New change");
        context->Yield(4);
        IM_CHECK_NE(Application::Instance().SnapshotForTest()->working_copy, previous_working_copy);
        const std::string empty_working_copy = Application::Instance().SnapshotForTest()->working_copy;
        context->MenuClick("//##MainMenuBar/Change/New change");
        context->Yield(4);
        IM_CHECK_EQ(Application::Instance().SnapshotForTest()->working_copy, empty_working_copy);

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
        OpenTestRepo(context);
        OpenAndCancel(context, "//##MainMenuBar/Repository/Clone...");
        context->MenuClick("//##MainMenuBar/Edit/Apply patch...");
        IM_CHECK_NE(WaitForWindow(context, "Apply Patch"), nullptr);
        context->SetRef("Apply Patch");
        IM_CHECK(context->ItemExists("Apply from Clipboard"));
        IM_CHECK(context->ItemExists("Apply from File"));
        context->ItemClick("Cancel");
        for (const char* action : {"Commit...", "Metaedit...", "Rebase...", "Squash...",
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
        IM_CHECK(context->ItemExists("User"));
        IM_CHECK(context->ItemExists("##Maximum new file size User"));
        IM_CHECK(!context->ItemExists("##Maximum new file size Repository"));
        IM_CHECK((context->ItemInfo("Repository").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((context->ItemInfo("Workspace").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        IM_CHECK(context->ItemExists("**/Open a repository to configure Repository and Workspace overrides."));

        const int original_scale = static_cast<int>(std::lround(ImGui::GetStyle().FontSizeBase / 16.0f * 100.0f));
        context->ItemInputValue("UI scale", 125);
        context->Yield();
        IM_CHECK_LE(std::fabs(ImGui::GetStyle().FontSizeBase - 20.0f), 0.001f);
        IM_CHECK_LE(std::fabs(ImGui::GetStyle().FontScaleMain - 1.0f), 0.001f);
        IM_CHECK_LE(std::fabs(ImGui::GetFontBaked()->Size - ImGui::GetFontSize()), 0.001f);
        context->ItemInputValue("UI scale", original_scale);
        context->ItemInputValue("##Maximum new file size User", "invalid");
        context->Yield();
        IM_CHECK(context->ItemExists("**/Enter unsigned bytes or a binary size such as 1MiB."));
        context->WindowClose("Settings");
        context->Yield();
        IM_CHECK(!ImGui::FindWindowByName("Settings")->Active);

        OpenTestRepo(context);
        WriteMaxNewFileSizeValue(Repository().Path(), ConfigScope::Repository, std::nullopt);
        application.RefreshForTest();
        context->Yield(3);
        const std::uint64_t generation = application.SnapshotForTest()->generation;
        context->MenuClick("//##MainMenuBar/Repository/Settings...");
        IM_CHECK_NE(WaitForWindow(context, "Settings"), nullptr);
        context->SetRef("Settings");
        IM_CHECK(context->ItemExists("##Maximum new file size User"));
        context->ItemClick("Repository");
        context->Yield();
        IM_CHECK(context->ItemExists("##Maximum new file size Repository"));
        context->ItemInputValue("##Maximum new file size Repository", "2MiB");
        context->ItemClick("Workspace");
        for (int frame = 0; frame < 100 && application.SnapshotForTest()->generation <= generation; ++frame)
            context->Yield();
        IM_CHECK_GT(application.SnapshotForTest()->generation, generation);
        const MaxNewFileSizeValues written = ReadMaxNewFileSizeValues(Repository().Path());
        IM_CHECK_EQ(written[1], "2MiB");

        context->SetRef("Settings");
        context->ItemClick("Repository");
        context->Yield();
        context->ItemInputValue("##Maximum new file size Repository", "");
        context->ItemClick("User");
        context->Yield(2);
        IM_CHECK(!ReadMaxNewFileSizeValues(Repository().Path())[1].has_value());
        context->WindowClose("Settings");
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
        {
            IM_CHECK(context->ItemExists(action));
            IM_CHECK((context->ItemInfo(action).ItemFlags & ImGuiItemFlags_Disabled) == 0);
        }

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
            IM_CHECK((context->ItemInfo(action).ItemFlags & ImGuiItemFlags_Disabled) != 0);
    };

    test = IM_REGISTER_TEST(engine, "Application", "ActionsLockDuringOperation");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        application.ApplyEventForTest(OperationStarted{"locked operation"});
        context->Yield(3);

        context->SetRef("ggui dockspace");
        for (const char* action : {"New", "Commit", "Prev", "Next", "Undo", "Redo",
                 "Refresh", "Pull", "Fetch", "Push", "Push to..."})
        {
            IM_CHECK(context->ItemExists(action));
            IM_CHECK((context->ItemInfo(action).ItemFlags & ImGuiItemFlags_Disabled) != 0);
        }
        IM_CHECK((context->ItemInfo("Cancel").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK_GE(context->ItemInfo("Cancel").RectFull.GetHeight(),
            context->ItemInfo("Refresh").RectFull.GetHeight() - 1.0f);

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
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"left"});
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
        IM_CHECK((context->ItemInfo("**/Forget").ItemFlags & ImGuiItemFlags_Disabled) != 0);
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

    test = IM_REGISTER_TEST(engine, "Navigation", "RevealCommit");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        RepoSnapshot snapshot = RichSnapshot();
        snapshot.generation++;
        snapshot.revisions.clear();
        for (int index = 0; index < 80; ++index)
        {
            Revision revision;
            revision.oid = "revision-" + std::to_string(index);
            if (index + 1 < 80) revision.parents.push_back("revision-" + std::to_string(index + 1));
            revision.aliases = {"change-" + std::to_string(index)};
            revision.description = "Revision " + std::to_string(index);
            revision.author = "Author";
            snapshot.revisions.push_back(std::move(revision));
        }
        snapshot.working_copy = "revision-0";
        snapshot.refs = {{"deep-bookmark", {}, "revision-65", GG_NAMED_REF_LOCAL_BOOKMARK, false, false},
            {"deep-tag", {}, "revision-60", GG_NAMED_REF_LOCAL_TAG, false, false}};
        snapshot.status.clear();
        application.SetSnapshotForTest(std::move(snapshot));
        context->Yield(3);

        FocusWindow(context, "History");
        context->ItemInputValue("##graph filter", "does-not-match");
        context->Yield(2);
        IM_CHECK(GatherItems(context, "//History", "row").empty());

        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/deep-bookmark", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Reveal commit");
        context->Yield(3);
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"revision-65"});
        ImGuiWindow* history = ImGui::FindWindowByName("History");
        IM_CHECK_NE(history, nullptr);
        const auto graph = std::ranges::find_if(history->DC.ChildWindows, [](const ImGuiWindow* child) {
            return std::string_view(child->Name).find("graph scroll") != std::string_view::npos;
        });
        IM_CHECK(graph != history->DC.ChildWindows.end());
        IM_CHECK_GT((*graph)->Scroll.y, 0.0f);

        FocusWindow(context, "Tags");
        context->ItemClick("**/deep-tag", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Reveal commit");
        context->Yield(2);
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"revision-60"});
    };

    test = IM_REGISTER_TEST(engine, "Application", "PushBookmarkDialog");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application::Instance().SetSnapshotForTest(RichSnapshot());
        context->Yield(2);
        context->SetRef("ggui dockspace");
        context->ItemClick("Push to...");
        ImGuiWindow* dialog = WaitForWindow(context, "ggui action");
        IM_CHECK_NE(dialog, nullptr);
        context->SetRef("ggui action");
        IM_CHECK_EQ(GImGui->NavId, context->ItemInfo("Remote").ID);
        IM_CHECK(context->ItemExists("Force push"));
        IM_CHECK(!context->ItemExists("**/Warning: force push can overwrite remote history."));
        context->ItemClick("Force push");
        context->Yield();
        IM_CHECK(context->ItemExists("**/Warning: force push can overwrite remote history."));
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
        IM_CHECK((context->ItemInfo("New").ItemFlags & ImGuiItemFlags_Disabled) != 0);

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
        context->ItemClick("New");
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
            {GIT_DELTA_UNTRACKED, "?"}, {GIT_DELTA_IGNORED, "I"}, {GIT_DELTA_CONFLICTED, "!"},
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
        IM_CHECK_EQ(Application::ClosestBookmarkForTest(snapshot, "merge"), "coverage-bookmark");
        IM_CHECK_EQ(Application::ClosestBookmarkForTest(snapshot, "third"), "upstream/remote-only");
        IM_CHECK(Application::ClosestBookmarkForTest(snapshot, "missing").empty());
        IM_CHECK_EQ(Application::ReferenceBadgeLabelForTest(snapshot.refs[5], snapshot.refs),
            std::make_pair(std::string("origin/remote-bookmark"), std::size_t{7}));
        IM_CHECK(Application::ReferenceBadgeLabelForTest(snapshot.refs[6], snapshot.refs).first.empty());
        IM_CHECK_EQ(Application::ReferenceBadgeLabelForTest(snapshot.refs[8], snapshot.refs).first, "diverged");
        IM_CHECK_EQ(
            Application::ReferenceBadgeLabelForTest(snapshot.refs[9], snapshot.refs).first, "origin/diverged");
        const std::array bookmark_colors{
            Application::BookmarkColorForTest("coverage-bookmark", snapshot.refs),
            Application::BookmarkColorForTest("remote-only", snapshot.refs),
            Application::BookmarkColorForTest("remote-bookmark", snapshot.refs),
            Application::BookmarkColorForTest("diverged", snapshot.refs)};
        for (std::size_t left = 0; left < bookmark_colors.size(); ++left)
            for (std::size_t right = left + 1; right < bookmark_colors.size(); ++right)
                IM_CHECK_NE(bookmark_colors[left], bookmark_colors[right]);
        IM_CHECK_EQ(Application::FormatTimestampForTest(0), "Unknown date");
        IM_CHECK_EQ(Application::FormatTimestampForTest(1'700'000'000).size(), 16U);
        IM_CHECK_EQ(Application::DropPlacementForTest(0), GG_REORDER_AFTER);
        IM_CHECK_EQ(Application::DropPlacementForTest(1), GG_REORDER_BEFORE);
        IM_CHECK_EQ(Application::DropTooltipForTest(0, "target"), "Move before target");
        IM_CHECK_EQ(Application::DropTooltipForTest(1, "target"), "Move after target");
        IM_CHECK_EQ(Application::DropTooltipForTest(2, "target"), "Squash into target");
        IM_CHECK_EQ(Application::DropTooltipForTest(3, "target"), "Rebase onto target");
        IM_CHECK_NE(ImGui::GetFontBaked()->FindGlyphNoFallback(0xf097), nullptr);
        IM_CHECK(std::string_view(ICON_MS_EDIT).size() > 1);
    };

    test = IM_REGISTER_TEST(engine, "Application", "SelectionSurvivesSnapshotRefresh");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();

        application.SetSnapshotForTest(RichSnapshot());
        application.SelectRevisionForTest("left");
        application.SelectRevisionForTest("right", true);
        RepoSnapshot without_right = RichSnapshot();
        std::erase_if(without_right.revisions, [](const Revision& revision) { return revision.oid == "right"; });
        application.ApplyEventForTest(SnapshotReady{std::make_shared<RepoSnapshot>(std::move(without_right))});
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"left"});

        application.SetSnapshotForTest(RichSnapshot());
        application.SelectRevisionForTest("left");
        RepoSnapshot without_left = RichSnapshot();
        std::erase_if(without_left.revisions, [](const Revision& revision) { return revision.oid == "left"; });
        application.ApplyEventForTest(SnapshotReady{std::make_shared<RepoSnapshot>(std::move(without_left))});
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"merge"});

        application.SetSnapshotForTest(RichSnapshot());
        RepoSnapshot without_working_copy = RichSnapshot();
        without_working_copy.working_copy.clear();
        for (Revision& revision : without_working_copy.revisions)
            revision.working_copy = false;
        application.ApplyEventForTest(
            SnapshotReady{std::make_shared<RepoSnapshot>(std::move(without_working_copy))});
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"left"});

        application.SelectRevisionForTest("left", true);
        IM_CHECK(application.SelectedRevisionsForTest().empty());

        application.SetSnapshotForTest(RichSnapshot());
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
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"left-rewritten"});
        IM_CHECK_EQ(application.SelectedFileForTest(), "modified.txt");

        application.SetSnapshotForTest(RichSnapshot());
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
        const std::vector<std::string> branch = application.AbandonRevisionsForTest("base");
        IM_CHECK_EQ(branch.size(), 5U);
        for (const char* revision : {"base", "left", "right", "third", "merge"})
            IM_CHECK(std::ranges::find(branch, revision) != branch.end());
        application.SelectRevisionForTest("merge");
        context->Yield(2);
        FocusWindow(context, "Change information");
        ImGuiWindow* change_information = ImGui::FindWindowByName("Change information");
        IM_CHECK_NE(change_information, nullptr);
        const ImVec2 author_position = change_information->DC.CursorStartPos
            + ImVec2(20.0f, ImGui::GetTextLineHeight() * 0.5f);
        context->MouseMoveToPos(author_position);
        context->Yield(2);
        IM_CHECK(GImGui->TooltipPreviousWindow != nullptr);
        context->MouseClick(ImGuiMouseButton_Right);
        context->Yield();
        for (const char* action : {"Copy author", "Copy email", "Edit author"})
            IM_CHECK(context->ItemExists((std::string("**/") + action).c_str()));
        context->ItemClick("**/Copy email");
        IM_CHECK_STR_EQ(ImGui::GetClipboardText(), "merger@example.test");
        context->SetRef("Change information");
        context->MouseMoveToPos(author_position);
        context->MouseClick(ImGuiMouseButton_Right);
        context->Yield();
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
        const std::string repository_name = Repository().Path().filename().string();
        context->ComboClick(("Repository/" + repository_name).c_str());
        context->Yield();
        context->SetRef("//$FOCUSED");
        IM_CHECK(context->ItemExists("##recent repository filter"));
        context->ItemInputValue("##recent repository filter", "secret-parent");
        context->Yield();
        IM_CHECK(context->ItemExists("No matching repositories."));
        context->ItemInputValue("##recent repository filter", "east/unique");
        context->Yield();
        IM_CHECK(!context->ItemExists("No matching repositories."));
        IM_CHECK_EQ(GatherItems(context, "//$FOCUSED", "repository").size(), 1U);
        context->ItemInputValue("##recent repository filter", "");
        context->KeyPress(ImGuiKey_Escape);

        context->SetRef("##MainMenuBar");
        context->ItemClick("Repository");
        context->Yield();
        context->ItemClick("**/Recent");
        context->Yield();
        context->SetRef("//$FOCUSED");
        IM_CHECK(context->ItemExists("##recent repository filter"));
        context->KeyPress(ImGuiKey_Escape);
        context->KeyPress(ImGuiKey_Escape);
    };

    test = IM_REGISTER_TEST(engine, "Presentation", "RichRepositoryStates");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        for (int index = 0; index < 12; ++index)
            application.AddRecentForTest("recent-" + std::to_string(index));
        context->Yield(4);
        context->SetRef("ggui dockspace");
        const std::string repository_name = Repository().Path().filename().string();
        const ImGuiTestItemInfo repository_combo = context->ItemInfo("Repository");
        const ImGuiTestItemInfo folder_button = context->ItemInfo("Open repository folder");
        IM_CHECK_GT(folder_button.RectFull.Min.x, repository_combo.RectFull.Max.x);
        IM_CHECK_LT(folder_button.RectFull.GetWidth(), ImGui::CalcTextSize("Open repository folder").x);
        IM_CHECK(context->ItemExists("**/merge"));
        IM_CHECK(context->ItemExists("**/coverage-bookmark"));
        RepoSnapshot without_working_copy = RichSnapshot();
        without_working_copy.working_copy.clear();
        application.SetSnapshotForTest(std::move(without_working_copy));
        context->Yield(2);
        context->SetRef("ggui dockspace");
        IM_CHECK(context->ItemExists("**/left"));
        IM_CHECK(context->ItemExists("**/feature"));
        context->ComboClick(("Repository/" + repository_name).c_str());
        context->Yield();
        context->SetRef("//$FOCUSED");
        IM_CHECK(context->ItemExists("**/recent-11"));
        IM_CHECK((context->ItemInfo("**/recent-11").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        context->ItemClick("**/recent-11");
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

        context->MenuClick("//##MainMenuBar/Repository/Recent/recent-11");
        application.ApplyEventForTest(ErrorEvent{"coverage", "welcome error"});
        application.ClearSnapshotForTest();
        context->Yield(3);
        context->SetRef("Welcome");
        context->ItemClick("**/recent-11");
        application.SetSnapshotForTest(RichSnapshot());
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
        application.SetSnapshotForTest(RichSnapshot());
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
        RepoSnapshot snapshot = RichSnapshot();
        snapshot.refs.push_back({bookmark, {}, "merge", GG_NAMED_REF_LOCAL_BOOKMARK, false, false});
        snapshot.refs.push_back({tag, {}, "left", GG_NAMED_REF_LOCAL_TAG, false, false});
        snapshot.workspaces.push_back({workspace, "/root/" + long_text, "merge", false});
        snapshot.remotes.push_back({remote, "https://fetch.example/" + long_text,
            "ssh://push.example/" + long_text});
        snapshot.status = {{{}, path, GIT_DELTA_MODIFIED, false}};
        application.SetSnapshotForTest(std::move(snapshot));
        context->Yield(3);

        const auto check_tooltip = [&](const char* window, const std::string& item) {
            FocusWindow(context, window);
            context->MouseMove(("**/" + item).c_str());
            context->Yield(2);
            const ImGuiWindow* tooltip = GImGui->TooltipPreviousWindow;
            IM_CHECK(tooltip != nullptr && (tooltip->Active || tooltip->WasActive));
        };
        check_tooltip("Bookmarks", bookmark);
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
            if (std::string_view(item->DebugLabel).starts_with("###M  "))
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
                            - context->ItemInfo("**/A  added.txt").RectFull.Max.x),
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
        IM_CHECK((context->ItemInfo("**/Open working-copy file").ItemFlags & ImGuiItemFlags_Disabled) != 0);
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
        context->ItemClick(rows.front(), ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Copy");
        context->Yield();
        context->ItemClick("**/Full description");
        IM_CHECK_EQ(std::string(ImGui::GetClipboardText()), "Merge subject\nbody");
    };

    test = IM_REGISTER_TEST(engine, "Application", "CloseRepositoryAndRenameBookmark");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        application.AddRecentForTest("retained-repository");
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
        Application::Instance().SetSnapshotForTest(RichSnapshot());
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
        RepoSnapshot snapshot = RichSnapshot();
        snapshot.refs.push_back(
            {"diverged", "upstream", "third", GG_NAMED_REF_REMOTE_BOOKMARK, true, false});
        IM_CHECK_EQ(
            ClassifyBookmarkRelation(snapshot, "left", "right"), BookmarkRelation::Diverged);
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

        for (const char* action : {"Metaedit...", "Squash...", "Restore...", "Abandon..."})
        {
            const std::string path = std::string("//##MainMenuBar/Change/") + action;
            ApplyOpenDialog(context, path.c_str());
            context->ItemClick("Apply");
            context->Yield(2);
        }
        ApplyOpenDialog(context, "//##MainMenuBar/Change/Rebase...");
        context->ItemInputValue("Destination", "base");
        context->ItemCheck("Rebase entire branch");
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

    test = IM_REGISTER_TEST(engine, "Interactions", "RebaseBranchPreview");
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
        context->ItemCheck("Rebase entire branch");
        context->Yield();
        IM_CHECK_EQ(application.RebaseSourceForTest(), "left");
        IM_CHECK(application.DialogModifiesLockedCommitForTest());
        context->ItemClick("Cancel");
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "GraphAndContextMenus");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        context->Yield(4);

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
            IM_CHECK(context->ItemExists("Rebase entire branch"));
            IM_CHECK(!context->ItemIsChecked("Rebase entire branch"));
            context->ItemCheck("Rebase entire branch");
            context->ItemClick("Cancel");
            application.SelectRevisionForTest("merge");
            context->Yield(2);
        }
        if (rows.size() >= 2)
        {
            context->SetRef("Changes");
            context->ItemDragAndDrop("**/M  modified.txt", rows[1]);
            context->Yield(2);

            context->ItemDragAndDrop(rows[0], rows[1]);
            IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
            context->SetRef("ggui action");
            context->ItemClick("Cancel");
            context->Yield(2);

            for (const char* action : {"Move before", "Move after", "Squash", "Rebase"})
            {
                context->ItemDragAndDrop(rows[0], rows[1], ImGuiMouseButton_Right);
                context->Yield();
                context->SetRef("//$FOCUSED");
                const std::string path = std::string("**/") + action;
                IM_CHECK(context->ItemExists(path.c_str()));
                context->ItemClick(path.c_str());
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
            IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
            context->SetRef("ggui action");
            context->ItemClick("Cancel");
            context->Yield(2);

            const std::vector<ImGuiID> endings = GatherItems(context, "//History", "move to end");
            IM_CHECK_EQ(endings.size(), 1U);
            if (!endings.empty())
            {
                context->ItemDragAndDrop(rows[0], endings[0]);
                IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
                context->SetRef("ggui action");
                context->ItemClick("Cancel");
                context->Yield(2);

                context->ItemDragAndDrop(rows[0], endings[0], ImGuiMouseButton_Right);
                context->Yield();
                context->SetRef("//$FOCUSED");
                context->ItemClick("**/Move after");
                IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
                context->SetRef("ggui action");
                context->ItemClick("Cancel");
                context->Yield(2);
            }

            context->SetRef("History");
            context->ItemClick(rows[0], ImGuiMouseButton_Right);
            context->Yield();
            IM_CHECK(context->ItemExists("**/New"));
            const ImGuiTestItemInfo push_item = context->ItemInfo("**/Push");
            const ImGuiTestItemInfo push_to_item = context->ItemInfo("**/Push to...");
            IM_CHECK_GE(push_to_item.RectFull.Min.y - push_item.RectFull.Min.y,
                ImGui::GetTextLineHeight() + 4.0f);
            for (const char* action : {"Push", "Push to...", "Create bookmark...", "Move bookmark here",
                     "Delete bookmark", "Copy"})
                IM_CHECK(context->ItemExists((std::string("**/") + action).c_str()));
            IM_CHECK(!context->ItemExists("**/Describe..."));
            constexpr std::array change_actions{"Metaedit...", "Edit", "Duplicate", "Rebase...", "Squash...", "Split...",
                "Restore...", "Abandon...", "Simplify parents"};
            float previous_y = -FLT_MAX;
            for (const char* action : change_actions)
            {
                const ImGuiTestItemInfo item = context->ItemInfo((std::string("**/") + action).c_str());
                IM_CHECK(item.ID != 0);
                IM_CHECK_GT(item.RectFull.Min.y, previous_y);
                previous_y = item.RectFull.Min.y;
            }
            context->ItemClick("**/Duplicate");
            context->Yield();
            IM_CHECK(context->ItemExists("**/Change"));
            IM_CHECK(context->ItemExists("**/Branch"));
            context->KeyPress(ImGuiKey_Escape);
            context->ItemClick("**/Copy");
            context->Yield();
            for (const char* action : {"Short commit ID", "Full commit ID", "Short alias 1", "Full alias 1"})
                IM_CHECK(context->ItemExists((std::string("**/") + action).c_str()));
            context->ItemClick("**/Short alias 1");
            context->Yield();
            IM_CHECK_STR_EQ(ImGui::GetClipboardText(), "change-m");

            context->SetRef("History");
            context->ItemClick(rows[0], ImGuiMouseButton_Right);
            context->Yield();
            context->ItemClick("**/Create bookmark...");
            IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
            context->SetRef("ggui action");
            context->ItemClick("Cancel");
            context->Yield(2);

            for (const char* action : {"Metaedit...", "Edit", "Squash...", "Split...", "Restore...",
                     "Abandon..."})
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
            context->ItemClick(rows[0], ImGuiMouseButton_Right);
            context->Yield();
            IM_CHECK(context->ItemExists("**/Split..."));
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
        context->ItemClick("**/Forget");
        context->Yield(2);
        FocusWindow(context, "Workspaces");
        context->ItemClick("**/current");
        context->ItemClick("**/current", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Rename current...");
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
        IM_CHECK_EQ(application.SelectedFileForTest(), "modified.txt");
        application.ApplyEventForTest(DiffReady{{1000, "left", "fallback.txt", {}, "fallback\n", false,
            {{{}, "fallback.txt", GIT_DELTA_ADDED, false}}}});
        context->Yield(2);
        context->SetRef("Changes");
        IM_CHECK_EQ(application.SelectedFileForTest(), "fallback.txt");

        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/coverage-bookmark");
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
            revision.aliases = {"extra-change-" + std::to_string(index)};
            revision.description = "Extra revision " + std::to_string(index);
            snapshot.revisions.push_back(std::move(revision));
        }
        application.SetSnapshotForTest(std::move(snapshot));
        context->Yield(2);
        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/feature");
        FocusWindow(context, "History");
        ImGuiWindow* history = ImGui::FindWindowByName("History");
        IM_CHECK_NE(history, nullptr);
        const auto graph = std::ranges::find_if(history->DC.ChildWindows, [](const ImGuiWindow* child) {
            return std::string_view(child->Name).find("graph scroll") != std::string_view::npos;
        });
        IM_CHECK(graph != history->DC.ChildWindows.end());
        IM_CHECK_GT((*graph)->ScrollMax.y, 0.0f);
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
        context->Yield(2);
        IM_CHECK(!ActionDialogOpen());

        context->KeyPress(ImGuiKey_S);
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        IM_CHECK(context->ItemExists("Into"));
        IM_CHECK(!context->ItemExists("Selected filesets"));
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
    };

}

} // namespace Ggui
