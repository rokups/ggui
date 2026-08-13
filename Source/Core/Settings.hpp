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

using MaxNewFileSizeValues = std::array<std::optional<std::string>, 3>;

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
const char* ConfigScopeName(ConfigScope scope);

} // namespace Ggui
