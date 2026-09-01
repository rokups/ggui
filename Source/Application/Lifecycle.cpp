// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <SDL3/SDL_opengl.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl3.h>
#ifdef IMGUI_BUILD_TESTING
#include <imgui_te_engine.h>
#include <imgui_te_exporters.h>
#endif
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace Ggui
{
using namespace ApplicationInternal;

SDL_Rect ApplicationInternal::FitWindowToDisplays(
    SDL_Rect window, std::span<const SDL_Rect> displays)
{
    if (displays.empty()) return window;
    const auto overlap = [&](const SDL_Rect& display) {
        const int width = std::max(0,
            std::min(window.x + window.w, display.x + display.w) - std::max(window.x, display.x));
        const int height = std::max(0,
            std::min(window.y + window.h, display.y + display.h) - std::max(window.y, display.y));
        return static_cast<std::int64_t>(width) * height;
    };
    const SDL_Rect* target = &displays.front();
    std::int64_t best_overlap = overlap(*target);
    for (const SDL_Rect& display : displays.subspan(1))
    {
        const std::int64_t candidate = overlap(display);
        if (candidate > best_overlap)
        {
            target = &display;
            best_overlap = candidate;
        }
    }
    window.w = std::min(window.w, target->w);
    window.h = std::min(window.h, target->h);
    window.x = std::clamp(window.x, target->x, target->x + target->w - window.w);
    window.y = std::clamp(window.y, target->y, target->y + target->h - window.h);
    return window;
}

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
        EnqueueAction(OpenRepository{repository_path});
#else
    if (argc > 1)
        EnqueueAction(OpenRepository{argv[1]});
#endif
    else if (!_recent_repositories.empty() && std::filesystem::exists(_recent_repositories.front()))
        EnqueueAction(OpenRepository{_recent_repositories.front()});

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
        // ImGui colors are already authored as display-encoded sRGB values.
        // Encoding them again through an sRGB framebuffer visibly washes out
        // the interface, notably with some Windows OpenGL pixel formats.
        glDisable(GL_FRAMEBUFFER_SRGB);
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
                ImGuiTestEngine_PrintResultSummary(_test_engine);
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
    if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr && _active_operation.empty())
        EnqueueAction(OpenRepository{event.drop.data});
    if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST
        && event.window.windowID == SDL_GetWindowID(_window))
    {
        const SDL_WindowFlags flags = SDL_GetWindowFlags(_window);
        _window_maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
        const bool normal = (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN)) == 0;
        if (normal && event.type == SDL_EVENT_WINDOW_MOVED)
        {
            _window_x = event.window.data1;
            _window_y = event.window.data2;
            _window_has_position = true;
        }
        if (normal && event.type == SDL_EVENT_WINDOW_RESIZED)
        {
            _window_width = event.window.data1;
            _window_height = event.window.data2;
            _window_has_size = true;
        }
        if (event.type == SDL_EVENT_WINDOW_MOVED || event.type == SDL_EVENT_WINDOW_RESIZED
            || event.type == SDL_EVENT_WINDOW_MAXIMIZED || event.type == SDL_EVENT_WINDOW_RESTORED)
            ImGui::MarkIniSettingsDirty();
        if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED)
            ApplyTheme();
    }
}

bool Application::Initialize()
{
    // SDL 3.4.2+ can force the non-sRGB WGL pixel format instead of merely
    // requesting one. This keeps Windows presentation consistent with Linux.
    SDL_SetHint(SDL_HINT_OPENGL_FORCE_SRGB_FRAMEBUFFER, "0");
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        spdlog::error("SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    // Request a conventional untagged framebuffer. Dear ImGui emits packed
    // display colors rather than linear-light values.
    SDL_GL_SetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, 0);
    _window = SDL_CreateWindow(
        "ggui", 1440, 900, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (_window == nullptr)
    {
        spdlog::error("SDL_CreateWindow failed: {}", SDL_GetError());
        SDL_Quit();
        return false;
    }
    _window_has_position = SDL_GetWindowPosition(_window, &_window_x, &_window_y);
    _window_has_size = SDL_GetWindowSize(_window, &_window_width, &_window_height);
    _gl_context = SDL_GL_CreateContext(_window);
    if (_gl_context == nullptr)
    {
        spdlog::error("SDL_GL_CreateContext failed: {}", SDL_GetError());
        SDL_DestroyWindow(_window);
        _window = nullptr;
        SDL_Quit();
        return false;
    }
    glDisable(GL_FRAMEBUFFER_SRGB);
    SDL_GL_SetSwapInterval(1);

    char* preferences = SettingsPersistenceEnabled() ? SDL_GetPrefPath("gg", "ggui") : nullptr;
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
    io.ConfigDpiScaleFonts = true;
    io.IniFilename = _imgui_ini_path.empty() ? nullptr : _imgui_ini_path.c_str();
    RegisterWindowSettings();
    if (io.IniFilename != nullptr && std::filesystem::exists(io.IniFilename))
        ImGui::LoadIniSettingsFromDisk(io.IniFilename);
    LoadUiFont();
    ApplyTheme();
    ImGui_ImplSDL3_InitForOpenGL(_window, _gl_context);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    return true;
}

void Application::RegisterWindowSettings()
{
    ImGuiSettingsHandler handler;
    handler.TypeName = "Ggui";
    handler.TypeHash = ImHashStr(handler.TypeName);
    handler.ReadOpenFn = WindowSettingsReadOpen;
    handler.ReadLineFn = WindowSettingsReadLine;
    handler.ApplyAllFn = WindowSettingsApplyAll;
    handler.WriteAllFn = WindowSettingsWriteAll;
    handler.UserData = this;
    ImGui::AddSettingsHandler(&handler);
}

void* Application::WindowSettingsReadOpen(
    ImGuiContext*, ImGuiSettingsHandler* handler, const char* name)
{
    if (std::strcmp(name, "MainWindow") != 0)
        return nullptr;
    auto* application = static_cast<Application*>(handler->UserData);
    application->_window_has_position = false;
    application->_window_has_size = false;
    application->_window_maximized = false;
    return application;
}

void Application::WindowSettingsReadLine(
    ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
{
    auto* application = static_cast<Application*>(entry);
    int first = 0;
    int second = 0;
    if (std::sscanf(line, "Pos=%d,%d", &first, &second) == 2)
    {
        application->_window_x = first;
        application->_window_y = second;
        application->_window_has_position = true;
    }
    else if (std::sscanf(line, "Size=%d,%d", &first, &second) == 2 && first >= 320 && second >= 240)
    {
        application->_window_width = first;
        application->_window_height = second;
        application->_window_has_size = true;
    }
    else if (std::sscanf(line, "Maximized=%d", &first) == 1)
        application->_window_maximized = first != 0;
}

void Application::WindowSettingsApplyAll(ImGuiContext*, ImGuiSettingsHandler* handler)
{
    auto* application = static_cast<Application*>(handler->UserData);
    if (application->_window == nullptr)
        return; // GCOV_EXCL_LINE: handler is registered only while the SDL window exists
    const SDL_WindowFlags flags = SDL_GetWindowFlags(application->_window);
    if ((flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) != 0)
        SDL_RestoreWindow(application->_window); // GCOV_EXCL_LINE: headless Xvfb has no window manager state to restore
    if (application->_window_has_size || application->_window_has_position)
    {
        int width = 0;
        int height = 0;
        int x = 0;
        int y = 0;
        SDL_GetWindowSize(application->_window, &width, &height);
        SDL_GetWindowPosition(application->_window, &x, &y);
        SDL_Rect desired{application->_window_has_position ? application->_window_x : x,
            application->_window_has_position ? application->_window_y : y,
            application->_window_has_size ? application->_window_width : width,
            application->_window_has_size ? application->_window_height : height};
        int display_count = 0;
        SDL_DisplayID* display_ids = SDL_GetDisplays(&display_count);
        std::vector<SDL_Rect> displays;
        displays.reserve(static_cast<std::size_t>(std::max(0, display_count)));
        for (int index = 0; index < display_count; ++index)
        {
            SDL_Rect usable{};
            if (SDL_GetDisplayUsableBounds(display_ids[index], &usable))
                displays.push_back(usable);
        }
        SDL_free(display_ids);
        const SDL_Rect fitted = FitWindowToDisplays(desired, displays);
        if (fitted.x != desired.x || fitted.y != desired.y || fitted.w != desired.w || fitted.h != desired.h)
            application->_default_layout = true;
        desired = fitted;
        application->_window_x = desired.x;
        application->_window_y = desired.y;
        application->_window_width = desired.w;
        application->_window_height = desired.h;
        SDL_SetWindowSize(application->_window, desired.w, desired.h);
        SDL_SetWindowPosition(application->_window, desired.x, desired.y);
    }
    if (application->_window_maximized)
        SDL_MaximizeWindow(application->_window);
}

void Application::WindowSettingsWriteAll(
    ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* output)
{
    auto* application = static_cast<Application*>(handler->UserData);
    if (application->_window != nullptr)
    {
        const SDL_WindowFlags flags = SDL_GetWindowFlags(application->_window);
        application->_window_maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
        if ((flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN)) == 0)
        {
            application->_window_has_position =
                SDL_GetWindowPosition(application->_window, &application->_window_x, &application->_window_y);
            application->_window_has_size =
                SDL_GetWindowSize(application->_window, &application->_window_width, &application->_window_height);
        }
    }
    output->appendf("[%s][MainWindow]\n", handler->TypeName);
    output->appendf("Pos=%d,%d\n", application->_window_x, application->_window_y);
    output->appendf("Size=%d,%d\n", application->_window_width, application->_window_height);
    output->appendf("Maximized=%d\n\n", application->_window_maximized ? 1 : 0);
}

void Application::Shutdown()
{
    SaveSettings();
    ClearConflictMerge();
    if (!_editor_temp_directory.empty())
    {
        std::error_code error;
        std::filesystem::remove_all(_editor_temp_directory, error);
    }
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

bool Application::SettingsPersistenceEnabled() const
{
#ifdef IMGUI_BUILD_TESTING
    return !_test_mode && !_smoke_mode;
#else
    return true;
#endif
}

void Application::LoadSettings()
{
    if (!SettingsPersistenceEnabled() || _settings_path.empty() || !std::filesystem::exists(_settings_path))
        return;
    try
    {
        std::ifstream input(_settings_path);
        nlohmann::json json;
        input >> json;
        _recent_repositories = json.value("recentRepositories", std::vector<std::string>{});
        _repository_visible_bookmarks = json.value("visibleBookmarks", decltype(_repository_visible_bookmarks){});
        _repository_selected_tags = json.value("selectedTags", decltype(_repository_selected_tags){});
        _repository_selected_remotes = json.value("selectedRemotes", decltype(_repository_selected_remotes){});
        _default_layout = json.value("defaultLayout", true);
        _diff_side_by_side = json.value("diffSideBySide", false);
        const int whitespace = json.value("diffWhitespaceMode", 0);
        _diff_whitespace_mode = WhitespaceModeFromIndex(whitespace);
        const int context_lines = json.value("diffContextLines", 3);
        _diff_context_lines =
            std::ranges::find(kDiffContextChoices, context_lines) == kDiffContextChoices.end() ? 3 : context_lines;
        _ui_scale_percent = std::clamp(json.value("uiScale", 100), 50, 300);
    }
    catch (const std::exception& error)
    {
        spdlog::warn("Could not load settings: {}", error.what());
    }
}

void Application::SaveSettings()
{
    if (!SettingsPersistenceEnabled() || _settings_path.empty())
        return; // GCOV_EXCL_LINE: SDL supplied no preferences directory
    try
    {
        std::filesystem::create_directories(_settings_path.parent_path());
        const nlohmann::json json{{"recentRepositories", _recent_repositories},
            {"visibleBookmarks", _repository_visible_bookmarks},
            {"selectedTags", _repository_selected_tags},
            {"selectedRemotes", _repository_selected_remotes}, {"defaultLayout", _default_layout},
            {"diffSideBySide", _diff_side_by_side}, {"diffWhitespaceMode", WhitespaceModeIndex(_diff_whitespace_mode)},
            {"diffContextLines", _diff_context_lines}, {"uiScale", _ui_scale_percent}};
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

void Application::ForgetRepository(const std::string& path)
{
    std::erase(_recent_repositories, path);
    SaveSettings();
}

} // namespace Ggui
