// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "Application.hpp"

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
    snapshot.revisions = {
        {"merge", {"left", "right", "third"}, "change-merge", "Merge subject\nbody", "Merger", 5, true, true, false},
        {"left", {"base"}, "change-left", "Left", "Left Author", 4, false, false, false},
        {"right", {"base"}, "change-right", "Right", "Right Author", 3, false, false, true},
        {"third", {"base"}, "change-third", "", "Third Author", 2, false, false, false, true},
        {"base", {}, "change-base", "Base", "Base Author", 1, false, false, true},
    };
    snapshot.refs = {
        {"coverage-bookmark", {}, "merge", GG_NAMED_REF_LOCAL_BOOKMARK, true, true},
        {"feature", {}, "left", GG_NAMED_REF_LOCAL_BOOKMARK, false, false},
        {"coverage-tag", {}, "left", GG_NAMED_REF_LOCAL_TAG, false, false},
        {"remote-bookmark", {}, "right", GG_NAMED_REF_LOCAL_BOOKMARK, true, false},
        {"remote-bookmark", "origin", "right", GG_NAMED_REF_REMOTE_BOOKMARK, true, false},
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

    test = IM_REGISTER_TEST(engine, "Workflow", "CreateEditAndInspectWorkingChange");
    test->TestFunc = [](ImGuiTestContext* context) {
        OpenTestRepo(context);
        const std::string previous_working_copy = Application::Instance().SnapshotForTest()->working_copy;
        context->MenuClick("//##MainMenuBar/Change/New change");
        context->Yield(4);
        IM_CHECK_NE(Application::Instance().SnapshotForTest()->working_copy, previous_working_copy);

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
        for (const char* action : {"Commit...", "Describe...", "Metaedit...", "Rebase...", "Squash...",
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

        snapshot = RichSnapshot();
        snapshot.can_undo = true;
        snapshot.can_redo = false;
        Application::Instance().SetSnapshotForTest(std::move(snapshot));
        context->Yield(2);
        context->SetRef("ggui dockspace");
        IM_CHECK((context->ItemInfo("Undo").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK((context->ItemInfo("Redo").ItemFlags & ImGuiItemFlags_Disabled) != 0);
    };

    test = IM_REGISTER_TEST(engine, "Application", "DialogUsabilityAndCommitScope");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        context->Yield(3);

        ApplyOpenDialog(context, "//##MainMenuBar/Repository/Clone...");
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

        context->MouseMove("Move @ earlier");
        context->Yield();
        context->MouseMove("Move @ later");
        context->Yield();
        const std::string repository_name = Repository().Path().filename().string();
        context->MouseMove(repository_name.c_str());
        context->Yield();
        const ImGuiTestItemInfo repository_button = context->ItemInfo(repository_name.c_str());
        IM_CHECK_GT(repository_button.RectFull.GetWidth(), ImGui::CalcTextSize(repository_name.c_str()).x + 12.0f);
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
        IM_CHECK_EQ(parents[0], "change-left");
        IM_CHECK_EQ(parents[1], "change-right");
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
        IM_CHECK_NE(Application::IdColorForTest(true, false), Application::IdColorForTest(false, false));
        IM_CHECK_NE(Application::IdColorForTest(true, false), Application::IdColorForTest(true, true));
        IM_CHECK_NE(Application::IdColorForTest(false, false), Application::IdColorForTest(false, true));
        IM_CHECK(Application::SupportsDiffLanguageForTest("source.cpp"));
        IM_CHECK(Application::SupportsDiffLanguageForTest("shader.glsl"));
        const std::string limited = Application::LimitLinesForTest(
            "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17", 16);
        IM_CHECK_EQ(static_cast<std::size_t>(std::ranges::count(limited, '\n')), 15U);
        IM_CHECK_EQ(limited, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16...");
        IM_CHECK(!Application::SupportsDiffLanguageForTest("README"));
        IM_CHECK_EQ(Application::FileUrlForTest("/tmp/a b\\c#d"), "file:///tmp/a%20b/c%23d");
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
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"merge"});

        application.SelectRevisionForTest("merge", true);
        IM_CHECK(application.SelectedRevisionsForTest().empty());

        application.SetSnapshotForTest(RichSnapshot());
        application.SelectRevisionForTest("left");
        application.ApplyEventForTest(
            DiffReady{{1000, "left", "modified.txt", "old\n", "new\n", false, RichSnapshot().status}});
        RepoSnapshot rewritten = RichSnapshot();
        rewritten.generation++;
        for (Revision& revision : rewritten.revisions)
            if (revision.oid == "left") revision.oid = "left-rewritten";
        for (NamedRef& ref : rewritten.refs)
            if (ref.target == "left") ref.target = "left-rewritten";
        application.ApplyEventForTest(SnapshotReady{std::make_shared<RepoSnapshot>(std::move(rewritten))});
        IM_CHECK_EQ(application.SelectedRevisionsForTest(), std::vector<std::string>{"left-rewritten"});
        IM_CHECK_EQ(application.SelectedFileForTest(), "modified.txt");

        application.SetSnapshotForTest(RichSnapshot());
        RepoSnapshot empty = RichSnapshot();
        empty.working_copy.clear();
        empty.revisions.clear();
        application.ApplyEventForTest(SnapshotReady{std::make_shared<RepoSnapshot>(std::move(empty))});
        IM_CHECK(application.SelectedRevisionsForTest().empty());
        context->Yield(2);
    };

    test = IM_REGISTER_TEST(engine, "Application", "LockedChangesAndChangeInformation");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        application.SelectRevisionForTest("right");
        context->Yield(3);

        FocusWindow(context, "Change information");
        context->ItemInputValue("**/##commit message", "updated locked message");
        context->ItemClick("Save message");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        IM_CHECK(context->ItemExists("Confirm"));
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
        context->ItemClick("Cancel");

        application.ShowDropConfirmationForTest("left", "right", 0);
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        IM_CHECK(context->ItemExists("Confirm"));
        context->ItemClick("Cancel");
    };

    test = IM_REGISTER_TEST(engine, "Presentation", "RichRepositoryStates");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        for (int index = 0; index < 12; ++index)
            application.AddRecentForTest("recent-" + std::to_string(index));
        context->Yield(4);
        if (std::getenv("GGUI_CAPTURE_MANUAL") != nullptr)
        {
            context->CaptureReset();
            IM_CHECK(context->CaptureScreenshot(ImGuiCaptureFlags_HideMouseCursor));
        }
        application.SetDarkThemeForTest(false);
        context->Yield(2);
        application.SetDarkThemeForTest(true);
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
        context->ItemClick("**/coverage-tag");
        context->Yield(2);
        FocusWindow(context, "Workspaces");
        IM_CHECK(context->ItemExists("**/Add workspace"));
        context->ItemClick("**/current", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK(context->ItemExists("**/Open directory"));
        context->KeyPress(ImGuiKey_Escape);
        FocusWindow(context, "Remotes");
        IM_CHECK(context->ItemExists("**/origin"));
        context->ItemClick("**/origin", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK(context->ItemExists("**/Pull"));
        IM_CHECK(context->ItemExists("**/Fetch"));
        context->KeyPress(ImGuiKey_Escape);
        FocusWindow(context, "Bookmarks");
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

        for (const char* action : {"Describe...", "Metaedit...", "Squash...", "Restore...", "Abandon..."})
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

            for (const char* action : {"Edit", "Describe...", "Split...", "Abandon..."})
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
        }

        context->SetRef("Changes");
        for (const char* action : {"Restore this file", "Track", "Untrack", "Mark executable", "Mark non-executable"})
        {
            context->ItemClick("**/M  modified.txt", ImGuiMouseButton_Right);
            context->Yield();
            const std::string path = std::string("**/") + action;
            context->ItemClick(path.c_str());
            context->Yield(2);
        }

        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/coverage-bookmark", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK(context->ItemExists("**/Push"));
        IM_CHECK(context->ItemExists("**/Push to..."));
        context->ItemClick("**/Delete");
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
        for (const char* action : {"Move @ earlier", "Move @ later", "Undo", "Redo", "Refresh"})
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

    test = IM_REGISTER_TEST(engine, "Interactions", "DiffOptionsAreGlobal");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        application.ApplyEventForTest(
            DiffReady{{1000, "merge", "first.cpp", "old\n", "new\n", false, RichSnapshot().status}});
        context->Yield(2);
        FocusWindow(context, "Diff");
        context->SetRef("Diff");
        context->ItemUncheck("Side by side");
        application.ApplyEventForTest(
            DiffReady{{1000, "merge", "second.cpp", "before\n", "after\n", false, RichSnapshot().status}});
        context->Yield(2);
        context->SetRef("Diff");
        IM_CHECK(!context->ItemIsChecked("Side by side"));
        context->ItemCheck("Side by side");
    };

    test = IM_REGISTER_TEST(engine, "Interactions", "HistoryHotkeys");
    test->TestFunc = [](ImGuiTestContext* context) {
        Application& application = Application::Instance();
        application.SetSnapshotForTest(RichSnapshot());
        context->Yield(2);
        FocusWindow(context, "Bookmarks");
        context->ItemClick("**/feature");
        FocusWindow(context, "History");

        context->KeyPress(ImGuiKey_E);
        context->KeyPress(ImGuiKey_N);
        context->Yield(2);
        IM_CHECK(!ActionDialogOpen());

        context->KeyPress(ImGuiKey_S);
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");

        context->KeyPress(ImGuiKey_A);
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");
    };

}

} // namespace Ggui
