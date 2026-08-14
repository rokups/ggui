// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <SDL3/SDL_opengl.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl3.h>
#ifdef IMGUI_BUILD_TESTING
#include <imgui_te_engine.h>
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
    if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr && _active_operation.empty())
        _engine.Enqueue(OpenRepository{event.drop.data});
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
    }
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
    if (application->_window_has_size)
        SDL_SetWindowSize(application->_window, application->_window_width, application->_window_height);
    if (application->_window_has_position)
        SDL_SetWindowPosition(application->_window, application->_window_x, application->_window_y);
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
    if (_settings_path.empty())
        return; // GCOV_EXCL_LINE: SDL supplied no preferences directory
    try
    {
        std::filesystem::create_directories(_settings_path.parent_path());
        const nlohmann::json json{{"recentRepositories", _recent_repositories}, {"defaultLayout", _default_layout},
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

} // namespace Ggui
