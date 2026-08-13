// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <imgui_stdlib.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace Ggui
{
using namespace ApplicationInternal;

namespace
{

std::size_t ScopeIndex(ConfigScope scope)
{
    return static_cast<std::size_t>(scope);
}

std::string InheritedHint(const MaxNewFileSizeValues& values, ConfigScope scope)
{
    const InheritedConfigValue inherited = InheritedMaxNewFileSize(values, scope);
    return inherited.value + " (" + (inherited.source.has_value()
        ? ConfigScopeName(*inherited.source) : "Default") + ")";
}

} // namespace

void Application::OpenSettings()
{
    if (!_show_settings)
    {
        _settings_select_user = true;
        _settings_repository.clear();
        ReloadNativeSettings();
    }
    _show_settings = true;
    ImGui::SetNextWindowFocus();
}

void Application::ReloadNativeSettings()
{
    const std::optional<std::filesystem::path> repository = _snapshot == nullptr
        ? std::nullopt : std::optional<std::filesystem::path>(_snapshot->root);
    _settings_repository = _snapshot == nullptr ? "" : _snapshot->root;
    try
    {
        _max_new_file_size_values = ReadMaxNewFileSizeValues(repository);
        for (std::size_t index = 0; index < _max_new_file_size_inputs.size(); ++index)
        {
            _max_new_file_size_inputs[index] = _max_new_file_size_values[index].value_or("");
            _max_new_file_size_errors[index] = _max_new_file_size_inputs[index].empty()
                || ParseFileSize(_max_new_file_size_inputs[index]).has_value()
                ? "" : "Enter unsigned bytes or a binary size such as 1MiB.";
        }
    }
    catch (const std::exception& error)
    {
        _error_message = error.what();
    }
}

void Application::RenderSettings()
{
    const std::string repository = _snapshot == nullptr ? "" : _snapshot->root;
    if (_settings_repository != repository)
        ReloadNativeSettings();

    ImGui::SetNextWindowSize(ImVec2(FontPx(520.0f), FontPx(280.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &_show_settings))
    {
        ImGui::End();
        return;
    }

    // UI scale
    ImGui::SetNextItemWidth(FontPx(260.0f));
    if (ImGui::SliderInt("UI scale", &_ui_scale_percent, 50, 300, "%d%%", ImGuiSliderFlags_AlwaysClamp))
        ApplyTheme();
    if (ImGui::IsItemDeactivatedAfterEdit())
        SaveSettings();
    ImGui::Separator();

    // Native configuration scopes
    const bool repository_available = _snapshot != nullptr;
    if (ImGui::BeginTabBar("Settings scopes"))
    {
        constexpr std::array scopes{ConfigScope::User, ConfigScope::Repository, ConfigScope::Workspace};
        for (ConfigScope scope : scopes)
        {
            const bool available = scope == ConfigScope::User || repository_available;
            ImGui::BeginDisabled(!available);
            const ImGuiTabItemFlags flags = scope == ConfigScope::User && _settings_select_user
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            const bool open = ImGui::BeginTabItem(ConfigScopeName(scope), nullptr, flags);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !available)
                ImGui::SetTooltip("Open a repository to edit this scope.");
            if (open)
            {
                // Maximum automatically tracked file size
                const std::size_t index = ScopeIndex(scope);
                ImGui::TextUnformatted("Maximum size for automatically tracked new files");
                ImGui::TextDisabled("Use bytes or binary suffixes (K, KB, KiB, MiB, GiB). 0 means unlimited.");
                const std::string hint = InheritedHint(_max_new_file_size_values, scope);
                ImGui::SetNextItemWidth(-1.0f);
                const std::string label = "##Maximum new file size " + std::string(ConfigScopeName(scope));
                if (ImGui::InputTextWithHint(label.c_str(), hint.c_str(), &_max_new_file_size_inputs[index]))
                {
                    _max_new_file_size_errors[index] = _max_new_file_size_inputs[index].empty()
                        || ParseFileSize(_max_new_file_size_inputs[index]).has_value()
                        ? "" : "Enter unsigned bytes or a binary size such as 1MiB.";
                }

                // Persist valid edits
                if (ImGui::IsItemDeactivatedAfterEdit() && _max_new_file_size_errors[index].empty())
                {
                    const std::optional<std::string> value = _max_new_file_size_inputs[index].empty()
                        ? std::nullopt : std::optional(_max_new_file_size_inputs[index]);
                    try
                    {
                        WriteMaxNewFileSizeValue(_snapshot == nullptr
                                ? std::nullopt : std::optional<std::filesystem::path>(_snapshot->root),
                            scope, value);
                        _max_new_file_size_values[index] = value;
                        if (_snapshot != nullptr)
                            _engine.Enqueue(Refresh{});
                    }
                    catch (const std::exception& error)
                    {
                        _error_message = error.what();
                    }
                }

                // Validation error
                if (!_max_new_file_size_errors[index].empty())
                    ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.35f, 1.0f), "%s",
                        _max_new_file_size_errors[index].c_str());
                ImGui::EndTabItem();
            }
            ImGui::EndDisabled();
        }
        ImGui::EndTabBar();
    }
    _settings_select_user = false;
    if (!repository_available)
        ImGui::TextDisabled("Open a repository to configure Repository and Workspace overrides.");
    ImGui::End();
}

} // namespace Ggui
