// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "Application.hpp"

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
        {"third", {"base"}, "change-third", "", "Third Author", 2, false, false, false},
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
    snapshot.conflicts = {{"conflict file.txt", 2, 3}};
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
        for (const char* panel : {"Navigator", "History", "Changes", "Diff", "Operations"})
        {
            ImGuiWindow* window = WaitForWindow(context, panel);
            IM_CHECK_NE(window, nullptr);
            IM_CHECK(window->Active);
        }
    };

    test = IM_REGISTER_TEST(engine, "Workflow", "CreateEditAndInspectWorkingChange");
    test->TestFunc = [](ImGuiTestContext* context) {
        OpenTestRepo(context);
        context->MenuClick("//##MainMenuBar/Change/New...");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemInputValue("Description", "work");
        context->ItemClick("Apply");
        context->Yield(4);

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
        for (const char* action : {"New...", "Commit...", "Describe...", "Metaedit...", "Rebase...", "Squash...",
                 "Split...", "Restore...", "Abandon..."})
        {
            const std::string path = std::string("//##MainMenuBar/Change/") + action;
            OpenAndCancel(context, path.c_str());
        }

        context->MenuClick("//##MainMenuBar/View/Dark theme");
        context->MenuClick("//##MainMenuBar/View/Dark theme");
        context->MenuClick("//##MainMenuBar/View/Reset layout");
        context->MenuClick("//##MainMenuBar/Repository/Refresh");
        context->Yield(3);

        context->SetRef("Navigator");
        context->ItemClick("navigator tabs/Bookmarks");
        context->Yield();
        context->ItemClick("**/Create bookmark");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");
        context->SetRef("Navigator");
        context->ItemClick("navigator tabs/Tags");
        context->Yield();
        context->ItemClick("**/Create tag");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");
        context->SetRef("Navigator");
        context->ItemClick("navigator tabs/Workspaces");
        context->Yield();
        context->ItemClick("**/Add workspace");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");
        context->Yield();
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
        IM_CHECK_NE(Application::DiffMarkerColorForTest("+added"), 0U);
        IM_CHECK_NE(Application::DiffMarkerColorForTest("-removed"), 0U);
        IM_CHECK_NE(Application::DiffMarkerColorForTest("+added"),
            Application::DiffMarkerColorForTest("-removed"));
        IM_CHECK_EQ(Application::DiffMarkerColorForTest(" context"), 0U);
        IM_CHECK_EQ(Application::FileUrlForTest("/tmp/a b\\c#d"), "file:///tmp/a%20b/c%23d");
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

        context->SetRef("Navigator");
        context->ItemClick("navigator tabs/Tags");
        context->Yield(2);
        IM_CHECK(context->ItemExists("**/coverage-tag"));
        context->ItemClick("**/coverage-tag");
        context->Yield(2);
        context->ItemClick("navigator tabs/Workspaces");
        context->Yield(2);
        IM_CHECK(context->ItemExists("**/Add workspace"));
        context->ItemClick("**/current", ImGuiMouseButton_Right);
        context->Yield();
        IM_CHECK(context->ItemExists("**/Open directory"));
        context->KeyPress(ImGuiKey_Escape);
        context->ItemClick("navigator tabs/Bookmarks");
        context->Yield(2);
        context->ItemClick("**/coverage-bookmark");
        context->Yield(2);

        application.ApplyEventForTest(OperationStarted{"coverage operation"});
        application.ApplyEventForTest(OperationProgress{"preparing", 0, 0});
        context->Yield(2);
        application.ApplyEventForTest(OperationProgress{"writing", 2, 4});
        context->Yield(2);
        application.ApplyEventForTest(OperationFinished{"coverage operation"});
        application.ApplyEventForTest(ErrorEvent{"coverage", "visible error"});
        context->Yield(2);

        application.ApplyEventForTest(DiffReady{{1000, "merge", "image.bin", {}, true, RichSnapshot().status}});
        context->Yield(2);
        application.ApplyEventForTest(
            DiffReady{{1000, "merge", "modified.txt", "@@ -1 +1 @@\n-old\n+new\n", false, RichSnapshot().status}});
        context->Yield(2);
        application.ApplyEventForTest(DiffReady{{0, {}, {}, {}, false, RichSnapshot().status}});
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

        context->SetRef("Operations");
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
        context->ItemClick("Apply");
        context->Yield(2);

        ApplyOpenDialog(context, "//##MainMenuBar/Change/New...");
        context->ItemInputValue("Description", "submitted new change");
        context->ItemInputValue("Parent", "base");
        context->ItemCheck("Create without editing");
        context->ItemClick("Apply");
        context->Yield(2);

        ApplyOpenDialog(context, "//##MainMenuBar/Change/Commit...");
        context->ItemInputValue("Description", "submitted commit");
        context->ItemInputValue("Filesets", " modified.txt, added.txt\n");
        context->ItemClick("Apply");
        context->Yield(2);

        for (const char* action : {"Describe...", "Metaedit...", "Rebase...", "Squash...", "Split...", "Restore...",
                 "Abandon..."})
        {
            const std::string path = std::string("//##MainMenuBar/Change/") + action;
            ApplyOpenDialog(context, path.c_str());
            context->ItemClick("Apply");
            context->Yield(2);
        }

        context->SetRef("Navigator");
        context->ItemClick("navigator tabs/Bookmarks");
        context->Yield();
        context->ItemClick("**/Create bookmark");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemInputValue("Name", "submitted-bookmark");
        context->ItemClick("Apply");
        context->Yield(2);

        context->SetRef("Navigator");
        context->ItemClick("navigator tabs/Tags");
        context->Yield();
        context->ItemClick("**/Create tag");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemInputValue("Name", "submitted-tag");
        context->ItemCheck("Allow move");
        context->ItemClick("Apply");
        context->Yield(2);

        context->SetRef("Navigator");
        context->ItemClick("navigator tabs/Workspaces");
        context->Yield();
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
            IM_CHECK_LE(std::fabs(current.RectFull.Max.y - next.RectFull.Min.y), 0.01f);
        }
        if (rows.size() >= 2)
        {
            context->ItemDragAndDrop(rows[0], rows[1]);
            IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
            context->SetRef("ggui action");
            context->ItemClick("Cancel");
            context->Yield(2);

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

        context->SetRef("Navigator");
        context->ItemClick("navigator tabs/Bookmarks");
        context->Yield();
        context->ItemClick("**/coverage-bookmark", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Delete");
        context->Yield(2);

        context->SetRef("Navigator");
        context->ItemClick("navigator tabs/Tags");
        context->Yield();
        context->ItemClick("**/coverage-tag", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Delete");
        context->Yield(2);

        context->SetRef("Navigator");
        context->ItemClick("navigator tabs/Workspaces");
        context->Yield();
        context->ItemClick("**/stale", ImGuiMouseButton_Right);
        context->Yield();
        context->ItemClick("**/Forget");
        context->Yield(2);
        context->SetRef("Navigator");
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
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");
        context->SetRef("ggui dockspace");
        context->ItemClick("Commit");
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");
        context->SetRef("ggui dockspace");
        for (const char* action : {"Previous", "Next", "Undo", "Redo", "Refresh"})
            context->ItemClick(action);

        context->MenuClick("//##MainMenuBar/Change/Edit");
        context->MenuClick("//##MainMenuBar/Change/Simplify parents");
        context->MenuClick("//##MainMenuBar/Edit/Undo");
        context->MenuClick("//##MainMenuBar/Edit/Redo");
        context->KeyPress(ImGuiMod_Ctrl | ImGuiKey_N);
        IM_CHECK_NE(WaitForWindow(context, "ggui action"), nullptr);
        context->SetRef("ggui action");
        context->ItemClick("Cancel");
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

}

} // namespace Ggui
