// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>

namespace Ggui::ApplicationInternal
{

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

const TextEditor::Language* DiffLanguage(const std::string& path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (extension == ".c") return TextEditor::Language::C();
    if (extension == ".cc" || extension == ".cpp" || extension == ".cxx" || extension == ".h"
        || extension == ".hh" || extension == ".hpp" || extension == ".hxx")
        return TextEditor::Language::Cpp();
    if (extension == ".cs") return TextEditor::Language::Cs();
    if (extension == ".as") return TextEditor::Language::AngelScript();
    if (extension == ".lua") return TextEditor::Language::Lua();
    if (extension == ".py") return TextEditor::Language::Python();
    if (extension == ".glsl" || extension == ".vert" || extension == ".frag")
        return TextEditor::Language::Glsl();
    if (extension == ".hlsl") return TextEditor::Language::Hlsl();
    if (extension == ".json") return TextEditor::Language::Json();
    if (extension == ".md") return TextEditor::Language::Markdown();
    if (extension == ".sql") return TextEditor::Language::Sql();
    return nullptr;
}

int WhitespaceModeIndex(DiffWhitespaceMode mode)
{
    switch (mode)
    {
    case DiffWhitespaceMode::IgnoreWhitespace: return 1;
    case DiffWhitespaceMode::IgnoreAllWhitespace: return 2;
    default: return 0;
    }
}

DiffWhitespaceMode WhitespaceModeFromIndex(int index)
{
    switch (index)
    {
    case 1: return DiffWhitespaceMode::IgnoreWhitespace;
    case 2: return DiffWhitespaceMode::IgnoreAllWhitespace;
    default: return DiffWhitespaceMode::Normal;
    }
}

int ContextLineChoiceIndex(int context_lines)
{
    const auto choice = std::ranges::find(kDiffContextChoices, context_lines);
    return choice == kDiffContextChoices.end() ? 2 : static_cast<int>(choice - kDiffContextChoices.begin());
}

bool IsImagePath(const std::string& path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::ranges::transform(
        extension, extension.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    constexpr std::array<std::string_view, 9> extensions{
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".tga", ".svg", ".ico"};
    return std::ranges::find(extensions, extension) != extensions.end();
}

bool IsSymlinkMode(unsigned int mode)
{
    return (mode & 0170000U) == 0120000U;
}

bool IsSubmoduleMode(unsigned int mode)
{
    return (mode & 0170000U) == 0160000U;
}

std::string FormatFileMode(unsigned int mode)
{
    if (mode == 0)
        return "none";
    std::array<char, 16> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%06o", mode);
    return buffer.data();
}

const char* ModeKind(unsigned int mode)
{
    if (IsSubmoduleMode(mode))
        return "submodule";
    if (IsSymlinkMode(mode))
        return "symlink";
    if ((mode & 0111U) != 0)
        return "executable file";
    return mode == 0 ? "none" : "file";
}

} // namespace Ggui::ApplicationInternal
