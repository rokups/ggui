// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Ggui
{

enum class ConfigScope { User, Repository, Workspace };

inline constexpr std::string_view kMaxNewFileSizeKey = "snapshot.max-new-file-size";
inline constexpr std::string_view kDefaultMaxNewFileSize = "1MiB";
inline constexpr std::string_view kEditorKey = "core.editor";

using ConfigValues = std::array<std::optional<std::string>, 3>;
using MaxNewFileSizeValues = ConfigValues;
using EditorValues = ConfigValues;

struct InheritedConfigValue
{
    std::string value;
    std::optional<ConfigScope> source;
};

std::optional<std::uint64_t> ParseFileSize(std::string_view value);
MaxNewFileSizeValues ReadMaxNewFileSizeValues(const std::optional<std::filesystem::path>& repository);
void WriteMaxNewFileSizeValue(const std::optional<std::filesystem::path>& repository,
    ConfigScope scope, const std::optional<std::string>& value);
InheritedConfigValue InheritedMaxNewFileSize(const MaxNewFileSizeValues& values, ConfigScope scope);
EditorValues ReadEditorValues(const std::optional<std::filesystem::path>& repository);
void WriteEditorValue(const std::optional<std::filesystem::path>& repository,
    ConfigScope scope, const std::optional<std::string>& value);
InheritedConfigValue InheritedEditor(const EditorValues& values, ConfigScope scope);
std::string EffectiveEditor(const EditorValues& values);
const char* ConfigScopeName(ConfigScope scope);

} // namespace Ggui
