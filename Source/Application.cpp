// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "Application.hpp"

#include <SDL3/SDL_opengl.h>
#include <TextEditor.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#ifdef IMGUI_BUILD_TESTING
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#endif
#include <nfd.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Ggui
{

namespace
{

constexpr float kRowHeight = 50.0f;
constexpr float kLaneWidth = 20.0f;
constexpr float kDotRadius = 5.0f;
constexpr float kGraphPadding = 12.0f;

constexpr ImU32 kTextMuted = IM_COL32(125, 133, 144, 255);
constexpr ImU32 kRowBackground = IM_COL32(22, 27, 34, 255);
constexpr ImU32 kRowHover = IM_COL32(28, 34, 43, 255);
constexpr ImU32 kRowSelected = IM_COL32(33, 52, 74, 255);
constexpr ImU32 kRowBorder = IM_COL32(48, 54, 61, 180);
constexpr ImU32 kGraphBackground = IM_COL32(18, 22, 29, 255);
constexpr ImU32 kBadgeBookmark = IM_COL32(9, 105, 218, 235);
constexpr ImU32 kBadgeTag = IM_COL32(88, 70, 155, 235);
constexpr ImU32 kBadgeRemote = IM_COL32(66, 68, 90, 235);
constexpr ImU32 kBadgeWorkingCopy = IM_COL32(31, 136, 61, 235);
constexpr ImU32 kStatusAdded = IM_COL32(46, 160, 67, 255);
constexpr ImU32 kStatusModified = IM_COL32(210, 153, 34, 255);
constexpr ImU32 kStatusDeleted = IM_COL32(248, 81, 73, 255);
constexpr ImU32 kStatusRenamed = IM_COL32(47, 129, 247, 255);
constexpr ImU32 kStatusSpecial = IM_COL32(166, 91, 216, 255);
constexpr ImU32 kStatusConflict = IM_COL32(255, 123, 114, 255);

#ifdef IMGUI_BUILD_TESTING
Application* test_application = nullptr;

bool CaptureFramebuffer(
    ImGuiID viewport_id, int x, int y, int width, int height, unsigned int* pixels, void* user_data)
{
    (void)viewport_id;
    (void)user_data;
    const int framebuffer_height = static_cast<int>(ImGui::GetIO().DisplaySize.y);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, framebuffer_height - (y + height), width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (int row = 0; row < height / 2; ++row)
    {
        unsigned int* top = pixels + row * width;
        unsigned int* bottom = pixels + (height - row - 1) * width;
        std::swap_ranges(top, top + width, bottom);
    }
    return true;
}
#endif

constexpr std::array<ImU32, 8> kLaneColors{
    IM_COL32(47, 129, 247, 255), IM_COL32(166, 91, 216, 255), IM_COL32(46, 160, 67, 255),
    IM_COL32(210, 153, 34, 255), IM_COL32(248, 81, 73, 255), IM_COL32(57, 197, 187, 255),
    IM_COL32(219, 109, 40, 255), IM_COL32(163, 113, 247, 255)};

float FontPx(float value)
{
    return value * ImGui::GetFontSize() / 16.0f;
}

void LoadUiFont()
{
#ifdef _WIN32
    constexpr std::array paths{"C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cour.ttf"};
#elif defined(__APPLE__)
    constexpr std::array paths{"/System/Library/Fonts/Monaco.ttf", "/System/Library/Fonts/Menlo.ttc"};
#else
    constexpr std::array paths{"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf"};
#endif
    for (const char* path : paths)
    {
        ImFontConfig config;
        config.OversampleH = 2;
        config.OversampleV = 2;
        if (std::filesystem::exists(path)
            && ImGui::GetIO().Fonts->AddFontFromFileTTF(path, 16.0f, &config) != nullptr)
            return;
    }
}

ImU32 StatusColor(git_delta_t status)
{
    switch (status)
    {
    case GIT_DELTA_ADDED: return kStatusAdded;
    case GIT_DELTA_UNTRACKED: return kStatusAdded;
    case GIT_DELTA_DELETED: return kStatusDeleted;
    case GIT_DELTA_MODIFIED: return kStatusModified;
    case GIT_DELTA_RENAMED: return kStatusRenamed;
    case GIT_DELTA_COPIED:
    case GIT_DELTA_TYPECHANGE: return kStatusSpecial;
    case GIT_DELTA_CONFLICTED: return kStatusConflict;
    default: return kTextMuted;
    }
}

ImU32 RefBadgeColor(const NamedRef& ref)
{
    if (ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK)
        return ref.tracked ? kBadgeWorkingCopy : kBadgeBookmark;
    if (ref.kind == GG_NAMED_REF_LOCAL_TAG)
        return kBadgeTag;
    return kBadgeRemote;
}

void DrawBadge(ImDrawList* draw, ImVec2& cursor, float center_y, std::string_view label, ImU32 color)
{
    const ImVec2 text_size = ImGui::CalcTextSize(label.data(), label.data() + label.size());
    const float pad_x = FontPx(7.0f);
    const float pad_y = FontPx(3.0f);
    const ImVec2 minimum(cursor.x, center_y - text_size.y * 0.5f - pad_y);
    const ImVec2 maximum(cursor.x + text_size.x + pad_x * 2.0f, center_y + text_size.y * 0.5f + pad_y);
    draw->AddRectFilled(minimum, maximum, color, FontPx(6.0f));
    draw->AddText(ImVec2(minimum.x + pad_x, minimum.y + pad_y), IM_COL32_WHITE, label.data(), label.data() + label.size());
    cursor.x = maximum.x + FontPx(6.0f);
}

std::vector<std::string> SplitLines(std::string_view text)
{
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin < text.size())
    {
        const std::size_t end = text.find_first_of("\n,", begin);
        std::string value(text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin));
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) { return !std::isspace(c); }).base(),
            value.end());
        if (!value.empty())
            result.push_back(std::move(value));
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return result;
}

const char* DeltaName(git_delta_t status)
{
    switch (status)
    {
    case GIT_DELTA_ADDED: return "A";
    case GIT_DELTA_DELETED: return "D";
    case GIT_DELTA_MODIFIED: return "M";
    case GIT_DELTA_RENAMED: return "R";
    case GIT_DELTA_COPIED: return "C";
    case GIT_DELTA_TYPECHANGE: return "T";
    case GIT_DELTA_UNTRACKED: return "?";
    case GIT_DELTA_IGNORED: return "I";
    case GIT_DELTA_CONFLICTED: return "!";
    default: return " ";
    }
}

bool ContainsInsensitive(std::string_view haystack, std::string_view needle)
{
    return std::ranges::search(haystack, needle, [](char left, char right) {
        return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
    }).begin()
        != haystack.end();
}

std::string FileUrl(const std::string& path)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::string result = "file://";
#ifdef _WIN32
    result += '/';
#endif
    for (const unsigned char character : normalized)
    {
        if (std::isalnum(character) != 0 || std::string_view("-._~/:@").find(character) != std::string_view::npos)
            result += static_cast<char>(character);
        else
        {
            result += '%';
            result += hex[character >> 4];
            result += hex[character & 0x0f];
        }
    }
    return result;
}

} // namespace

#ifdef IMGUI_BUILD_TESTING
void RegisterUiTests(ImGuiTestEngine* engine);
#endif

int Application::Run(int argc, char** argv)
{
#ifdef IMGUI_BUILD_TESTING
    test_application = this;
    std::string repository_path;
    std::string test_filter = "all";
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument == "--test")
            _test_mode = true;
        else if (argument == "--smoke")
            _smoke_mode = true;
        else if (argument.starts_with("--test="))
        {
            _test_mode = true;
            test_filter = argument.substr(7);
        }
        else
            repository_path = argument;
    }
#endif
    if (!Initialize())
        return 1;
#ifdef IMGUI_BUILD_TESTING
    if (_test_mode)
        InitializeTestEngine(test_filter);
    else if (!repository_path.empty())
        _engine.Enqueue(OpenRepository{repository_path});
#else
    if (argc > 1)
        _engine.Enqueue(OpenRepository{argv[1]});
#endif
    else if (!_recent_repositories.empty() && std::filesystem::exists(_recent_repositories.front()))
        _engine.Enqueue(OpenRepository{_recent_repositories.front()});

    while (_running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            ProcessEvent(event);
        }

        PollEngine();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        RenderFrame();
        ImGui::Render();

        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(_window, &width, &height);
        glViewport(0, 0, width, height);
        if (_dark_theme)
            glClearColor(0.047f, 0.067f, 0.094f, 1.0f);
        else
            glClearColor(0.94f, 0.95f, 0.97f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(_window);
#ifdef IMGUI_BUILD_TESTING
        if (_smoke_mode)
            _running = false;
        if (_test_engine != nullptr)
        {
            ImGuiTestEngine_PostSwap(_test_engine);
            if (_test_mode && ImGuiTestEngine_IsTestQueueEmpty(_test_engine))
            {
                ImGuiTestEngineResultSummary summary;
                ImGuiTestEngine_GetResultSummary(_test_engine, &summary);
                _test_result = summary.CountTested > 0 && summary.CountTested == summary.CountSuccess ? 0 : 1;
                _running = false;
            }
        }
#endif
    }
    Shutdown();
#ifdef IMGUI_BUILD_TESTING
    test_application = nullptr;
    if (_test_mode)
        return _test_result;
#endif
    return 0;
}

void Application::ProcessEvent(SDL_Event& event)
{
    if (event.type == SDL_EVENT_QUIT
        || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(_window)))
        _running = false;
    if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr)
        _engine.Enqueue(OpenRepository{event.drop.data});
}

bool Application::Initialize()
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        spdlog::error("SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    _window = SDL_CreateWindow(
        "ggui", 1440, 900, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (_window == nullptr)
    {
        spdlog::error("SDL_CreateWindow failed: {}", SDL_GetError());
        SDL_Quit();
        return false;
    }
    _gl_context = SDL_GL_CreateContext(_window);
    if (_gl_context == nullptr)
    {
        spdlog::error("SDL_GL_CreateContext failed: {}", SDL_GetError());
        SDL_DestroyWindow(_window);
        _window = nullptr;
        SDL_Quit();
        return false;
    }
    SDL_GL_SetSwapInterval(1);

    char* preferences = SDL_GetPrefPath("gg", "ggui");
    if (preferences != nullptr)
    {
        _settings_path = std::filesystem::path(preferences) / "settings.json";
        _imgui_ini_path = (std::filesystem::path(preferences) / "imgui.ini").string();
        SDL_free(preferences);
    }
    LoadSettings();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = _imgui_ini_path.empty() ? nullptr : _imgui_ini_path.c_str();
    LoadUiFont();
    ApplyTheme();
    ImGui_ImplSDL3_InitForOpenGL(_window, _gl_context);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    return true;
}

void Application::Shutdown()
{
    SaveSettings();
#ifdef IMGUI_BUILD_TESTING
    if (_test_engine != nullptr)
        ImGuiTestEngine_Stop(_test_engine);
#endif
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
#ifdef IMGUI_BUILD_TESTING
    if (_test_engine != nullptr)
    {
        ImGuiTestEngine_DestroyContext(_test_engine);
        _test_engine = nullptr;
    }
#endif
    if (_gl_context != nullptr)
        SDL_GL_DestroyContext(_gl_context);
    if (_window != nullptr)
        SDL_DestroyWindow(_window);
    _gl_context = nullptr;
    _window = nullptr;
    SDL_Quit();
}

void Application::LoadSettings()
{
    if (_settings_path.empty() || !std::filesystem::exists(_settings_path))
        return;
    try
    {
        std::ifstream input(_settings_path);
        nlohmann::json json;
        input >> json;
        _recent_repositories = json.value("recentRepositories", std::vector<std::string>{});
        _dark_theme = json.value("darkTheme", true);
        _default_layout = json.value("defaultLayout", true);
    }
    catch (const std::exception& error)
    {
        spdlog::warn("Could not load settings: {}", error.what());
    }
}

void Application::SaveSettings()
{
    if (_settings_path.empty())
        return; // GCOV_EXCL_LINE: SDL supplied no preferences directory
    try
    {
        std::filesystem::create_directories(_settings_path.parent_path());
        const nlohmann::json json{{"recentRepositories", _recent_repositories}, {"darkTheme", _dark_theme},
            {"defaultLayout", _default_layout}};
        const std::filesystem::path temporary = _settings_path.string() + ".tmp";
        std::ofstream(temporary) << json.dump(2) << '\n';
        std::error_code error;
        std::filesystem::rename(temporary, _settings_path, error);
        if (error)
        {
            std::filesystem::remove(_settings_path, error);
            std::filesystem::rename(temporary, _settings_path, error);
        }
    }
    catch (const std::exception& error) // GCOV_EXCL_START: filesystem/allocator failure while persisting settings
    {
        spdlog::warn("Could not save settings: {}", error.what());
    }
    // GCOV_EXCL_STOP
}

void Application::RememberRepository(const std::string& path)
{
    _recent_repositories.erase(
        std::remove(_recent_repositories.begin(), _recent_repositories.end(), path), _recent_repositories.end());
    _recent_repositories.insert(_recent_repositories.begin(), path);
    if (_recent_repositories.size() > 10)
        _recent_repositories.resize(10);
}

void Application::PollEngine()
{
#ifdef IMGUI_BUILD_TESTING
    if (_test_snapshot_mode)
    {
        _engine.PollEvents();
        return;
    }
#endif
    for (Event& event : _engine.PollEvents())
        ApplyEvent(std::move(event));
}

void Application::ApplyEvent(Event event)
{
    std::visit(
            [this](auto&& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, SnapshotReady>)
                {
                    const std::string old_root = _snapshot == nullptr ? "" : _snapshot->root;
                    const std::string old_working = _snapshot == nullptr ? "" : _snapshot->working_copy;
                    _snapshot = std::move(value.snapshot);
                    RememberRepository(_snapshot->root);
                    _graph_generation = 0;
                    const bool repository_changed = old_root != _snapshot->root;
                    const bool selection_changed = _selected_revision.empty()
                        || std::ranges::none_of(_snapshot->revisions,
                            [this](const Revision& revision) { return revision.oid == _selected_revision; });
                    if (selection_changed)
                    {
                        _selected_revision = !_snapshot->working_copy.empty() ? _snapshot->working_copy
                            : _snapshot->revisions.empty()                    ? ""
                                                                             : _snapshot->revisions.front().oid;
                    }
                    if (repository_changed || selection_changed)
                    {
                        _selected_file.clear();
                        _diff = {};
                        if (!_selected_revision.empty())
                            _engine.Enqueue(LoadDiff{_selected_revision, {}});
                    }
                    else if (!_selected_file.empty() && old_working != _snapshot->working_copy
                        && !_snapshot->working_copy.empty())
                    {
                        _engine.Enqueue(LoadDiff{_snapshot->working_copy, _selected_file});
                    }
                }
                else if constexpr (std::is_same_v<T, DiffReady>)
                {
                    _diff = std::move(value.diff);
                }
                else if constexpr (std::is_same_v<T, OperationStarted>)
                {
                    _active_operation = value.name;
                    _error_message.clear();
                }
                else if constexpr (std::is_same_v<T, OperationProgress>)
                {
                    _progress_phase = value.phase;
                    _progress_completed = value.completed;
                    _progress_total = value.total;
                }
                else if constexpr (std::is_same_v<T, OperationFinished>)
                {
                    _status_message = value.name + " completed";
                    _active_operation.clear();
                    _progress_phase.clear();
                }
                else if constexpr (std::is_same_v<T, ErrorEvent>)
                {
                    _error_message = value.message;
                    _active_operation.clear();
                    _progress_phase.clear();
                }
                else if constexpr (std::is_same_v<T, CredentialRequest>)
                {
                    _credential_request = std::move(value);
                    OpenDialog(Dialog::Credentials);
                }
            },
            event);
}

void Application::RenderFrame()
{
    RenderMenuBar();
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput)
    {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) PickAndOpen(false);
        if (_snapshot != nullptr && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N)) OpenDialog(Dialog::New);
        if (_snapshot != nullptr && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) _engine.Enqueue(Undo{});
        if (_snapshot != nullptr && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) _engine.Enqueue(Redo{});
        if (_snapshot != nullptr && ImGui::IsKeyPressed(ImGuiKey_F5)) _engine.Enqueue(Refresh{});
    }
    if (_snapshot == nullptr)
        RenderWelcome();
    else
    {
        SetupDockspace();
        RenderNavigator();
        RenderHistory();
        RenderChanges();
        RenderDiff();
        RenderOperations();
    }
    RenderDialogs();
}

void Application::SetupDockspace()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("ggui dockspace", nullptr, flags);
    ImGui::PopStyleVar(3);
    RenderToolbar();
    const ImGuiID dockspace = ImGui::GetID("ggui main dockspace");
    ImGui::DockSpace(dockspace, {}, ImGuiDockNodeFlags_PassthruCentralNode);
    if (_default_layout && _snapshot != nullptr)
    {
        ImGui::DockBuilderRemoveNode(dockspace);
        ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);
        ImGuiID operations = 0;
        ImGuiID content = 0;
        ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Down, 0.25f, &operations, &content);
        ImGuiID navigator = 0;
        ImGuiID main = 0;
        ImGui::DockBuilderSplitNode(content, ImGuiDir_Left, 0.24f, &navigator, &main);
        ImGuiID center = 0;
        ImGuiID diff = 0;
        ImGui::DockBuilderSplitNode(main, ImGuiDir_Left, 0.68f, &center, &diff);
        ImGuiID history = 0;
        ImGuiID changes = 0;
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.68f, &history, &changes);
        ImGui::DockBuilderDockWindow("Navigator", navigator);
        ImGui::DockBuilderDockWindow("History", history);
        ImGui::DockBuilderDockWindow("Changes", changes);
        ImGui::DockBuilderDockWindow("Diff", diff);
        ImGui::DockBuilderDockWindow("Operations", operations);
        ImGui::DockBuilderFinish(dockspace);
        _default_layout = false;
    }
    ImGui::End();
}

void Application::RenderMenuBar()
{
    if (!ImGui::BeginMainMenuBar())
        return; // GCOV_EXCL_LINE: defensive ImGui frame rejection
    if (ImGui::BeginMenu("Repository"))
    {
        if (ImGui::MenuItem("Open...", "Ctrl+O"))
            PickAndOpen(false); // GCOV_EXCL_LINE: native folder picker integration
        if (ImGui::MenuItem("Initialize..."))
            PickAndOpen(true); // GCOV_EXCL_LINE: native folder picker integration
        if (ImGui::MenuItem("Clone..."))
            OpenDialog(Dialog::Clone);
        if (!_recent_repositories.empty() && ImGui::BeginMenu("Recent"))
        {
            for (const std::string& path : _recent_repositories)
                if (ImGui::MenuItem(path.c_str()))
                    _engine.Enqueue(OpenRepository{path});
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Refresh", "F5", false, _snapshot != nullptr))
            _engine.Enqueue(Refresh{});
        if (ImGui::MenuItem("Quit"))
            _running = false; // GCOV_EXCL_LINE: terminating the host aborts an in-process test queue
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Change", _snapshot != nullptr))
    {
        if (ImGui::MenuItem("New...", "Ctrl+N")) OpenDialog(Dialog::New);
        if (ImGui::MenuItem("Commit...")) OpenDialog(Dialog::Commit);
        if (ImGui::MenuItem("Describe...", nullptr, false, !_selected_revision.empty())) OpenDialog(Dialog::Describe);
        if (ImGui::MenuItem("Metaedit...", nullptr, false, !_selected_revision.empty())) OpenDialog(Dialog::Metaedit);
        if (ImGui::MenuItem("Edit", nullptr, false, !_selected_revision.empty()))
            _engine.Enqueue(Edit{_selected_revision});
        if (ImGui::MenuItem("Move working copy to previous")) _engine.Enqueue(MoveChange{GG_MOVE_PREVIOUS});
        if (ImGui::MenuItem("Move working copy to next")) _engine.Enqueue(MoveChange{GG_MOVE_NEXT});
        if (ImGui::MenuItem("Rebase...", nullptr, false, !_selected_revision.empty())) OpenDialog(Dialog::Rebase);
        if (ImGui::MenuItem("Squash...", nullptr, false, !_selected_revision.empty())) OpenDialog(Dialog::Squash);
        if (ImGui::MenuItem("Split...", nullptr, false, !_selected_revision.empty())) OpenDialog(Dialog::Split);
        if (ImGui::MenuItem("Restore...", nullptr, false, !_selected_revision.empty())) OpenDialog(Dialog::Restore);
        if (ImGui::MenuItem("Abandon...", nullptr, false, !_selected_revision.empty())) OpenDialog(Dialog::Abandon);
        if (ImGui::MenuItem("Simplify parents", nullptr, false, !_selected_revision.empty()))
            _engine.Enqueue(SimplifyParents{{_selected_revision}});
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit", _snapshot != nullptr))
    {
        if (ImGui::MenuItem("Undo", "Ctrl+Z")) _engine.Enqueue(Undo{});
        if (ImGui::MenuItem("Redo", "Ctrl+Y")) _engine.Enqueue(Redo{});
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View"))
    {
        bool dark = _dark_theme;
        if (ImGui::MenuItem("Dark theme", nullptr, &dark))
        {
            _dark_theme = dark;
            ApplyTheme();
        }
        if (ImGui::MenuItem("Reset layout"))
            _default_layout = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Application::RenderToolbar()
{
    if (_snapshot == nullptr)
        return;
    ImGui::SetCursorPos(ImVec2(10.0f, 8.0f));
    ImGui::BeginDisabled(!_active_operation.empty());
    if (ImGui::Button("New")) OpenDialog(Dialog::New);
    ImGui::SameLine();
    if (ImGui::Button("Commit")) OpenDialog(Dialog::Commit);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.122f, 0.161f, 0.216f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.176f, 0.235f, 0.314f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.208f, 0.278f, 0.369f, 1.0f));
    if (ImGui::Button("Previous")) _engine.Enqueue(MoveChange{GG_MOVE_PREVIOUS});
    ImGui::SameLine();
    if (ImGui::Button("Next")) _engine.Enqueue(MoveChange{GG_MOVE_NEXT});
    ImGui::SameLine();
    if (ImGui::Button("Undo")) _engine.Enqueue(Undo{});
    ImGui::SameLine();
    if (ImGui::Button("Redo")) _engine.Enqueue(Redo{});
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) _engine.Enqueue(Refresh{});
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("REPOSITORY");
    ImGui::SameLine();
    ImGui::TextUnformatted(_snapshot->root.c_str());
    if (!_snapshot->working_copy.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.30f, 0.78f, 0.42f, 1.0f), "@ %s", ShortId(_snapshot->working_copy).c_str());
    }
    if (!_active_operation.empty())
    {
        ImGui::SameLine();
        ImGui::Text("Working: %s", _active_operation.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) _engine.Cancel();
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
}

void Application::RenderWelcome()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("Welcome", nullptr, flags);
    ImGui::PopStyleVar(2);
    const float width = 460.0f;
    ImGui::SetCursorPosX(std::max(20.0f, (ImGui::GetContentRegionAvail().x - width) * 0.5f));
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0.0f, 70.0f));
    ImGui::SetWindowFontScale(2.0f);
    ImGui::TextColored(ImVec4(0.32f, 0.64f, 1.0f, 1.0f), "ggui");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("A graph-first workspace for the gg workflow");
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::BeginDisabled(!_active_operation.empty());
    if (ImGui::Button("Open repository...", ImVec2(width, 42.0f))) PickAndOpen(false);
    if (ImGui::Button("Initialize repository...", ImVec2(width, 42.0f))) PickAndOpen(true);
    if (ImGui::Button("Clone repository...", ImVec2(width, 42.0f))) OpenDialog(Dialog::Clone);
    ImGui::EndDisabled();
    if (!_active_operation.empty())
        ImGui::TextDisabled("Working: %s", _active_operation.c_str());
    if (!_error_message.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.35f, 1.0f), "%s", _error_message.c_str());
    if (!_recent_repositories.empty())
    {
        ImGui::SeparatorText("Recent repositories");
        for (const std::string& path : _recent_repositories)
            if (ImGui::Selectable(path.c_str(), false, 0, ImVec2(width, 34.0f)))
                _engine.Enqueue(OpenRepository{path});
    }
    ImGui::EndGroup();
    ImGui::End();
}

void Application::RenderNavigator()
{
    ImGui::Begin("Navigator");
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 5.0f));
    if (ImGui::BeginTabBar("navigator tabs", ImGuiTabBarFlags_FittingPolicyResizeDown))
    {
        if (ImGui::BeginTabItem("Bookmarks"))
        {
            if (ImGui::Button("Create bookmark", ImVec2(-1.0f, 0.0f))) OpenDialog(Dialog::Bookmark);
            for (const NamedRef& ref : _snapshot->refs)
            {
                if (ref.kind != GG_NAMED_REF_LOCAL_BOOKMARK)
                    continue;
                ImGui::PushID(&ref);
                if (ImGui::Selectable(ref.name.c_str(), ref.target == _selected_revision, 0, ImVec2(0.0f, 38.0f)))
                    SelectRevision(ref.target);
                const ImVec2 minimum = ImGui::GetItemRectMin();
                const ImVec2 maximum = ImGui::GetItemRectMax();
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(minimum, ImVec2(minimum.x + 4.0f, maximum.y), RefBadgeColor(ref), 4.0f,
                    ImDrawFlags_RoundCornersLeft);
                draw->AddText(ImVec2(minimum.x + 12.0f, minimum.y + 21.0f), kTextMuted,
                    ShortId(ref.target).c_str());
                if (ImGui::BeginPopupContextItem("bookmark context"))
                {
                    if (ImGui::MenuItem("Delete"))
                        _engine.Enqueue(Bookmark{GG_BOOKMARK_DELETE, {ref.name}, {}, {}});
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Tags"))
        {
            if (ImGui::Button("Create tag", ImVec2(-1.0f, 0.0f))) OpenDialog(Dialog::Tag);
            for (const NamedRef& ref : _snapshot->refs)
            {
                if (ref.kind != GG_NAMED_REF_LOCAL_TAG)
                    continue;
                ImGui::PushID(&ref);
                if (ImGui::Selectable(ref.name.c_str(), ref.target == _selected_revision, 0, ImVec2(0.0f, 38.0f)))
                    SelectRevision(ref.target);
                const ImVec2 minimum = ImGui::GetItemRectMin();
                const ImVec2 maximum = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRectFilled(minimum, ImVec2(minimum.x + 4.0f, maximum.y),
                    RefBadgeColor(ref), 4.0f, ImDrawFlags_RoundCornersLeft);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(minimum.x + 12.0f, minimum.y + 21.0f), kTextMuted, ShortId(ref.target).c_str());
                if (ImGui::BeginPopupContextItem("tag context"))
                {
                    if (ImGui::MenuItem("Delete")) _engine.Enqueue(Tag{GG_TAG_DELETE, {ref.name}, {}, false});
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Workspaces"))
        {
            if (ImGui::Button("Add workspace", ImVec2(-1.0f, 0.0f))) OpenDialog(Dialog::WorkspaceAdd);
            for (const Workspace& workspace : _snapshot->workspaces)
            {
                ImGui::PushID(&workspace);
                if (ImGui::Selectable(workspace.name.c_str(), workspace.working_copy == _selected_revision, 0,
                        ImVec2(0.0f, 42.0f)))
                    SelectRevision(workspace.working_copy);
                const ImVec2 minimum = ImGui::GetItemRectMin();
                const ImVec2 maximum = ImGui::GetItemRectMax();
                const ImU32 accent = workspace.stale ? kStatusDeleted : kBadgeWorkingCopy;
                ImGui::GetWindowDrawList()->AddRectFilled(minimum, ImVec2(minimum.x + 4.0f, maximum.y), accent,
                    4.0f, ImDrawFlags_RoundCornersLeft);
                ImGui::GetWindowDrawList()->AddText(ImVec2(minimum.x + 12.0f, minimum.y + 23.0f), kTextMuted,
                    workspace.stale ? "Unavailable" : workspace.root.c_str());
                if (ImGui::BeginPopupContextItem("workspace context"))
                {
                    if (ImGui::MenuItem("Forget")) _engine.Enqueue(WorkspaceForget{{workspace.name}});
                    if (ImGui::MenuItem("Rename current...")) OpenDialog(Dialog::WorkspaceRename);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sparse"))
        {
            if (ImGui::Button("Reset patterns", ImVec2(-1.0f, 0.0f))) _engine.Enqueue(SparseReset{});
            for (const std::string& pattern : _snapshot->sparse_patterns)
                ImGui::BulletText("%s", pattern.c_str());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    ImGui::End();
}

void Application::RebuildGraph()
{
    _visible_revisions.clear();
    std::vector<GraphNode> nodes;
    for (int index = 0; index < static_cast<int>(_snapshot->revisions.size()); ++index)
    {
        const Revision& revision = _snapshot->revisions[index];
        bool matches = _graph_filter.empty() || ContainsInsensitive(revision.description, _graph_filter)
            || ContainsInsensitive(revision.change_id, _graph_filter) || ContainsInsensitive(revision.oid, _graph_filter);
        if (!matches)
        {
            matches = std::ranges::any_of(_snapshot->refs, [&](const NamedRef& ref) {
                return ref.target == revision.oid && ContainsInsensitive(ref.name, _graph_filter);
            });
        }
        if (matches)
        {
            _visible_revisions.push_back(index);
            nodes.push_back({revision.oid, revision.parents});
        }
    }
    _graph_rows = BuildGraphLayout(nodes);
    _graph_generation = _snapshot->generation;
    _built_filter = _graph_filter;
}

void Application::RenderHistory()
{
    ImGui::Begin("History");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##graph filter", "Filter changes, IDs, bookmarks, tags", &_graph_filter);
    if (_graph_generation != _snapshot->generation || _built_filter != _graph_filter)
        RebuildGraph();
    const int column_count = GraphColumnCount(_graph_rows);
    ImGui::BeginChild("graph scroll", {}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(_visible_revisions.size()), kRowHeight);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    while (clipper.Step())
    {
        for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible)
        {
            const Revision& revision = _snapshot->revisions[_visible_revisions[visible]];
            const GraphRow& row = _graph_rows[visible];
            ImGui::PushID(revision.oid.c_str());
            const float width = std::max(ImGui::GetContentRegionAvail().x, column_count * kLaneWidth + 520.0f);
            ImGui::InvisibleButton("row", ImVec2(width, kRowHeight));
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            const float center = (minimum.y + maximum.y) * 0.5f;
            const bool selected = revision.oid == _selected_revision;
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) SelectRevision(revision.oid);
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
            {
                ImGui::SetDragDropPayload("GGUI_CHANGE", revision.oid.c_str(), revision.oid.size() + 1);
                ImGui::Text("Move %s", ShortId(revision.change_id).c_str());
                ImGui::EndDragDropSource();
            }
            std::optional<DropAction> hovered_drop;
            if (ImGui::BeginDragDropTarget())
            {
                const float ratio = (ImGui::GetMousePos().y - minimum.y) / kRowHeight;
                hovered_drop = ratio < 0.2f ? DropAction::ReorderBefore
                    : ratio < 0.8f                  ? DropAction::Squash
                                                  : DropAction::Rebase;
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_CHANGE"))
                {
                    _pending_drop = {static_cast<const char*>(payload->Data), revision.oid, *hovered_drop};
                    if (_pending_drop.source != _pending_drop.target) OpenDialog(Dialog::ConfirmDrop);
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem("change context"))
            {
                if (ImGui::MenuItem("Edit")) _engine.Enqueue(Edit{revision.oid});
                if (ImGui::MenuItem("Describe...")) { SelectRevision(revision.oid); OpenDialog(Dialog::Describe); }
                if (ImGui::MenuItem("Split...")) { SelectRevision(revision.oid); OpenDialog(Dialog::Split); }
                if (ImGui::MenuItem("Abandon...")) { SelectRevision(revision.oid); OpenDialog(Dialog::Abandon); }
                ImGui::EndPopup();
            }
            const ImU32 row_fill = _dark_theme
                ? selected ? kRowSelected : hovered ? kRowHover : kRowBackground
                : ImGui::GetColorU32(selected ? ImGuiCol_HeaderActive
                                             : hovered ? ImGuiCol_HeaderHovered : ImGuiCol_WindowBg);
            draw->AddRectFilled(minimum, maximum, row_fill, 6.0f);
            draw->AddRect(minimum, maximum, _dark_theme ? kRowBorder : ImGui::GetColorU32(ImGuiCol_Border), 6.0f);
            if (selected)
                draw->AddRectFilled(minimum, ImVec2(minimum.x + 4.0f, maximum.y), kBadgeBookmark, 6.0f,
                    ImDrawFlags_RoundCornersLeft);
            if (hovered_drop.has_value())
            {
                const ImU32 zone_color = *hovered_drop == DropAction::ReorderBefore ? IM_COL32(90, 150, 255, 80)
                    : *hovered_drop == DropAction::Squash                            ? IM_COL32(220, 170, 70, 80)
                                                                                     : IM_COL32(110, 210, 145, 80);
                const float zone_top = *hovered_drop == DropAction::ReorderBefore ? minimum.y
                    : *hovered_drop == DropAction::Squash                          ? minimum.y + kRowHeight * 0.2f
                                                                                  : minimum.y + kRowHeight * 0.8f;
                const float zone_bottom = *hovered_drop == DropAction::ReorderBefore ? minimum.y + kRowHeight * 0.2f
                    : *hovered_drop == DropAction::Squash                             ? minimum.y + kRowHeight * 0.8f
                                                                                     : maximum.y;
                draw->AddRectFilled(ImVec2(minimum.x, zone_top), ImVec2(maximum.x, zone_bottom), zone_color);
            }
            const float graph_width = std::max(column_count, 1) * kLaneWidth + kGraphPadding * 2.0f;
            draw->AddRectFilled(minimum, ImVec2(minimum.x + graph_width, maximum.y),
                _dark_theme ? kGraphBackground : IM_COL32(229, 233, 239, 255), 6.0f, ImDrawFlags_RoundCornersLeft);
            const float graph_left = minimum.x + kGraphPadding;
            auto lane_x = [&](int column) { return graph_left + column * kLaneWidth + kLaneWidth * 0.5f; };
            auto color = [](int track) { return kLaneColors[static_cast<std::size_t>(track) % kLaneColors.size()]; };

            std::vector<int> shifted_before(row.tracks_before.size(), -1);
            std::vector<int> shifted_after(row.tracks_after.size(), -1);
            for (std::size_t before = 0; before < row.tracks_before.size(); ++before)
            {
                const auto after = std::find(row.tracks_after.begin(), row.tracks_after.end(), row.tracks_before[before]);
                if (after != row.tracks_after.end() && static_cast<std::size_t>(after - row.tracks_after.begin()) != before)
                {
                    shifted_before[before] = static_cast<int>(after - row.tracks_after.begin());
                    shifted_after[shifted_before[before]] = static_cast<int>(before);
                }
            }
            for (std::size_t column = 0; column < row.tracks_before.size(); ++column)
                draw->AddLine(ImVec2(lane_x(static_cast<int>(column)), minimum.y),
                    ImVec2(lane_x(static_cast<int>(column)), shifted_before[column] < 0 ? center : center - 7.0f),
                    color(row.tracks_before[column]), 2.0f);
            for (std::size_t column = 0; column < row.tracks_after.size(); ++column)
            {
                const bool new_non_current_lane = static_cast<int>(column) != row.column && shifted_after[column] < 0
                    && std::ranges::find(row.tracks_before, row.tracks_after[column]) == row.tracks_before.end();
                if (new_non_current_lane)
                    continue;
                draw->AddLine(ImVec2(lane_x(static_cast<int>(column)), shifted_after[column] < 0 ? center : center + 7.0f),
                    ImVec2(lane_x(static_cast<int>(column)), maximum.y), color(row.tracks_after[column]), 2.0f);
            }
            for (std::size_t before = 0; before < shifted_before.size(); ++before)
            {
                if (shifted_before[before] < 0) continue;
                draw->AddBezierCubic(ImVec2(lane_x(static_cast<int>(before)), center - 7.0f),
                    ImVec2(lane_x(static_cast<int>(before)), center), ImVec2(lane_x(shifted_before[before]), center),
                    ImVec2(lane_x(shifted_before[before]), center + 7.0f), color(row.tracks_before[before]), 2.0f);
            }
            const float dot_x = lane_x(row.column);
            for (int parent_column : row.parent_columns)
            {
                if (parent_column == row.column || parent_column >= static_cast<int>(row.tracks_after.size())) continue;
                const float parent_x = lane_x(parent_column);
                draw->AddBezierCubic(ImVec2(dot_x, center + kDotRadius), ImVec2(dot_x, center + 12.0f),
                    ImVec2(parent_x, maximum.y - 10.0f), ImVec2(parent_x, maximum.y),
                    color(row.tracks_after[parent_column]), 2.0f);
            }
            if (selected)
                draw->AddCircle(ImVec2(dot_x, center), kDotRadius + 3.0f, IM_COL32(47, 129, 247, 150), 0, 2.0f);
            draw->AddCircleFilled(ImVec2(dot_x, center), kDotRadius,
                revision.conflicted ? kStatusConflict
                                    : revision.working_copy ? kStatusAdded : IM_COL32(246, 248, 250, 255));
            draw->AddCircle(ImVec2(dot_x, center), kDotRadius, IM_COL32(17, 24, 39, 255), 0, 1.25f);

            const float content_x = minimum.x + graph_width + 12.0f;
            ImGui::PushClipRect(ImVec2(content_x, minimum.y), ImVec2(maximum.x - 8.0f, maximum.y), true);
            const std::string description = FirstLine(revision.description);
            ImGui::SetCursorScreenPos(ImVec2(content_x, minimum.y + 6.0f));
            ImGui::TextUnformatted(description.empty() ? "(no description)" : description.c_str());
            const std::string meta = ShortId(revision.change_id) + "  " + revision.author;
            ImGui::SetCursorScreenPos(ImVec2(content_x, minimum.y + 28.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
            ImGui::TextUnformatted(meta.c_str());
            ImGui::PopStyleColor();
            ImVec2 badge_cursor(content_x + ImGui::CalcTextSize(meta.c_str()).x + 12.0f, minimum.y + 28.0f);
            for (const NamedRef& ref : _snapshot->refs)
            {
                if (ref.target != revision.oid) continue;
                DrawBadge(draw, badge_cursor, minimum.y + 36.0f, ref.name, RefBadgeColor(ref));
            }
            ImGui::PopClipRect();

            ImGui::SetCursorScreenPos(ImVec2(minimum.x, maximum.y));
            ImGui::PopID();
        }
    }
    ImGui::PopStyleVar();
    ImGui::InvisibleButton("move to end", ImVec2(-1.0f, 22.0f));
    if (!_visible_revisions.empty() && ImGui::BeginDragDropTarget())
    {
        ImGui::TextUnformatted("Move after final change");
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_CHANGE"))
        {
            const Revision& final = _snapshot->revisions[_visible_revisions.back()];
            _pending_drop = {static_cast<const char*>(payload->Data), final.oid, DropAction::ReorderAfter};
            if (_pending_drop.source != _pending_drop.target) OpenDialog(Dialog::ConfirmDrop);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();
    ImGui::End();
}

void Application::RenderChanges()
{
    ImGui::Begin("Changes");
    if (_diff.files.empty())
        ImGui::TextDisabled("Selected change is empty.");
    else
        ImGui::TextDisabled("%zu changed file%s", _diff.files.size(), _diff.files.size() == 1 ? "" : "s");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.0f));
    for (const StatusEntry& file : _diff.files)
    {
        ImGui::PushID(&file);
        const std::string label = std::string(DeltaName(file.status)) + "  " + file.path;
        const ImU32 accent = StatusColor(file.conflicted ? GIT_DELTA_CONFLICTED : file.status);
        ImGui::PushStyleColor(ImGuiCol_Text, accent);
        const bool selected = ImGui::Selectable(label.c_str(), file.path == _selected_file, 0, ImVec2(0.0f, 26.0f));
        ImGui::PopStyleColor();
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(
            minimum, ImVec2(minimum.x + 4.0f, maximum.y), accent, 4.0f, ImDrawFlags_RoundCornersLeft);
        if (selected) SelectFile(file.path);
        if (_selected_revision == _snapshot->working_copy && ImGui::BeginPopupContextItem("file context"))
        {
            if (ImGui::MenuItem("Commit only this file"))
            {
                _selected_file = file.path;
                OpenDialog(Dialog::Commit);
            }
            if (ImGui::MenuItem("Restore this file"))
                _engine.Enqueue(Restore{"@-", "@", {file.path}});
            if (ImGui::MenuItem("Track")) _engine.Enqueue(TrackPaths{{file.path}});
            if (ImGui::MenuItem("Untrack")) _engine.Enqueue(UntrackPaths{{file.path}});
            if (ImGui::MenuItem("Mark executable")) _engine.Enqueue(ChmodPaths{{file.path}, true});
            if (ImGui::MenuItem("Mark non-executable")) _engine.Enqueue(ChmodPaths{{file.path}, false});
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    if (_selected_revision == _snapshot->working_copy && !_snapshot->conflicts.empty())
    {
        ImGui::SeparatorText("Conflicts");
        for (const Conflict& conflict : _snapshot->conflicts)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kStatusConflict);
            ImGui::TextUnformatted(conflict.path.c_str());
            ImGui::PopStyleColor();
            ImGui::TextDisabled("%zu base removals, %zu side additions", conflict.removes, conflict.adds);
            ImGui::PushID(&conflict);
            if (ImGui::SmallButton("Open externally"))
            {
                const std::string path = (std::filesystem::path(_snapshot->root) / conflict.path).string(); // GCOV_EXCL_LINE: external application handoff
                SDL_OpenURL(FileUrl(path).c_str()); // GCOV_EXCL_LINE: external application handoff
            }
            ImGui::PopID();
        }
        ImGui::TextWrapped("Save resolved files; ggui snapshots them automatically.");
    }
    ImGui::End();
}

void Application::RenderDiff()
{
    ImGui::Begin("Diff");
    if (_diff.revision.empty())
    {
        ImGui::TextWrapped("Select a change or file to inspect its diff.");
    }
    else if (_diff.binary)
    {
        ImGui::Text("%s is binary.", _diff.path.c_str());
    }
    else
    {
        static TextEditor editor;
        static std::string loaded;
        if (loaded != _diff.patch)
        {
            loaded = _diff.patch;
            editor.SetReadOnlyEnabled(true);
            editor.SetShowWhitespacesEnabled(false);
            editor.SetText(loaded);
        }
        ImGui::TextDisabled("%s%s%s", ShortId(_diff.revision).c_str(), _diff.path.empty() ? "" : "  ",
            _diff.path.c_str());
        editor.Render("##diff editor", ImGui::GetContentRegionAvail(), true);
    }
    ImGui::End();
}

void Application::RenderOperations()
{
    ImGui::Begin("Operations");
    if (!_error_message.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.35f, 1.0f), "%s", _error_message.c_str());
        ImGui::Separator();
    }
    if (!_progress_phase.empty())
    {
        const float progress = _progress_total == 0 ? 0.0f
                                                    : static_cast<float>(_progress_completed) / _progress_total;
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), _progress_phase.c_str());
    }
    if (!_status_message.empty())
        ImGui::TextDisabled("%s", _status_message.c_str());
    if (ImGui::BeginTable("operation table", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Operation", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        for (const Operation& operation : _snapshot->operations)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(operation.description.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", ShortId(operation.oid).c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(&operation);
            if (ImGui::SmallButton("Restore")) _engine.Enqueue(RestoreOperation{operation.oid});
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void Application::OpenDialog(Dialog dialog)
{
    _dialog = dialog;
    _input_primary.clear();
    _input_secondary.clear();
    _input_tertiary.clear();
    _input_filesets = _selected_file;
    _input_flag = false;
    _input_mode = 0;
    if (dialog == Dialog::Describe || dialog == Dialog::Metaedit)
    {
        const auto selected = std::ranges::find_if(
            _snapshot->revisions, [this](const Revision& revision) { return revision.oid == _selected_revision; });
        if (selected != _snapshot->revisions.end())
            _input_primary = selected->description;
    }
    if (dialog == Dialog::Credentials)
        _input_primary = _credential_request.username;
}

void Application::RenderDialogs()
{
    if (_dialog == Dialog::None)
        return;
    constexpr std::array popup_titles{"Action###ggui action", "Clone repository###ggui action",
        "Create change###ggui action", "Commit change###ggui action", "Describe change###ggui action",
        "Edit metadata###ggui action", "Rebase change###ggui action", "Squash changes###ggui action",
        "Split change###ggui action", "Abandon change###ggui action", "Restore files###ggui action",
        "Create bookmark###ggui action", "Create tag###ggui action", "Add workspace###ggui action",
        "Rename workspace###ggui action", "Credentials###ggui action", "Confirm operation###ggui action"};
    if (!ImGui::IsPopupOpen("ggui action"))
        ImGui::OpenPopup("ggui action");
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::BeginPopupModal(
            popup_titles[static_cast<std::size_t>(_dialog)], &open, ImGuiWindowFlags_AlwaysAutoResize))
        return; // GCOV_EXCL_LINE: defensive ImGui popup frame rejection

    switch (_dialog)
    {
    case Dialog::Clone:
        ImGui::TextUnformatted("Clone repository");
        ImGui::InputTextWithHint("URL", "https://host/owner/repository.git", &_input_primary);
        ImGui::InputTextWithHint("Destination", "/path/to/repository", &_input_secondary);
        ImGui::SameLine();
        if (ImGui::Button("Browse")) _input_secondary = PickFolder();
        break;
    case Dialog::New:
        ImGui::TextUnformatted("Create change");
        ImGui::InputTextMultiline("Description", &_input_primary, ImVec2(-1.0f, 90.0f));
        ImGui::InputTextWithHint("Parent", "optional revision", &_input_secondary);
        ImGui::Checkbox("Create without editing", &_input_flag);
        break;
    case Dialog::Commit:
        ImGui::TextUnformatted("Commit working change and create a new one");
        ImGui::InputTextMultiline("Description", &_input_primary, ImVec2(-1.0f, 90.0f));
        ImGui::InputTextMultiline("Filesets", &_input_filesets, ImVec2(-1.0f, 70.0f));
        break;
    case Dialog::Describe:
        ImGui::Text("Describe %s", ShortId(_selected_revision).c_str());
        ImGui::InputTextMultiline("Description", &_input_primary, ImVec2(-1.0f, 120.0f));
        break;
    case Dialog::Metaedit:
        ImGui::Text("Edit metadata for %s", ShortId(_selected_revision).c_str());
        ImGui::InputTextMultiline("Description", &_input_primary, ImVec2(-1.0f, 100.0f));
        ImGui::InputTextWithHint("Author", "Name <email>", &_input_secondary);
        break;
    case Dialog::Rebase:
        ImGui::Text("Rebase %s", ShortId(_selected_revision).c_str());
        ImGui::InputTextWithHint("Destination", "change ID, bookmark, or commit ID", &_input_primary);
        break;
    case Dialog::Squash:
        ImGui::Text("Squash %s", ShortId(_selected_revision).c_str());
        ImGui::InputTextWithHint("Into", "defaults to parent", &_input_secondary);
        ImGui::InputTextMultiline("Combined description", &_input_primary, ImVec2(-1.0f, 90.0f));
        break;
    case Dialog::Split:
        ImGui::Text("Split %s", ShortId(_selected_revision).c_str());
        ImGui::InputTextMultiline("Selected filesets", &_input_filesets, ImVec2(-1.0f, 90.0f));
        ImGui::InputTextWithHint("Selected description", "optional", &_input_primary);
        break;
    case Dialog::Restore:
        ImGui::Text("Restore into %s", ShortId(_selected_revision).c_str());
        ImGui::InputTextWithHint("From", "defaults to parent", &_input_primary);
        ImGui::InputTextMultiline("Filesets", &_input_filesets, ImVec2(-1.0f, 90.0f));
        break;
    case Dialog::Abandon:
        ImGui::TextWrapped("Abandon %s and restack its descendants? This remains undoable.",
            ShortId(_selected_revision).c_str());
        ImGui::Checkbox("Retain bookmarks", &_input_flag);
        break;
    case Dialog::Bookmark:
        ImGui::TextUnformatted("Create bookmark");
        ImGui::InputText("Name", &_input_primary);
        ImGui::InputTextWithHint("Revision", "defaults to selected change", &_input_secondary);
        break;
    case Dialog::Tag:
        ImGui::TextUnformatted("Create or move tag");
        ImGui::InputText("Name", &_input_primary);
        ImGui::InputTextWithHint("Revision", "defaults to selected change", &_input_secondary);
        ImGui::Checkbox("Allow move", &_input_flag);
        break;
    case Dialog::WorkspaceAdd:
        ImGui::TextUnformatted("Add workspace");
        ImGui::InputText("Destination", &_input_primary);
        ImGui::SameLine();
        if (ImGui::Button("Browse")) _input_primary = PickFolder();
        ImGui::InputTextWithHint("Name", "derived from directory if empty", &_input_secondary);
        ImGui::InputTextWithHint("Revision", "defaults to @", &_input_tertiary);
        ImGui::Combo("Sparse patterns", &_input_mode, "Copy current\0Full\0Empty\0");
        break;
    case Dialog::WorkspaceRename:
        ImGui::TextUnformatted("Rename current workspace");
        ImGui::InputText("New name", &_input_primary);
        break;
    case Dialog::Credentials:
        ImGui::TextWrapped("Credentials requested by %s", _credential_request.url.c_str());
        ImGui::Combo("Method", &_input_mode, "Username / token\0SSH agent\0SSH key\0");
        ImGui::InputText("Username", &_input_primary);
        if (_input_mode == 2)
        {
            ImGui::InputText("Private key", &_input_secondary);
            ImGui::InputText("Public key", &_input_tertiary);
        }
        if (_input_mode != 1)
            ImGui::InputText("Token / passphrase", &_input_filesets, ImGuiInputTextFlags_Password);
        break;
    case Dialog::ConfirmDrop:
    {
        const char* action = _pending_drop.action == DropAction::Squash ? "Squash"
            : _pending_drop.action == DropAction::Rebase               ? "Rebase"
            : _pending_drop.action == DropAction::ReorderAfter         ? "Move after"
                                                                       : "Move before";
        ImGui::Text("%s %s", action, ShortId(_pending_drop.source).c_str());
        ImGui::Text("Target: %s", ShortId(_pending_drop.target).c_str());
        int affected = 0;
        for (const Revision& revision : _snapshot->revisions)
            affected += std::ranges::find(revision.parents, _pending_drop.source) != revision.parents.end();
        int refs = 0;
        for (const NamedRef& ref : _snapshot->refs)
            refs += ref.target == _pending_drop.source || ref.target == _pending_drop.target;
        ImGui::TextWrapped("gg will restack affected descendants and move associated refs atomically. Direct children: %d; refs on source/target: %d. Conflicts remain editable and this operation can be undone.",
            affected, refs);
        break;
    }
    case Dialog::None: break; // GCOV_EXCL_LINE: RenderDialogs returns before switching on None
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::BeginDisabled(!_active_operation.empty());
    if (ImGui::Button(_dialog == Dialog::ConfirmDrop ? "Confirm" : "Apply", ImVec2(110.0f, 0.0f)))
        SubmitDialog();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)))
    {
        if (_dialog == Dialog::Credentials) _engine.CancelCredential();
        _dialog = Dialog::None;
        ImGui::CloseCurrentPopup();
    }
    if (!open) // GCOV_EXCL_START: native title-bar close path; Cancel is automated instead
    {
        if (_dialog == Dialog::Credentials) _engine.CancelCredential();
        _dialog = Dialog::None;
        ImGui::CloseCurrentPopup();
    }
    // GCOV_EXCL_STOP
    ImGui::EndPopup();
}

void Application::SubmitDialog()
{
    switch (_dialog)
    {
    case Dialog::Clone: _engine.Enqueue(CloneRepository{_input_primary, _input_secondary}); break;
    case Dialog::New:
        _engine.Enqueue(NewChange{_input_primary, _input_secondary.empty() ? std::vector<std::string>{}
                                                                         : std::vector<std::string>{_input_secondary},
            {}, {}, _input_flag});
        break;
    case Dialog::Commit: _engine.Enqueue(Commit{_input_primary, SplitLines(_input_filesets)}); break;
    case Dialog::Describe: _engine.Enqueue(Describe{_selected_revision, _input_primary}); break;
    case Dialog::Metaedit: _engine.Enqueue(Metaedit{_selected_revision, _input_primary, _input_secondary}); break;
    case Dialog::Rebase: _engine.Enqueue(Rebase{_selected_revision, _input_primary}); break;
    case Dialog::Squash: _engine.Enqueue(Squash{_selected_revision, _input_secondary, _input_primary}); break;
    case Dialog::Split: _engine.Enqueue(Split{_selected_revision, _input_primary, SplitLines(_input_filesets)}); break;
    case Dialog::Abandon: _engine.Enqueue(Abandon{{_selected_revision}, _input_flag, false}); break;
    case Dialog::Restore:
        _engine.Enqueue(Restore{_input_primary, _selected_revision, SplitLines(_input_filesets)});
        break;
    case Dialog::Bookmark:
        _engine.Enqueue(Bookmark{GG_BOOKMARK_CREATE, {_input_primary},
            _input_secondary.empty() ? _selected_revision : _input_secondary, {}});
        break;
    case Dialog::Tag:
        _engine.Enqueue(Tag{GG_TAG_SET, {_input_primary},
            _input_secondary.empty() ? _selected_revision : _input_secondary, _input_flag});
        break;
    case Dialog::WorkspaceAdd:
        _engine.Enqueue(WorkspaceAdd{_input_primary, _input_secondary,
            _input_tertiary.empty() ? "@" : _input_tertiary, {}, _input_mode});
        break;
    case Dialog::WorkspaceRename: _engine.Enqueue(WorkspaceRename{_input_primary}); break;
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
            _engine.Enqueue(Reorder{_pending_drop.source, _pending_drop.target,
                _pending_drop.action == DropAction::ReorderAfter ? GG_REORDER_AFTER : GG_REORDER_BEFORE});
        break;
    case Dialog::None: break; // GCOV_EXCL_LINE: no dialog can submit None
    }
    _dialog = Dialog::None;
    ImGui::CloseCurrentPopup();
}

void Application::SelectRevision(const std::string& oid)
{
    _selected_revision = oid;
    _selected_file.clear();
    _engine.Enqueue(LoadDiff{oid, {}});
}

void Application::SelectFile(const std::string& path)
{
    _selected_file = path;
    if (!_selected_revision.empty())
        _engine.Enqueue(LoadDiff{_selected_revision, path});
}

// GCOV_EXCL_START: nativefiledialog owns the platform-dependent modal interaction
void Application::PickAndOpen(bool initialize)
{
    const std::string path = PickFolder();
    if (!path.empty())
        _engine.Enqueue(initialize ? Command{InitRepository{path}} : Command{OpenRepository{path}});
}

std::string Application::PickFolder(const std::string& initial)
{
    if (NFD_Init() != NFD_OKAY)
    {
        _error_message = NFD_GetError() == nullptr ? "Could not initialize folder picker" : NFD_GetError();
        return {};
    }
    nfdu8char_t* path = nullptr;
    nfdpickfolderu8args_t arguments{};
    arguments.defaultPath = initial.empty() ? nullptr : initial.c_str();
    const nfdresult_t result = NFD_PickFolderU8_With(&path, &arguments);
    std::string selected;
    if (result == NFD_OKAY && path != nullptr)
        selected = path;
    else if (result == NFD_ERROR)
        _error_message = NFD_GetError() == nullptr ? "Folder picker failed" : NFD_GetError();
    if (path != nullptr)
        NFD_FreePathU8(path);
    NFD_Quit();
    return selected;
}
// GCOV_EXCL_STOP

void Application::ApplyTheme()
{
    if (_dark_theme)
    {
        ImGui::StyleColorsDark();
        ImVec4* colors = ImGui::GetStyle().Colors;
        colors[ImGuiCol_Text] = ImVec4(0.902f, 0.925f, 0.953f, 1.0f);
        colors[ImGuiCol_TextDisabled] = ImVec4(0.490f, 0.521f, 0.564f, 1.0f);
        colors[ImGuiCol_WindowBg] = ImVec4(0.047f, 0.067f, 0.094f, 1.0f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.082f, 0.106f, 0.145f, 1.0f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.082f, 0.106f, 0.145f, 0.98f);
        colors[ImGuiCol_Border] = ImVec4(0.204f, 0.251f, 0.314f, 1.0f);
        colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.098f, 0.129f, 0.176f, 1.0f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.125f, 0.161f, 0.220f, 1.0f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.145f, 0.188f, 0.255f, 1.0f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.059f, 0.078f, 0.110f, 1.0f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.082f, 0.106f, 0.145f, 1.0f);
        colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.059f, 0.078f, 0.110f, 1.0f);
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.059f, 0.078f, 0.110f, 1.0f);
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.059f, 0.078f, 0.110f, 1.0f);
        colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.176f, 0.216f, 0.278f, 1.0f);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.235f, 0.286f, 0.365f, 1.0f);
        colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.278f, 0.341f, 0.435f, 1.0f);
        colors[ImGuiCol_CheckMark] = ImVec4(0.180f, 0.706f, 0.333f, 1.0f);
        colors[ImGuiCol_SliderGrab] = ImVec4(0.133f, 0.478f, 0.863f, 1.0f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(0.180f, 0.561f, 0.973f, 1.0f);
        colors[ImGuiCol_Button] = ImVec4(0.114f, 0.424f, 0.765f, 0.92f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.153f, 0.510f, 0.910f, 1.0f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.110f, 0.357f, 0.639f, 1.0f);
        colors[ImGuiCol_Header] = ImVec4(0.114f, 0.278f, 0.459f, 0.95f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.153f, 0.349f, 0.565f, 1.0f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.173f, 0.392f, 0.624f, 1.0f);
        colors[ImGuiCol_Separator] = ImVec4(0.204f, 0.251f, 0.314f, 1.0f);
        colors[ImGuiCol_SeparatorHovered] = ImVec4(0.267f, 0.337f, 0.427f, 1.0f);
        colors[ImGuiCol_SeparatorActive] = ImVec4(0.114f, 0.424f, 0.765f, 1.0f);
        colors[ImGuiCol_ResizeGrip] = ImVec4(0.114f, 0.424f, 0.765f, 0.25f);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.153f, 0.510f, 0.910f, 0.67f);
        colors[ImGuiCol_ResizeGripActive] = ImVec4(0.153f, 0.510f, 0.910f, 0.95f);
        colors[ImGuiCol_Tab] = ImVec4(0.082f, 0.106f, 0.145f, 1.0f);
        colors[ImGuiCol_TabHovered] = ImVec4(0.125f, 0.161f, 0.220f, 1.0f);
        colors[ImGuiCol_TabActive] = ImVec4(0.098f, 0.129f, 0.176f, 1.0f);
        colors[ImGuiCol_TabUnfocused] = ImVec4(0.074f, 0.094f, 0.129f, 1.0f);
        colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.090f, 0.114f, 0.157f, 1.0f);
        colors[ImGuiCol_DockingPreview] = ImVec4(0.153f, 0.510f, 0.910f, 0.35f);
        colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.047f, 0.067f, 0.094f, 1.0f);
        colors[ImGuiCol_TableHeaderBg] = ImVec4(0.082f, 0.106f, 0.145f, 1.0f);
        colors[ImGuiCol_TableBorderStrong] = ImVec4(0.204f, 0.251f, 0.314f, 1.0f);
        colors[ImGuiCol_TableBorderLight] = ImVec4(0.157f, 0.196f, 0.251f, 1.0f);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.071f, 0.090f, 0.122f, 0.55f);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(0.153f, 0.510f, 0.910f, 0.35f);
        colors[ImGuiCol_DragDropTarget] = ImVec4(0.180f, 0.706f, 0.333f, 1.0f);
        colors[ImGuiCol_NavHighlight] = ImVec4(0.153f, 0.510f, 0.910f, 0.85f);
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.031f, 0.043f, 0.063f, 0.78f);
    }
    else
        ImGui::StyleColorsLight();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 8.0f;
    style.PopupRounding = 8.0f;
    style.FrameRounding = 7.0f;
    style.GrabRounding = 7.0f;
    style.TabRounding = 8.0f;
    style.WindowPadding = ImVec2(10.0f, 10.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.CellPadding = ImVec2(10.0f, 8.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize = 13.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;
}

#ifdef IMGUI_BUILD_TESTING
void Application::InitializeTestEngine(const std::string& filter)
{
    _test_engine = ImGuiTestEngine_CreateContext();
    if (_test_engine == nullptr)
        throw std::runtime_error("could not create ImGui Test Engine"); // GCOV_EXCL_LINE: forced test-engine allocation failure
    RegisterUiTests(_test_engine);
    ImGuiTestEngine_Start(_test_engine, ImGui::GetCurrentContext());
    ImGuiTestEngineIO& io = ImGuiTestEngine_GetIO(_test_engine);
    io.ConfigLogToTTY = true;
    io.ConfigCaptureEnabled = std::getenv("GGUI_CAPTURE_MANUAL") != nullptr;
    io.ScreenCaptureFunc = CaptureFramebuffer;
    io.ConfigFixedDeltaTime = 1.0f / 60.0f;
    ImGuiTestEngine_QueueTests(
        _test_engine, ImGuiTestGroup_Unknown, filter.c_str(), ImGuiTestRunFlags_RunFromCommandLine);
}

Application& Application::Instance()
{
    return *test_application;
}

void Application::OpenTestRepository(const std::string& path)
{
    _test_snapshot_mode = false;
#ifdef GGUI_TESTING
    _engine.SetCommandsSuppressedForTest(false);
#endif
    _engine.Enqueue(OpenRepository{path});
}

void Application::RefreshForTest()
{
    _engine.Enqueue(Refresh{});
}

std::shared_ptr<const RepoSnapshot> Application::SnapshotForTest() const
{
    return _snapshot;
}

void Application::ApplyEventForTest(Event event)
{
    ApplyEvent(std::move(event));
}

void Application::ShowDropConfirmationForTest(const std::string& source, const std::string& target, int action)
{
    _pending_drop = {source, target, static_cast<DropAction>(std::clamp(action, 0, 3))};
    OpenDialog(Dialog::ConfirmDrop);
}

void Application::ShowWorkspaceRenameForTest()
{
    OpenDialog(Dialog::WorkspaceRename);
}

void Application::SetSnapshotForTest(RepoSnapshot snapshot)
{
    _test_snapshot_mode = true;
#ifdef GGUI_TESTING
    _engine.SetCommandsSuppressedForTest(true);
#endif
    _snapshot = std::make_shared<RepoSnapshot>(std::move(snapshot));
    _selected_revision = _snapshot->working_copy.empty()
        ? (_snapshot->revisions.empty() ? "" : _snapshot->revisions.front().oid)
        : _snapshot->working_copy;
    _selected_file.clear();
    _diff = {_snapshot->generation, _selected_revision, {}, {}, false, _snapshot->status};
    _graph_generation = 0;
}

void Application::ClearSnapshotForTest()
{
    _snapshot.reset();
    _selected_revision.clear();
    _selected_file.clear();
    _diff = {};
}

void Application::AddRecentForTest(const std::string& path)
{
    RememberRepository(path);
}

void Application::ProcessEventForTest(SDL_Event event)
{
    const bool running = _running;
    ProcessEvent(event);
    _running = running;
}

std::vector<std::string> Application::SplitLinesForTest(const std::string& text)
{
    return SplitLines(text);
}

std::string Application::DeltaNameForTest(git_delta_t status)
{
    return DeltaName(status);
}

bool Application::ContainsInsensitiveForTest(const std::string& text, const std::string& query)
{
    return ContainsInsensitive(text, query);
}

std::string Application::FileUrlForTest(const std::string& path)
{
    return FileUrl(path);
}
#endif

} // namespace Ggui
