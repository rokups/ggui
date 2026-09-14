// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "Settings.hpp"

#include <gg/gg.h>
#if __has_include(<git2-experimental/sys/errors.h>)
#include <git2-experimental/sys/errors.h>
#else
#include <git2/sys/errors.h>
#endif

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>

namespace Ggui
{
namespace
{

template <typename Type, void (*Free)(Type*)>
struct GitDeleter
{
    void operator()(Type* value) const { Free(value); }
};

template <typename Type, void (*Free)(Type*)>
using GitPtr = std::unique_ptr<Type, GitDeleter<Type, Free>>;

void Check(int result, std::string_view action)
{
    if (result >= 0)
        return;
    const git_error* error = git_error_last();
    throw std::runtime_error(std::string(action) + ": "
        + (error == nullptr || error->message == nullptr ? "libgit2 error" : error->message));
}

std::size_t ScopeIndex(ConfigScope scope)
{
    return static_cast<std::size_t>(scope);
}

std::filesystem::path GlobalConfigPath()
{
    git_buf path = GIT_BUF_INIT;
    const int found = git_config_find_global(&path);
    if (found == GIT_ENOTFOUND)
    {
        git_error_clear();
        Check(git_libgit2_opts(GIT_OPT_GET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, &path),
            "locate user Git configuration directory");
#ifdef _WIN32
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        std::string_view directories(path.ptr == nullptr ? "" : path.ptr, path.size);
        const std::size_t end = directories.find(separator);
        directories = directories.substr(0, end);
        if (directories.empty())
        {
            git_buf_dispose(&path);
            throw std::runtime_error("user Git configuration directory is unavailable");
        }
        const std::filesystem::path result = std::filesystem::path(directories) / ".gitconfig";
        git_buf_dispose(&path);
        return result;
    }
    Check(found, "locate user Git configuration");
    const std::filesystem::path result(path.ptr == nullptr ? "" : path.ptr);
    git_buf_dispose(&path);
    return result;
}

struct RepositoryConfigPaths
{
    std::filesystem::path repository;
    std::filesystem::path workspace;
};

RepositoryConfigPaths ConfigPaths(const std::filesystem::path& path)
{
    git_repository* raw_repository = nullptr;
    Check(git_repository_open_ext(&raw_repository, path.string().c_str(), 0, nullptr), "open repository configuration");
    GitPtr<git_repository, git_repository_free> repository(raw_repository);
    return {std::filesystem::path(git_repository_commondir(repository.get())) / "config",
        std::filesystem::path(git_repository_path(repository.get())) / "config.worktree"};
}

std::filesystem::path ConfigPath(
    const std::optional<std::filesystem::path>& repository, ConfigScope scope)
{
    if (scope == ConfigScope::User)
        return GlobalConfigPath();
    if (!repository.has_value())
        throw std::runtime_error("repository configuration is unavailable without an open repository");
    const RepositoryConfigPaths paths = ConfigPaths(*repository);
    return scope == ConfigScope::Repository ? paths.repository : paths.workspace;
}

std::optional<std::string> ReadValue(
    const std::filesystem::path& path, ConfigScope scope, std::string_view key, std::string_view action)
{
    git_config* raw_config = nullptr;
    Check(git_config_new(&raw_config), "create Git configuration view");
    GitPtr<git_config, git_config_free> config(raw_config);
    const std::string path_text = path.string();
    const git_config_level_t level = scope == ConfigScope::User ? GIT_CONFIG_LEVEL_GLOBAL
        : scope == ConfigScope::Repository ? GIT_CONFIG_LEVEL_LOCAL : GIT_CONFIG_LEVEL_WORKTREE;
    const int opened = git_config_add_file_ondisk(config.get(), path_text.c_str(), level, nullptr, 0);
    if (opened == GIT_ENOTFOUND)
    {
        git_error_clear();
        return std::nullopt;
    }
    Check(opened, "open Git configuration");
    git_buf value = GIT_BUF_INIT;
    const int read = git_config_get_string_buf(&value, config.get(), key.data());
    if (read == GIT_ENOTFOUND)
    {
        git_error_clear();
        return std::nullopt;
    }
    Check(read, action);
    std::string result(value.ptr == nullptr ? "" : value.ptr, value.size);
    git_buf_dispose(&value);
    return result;
}

void EnableWorktreeConfig(const std::filesystem::path& repository)
{
    const std::filesystem::path path = ConfigPaths(repository).repository;
    std::filesystem::create_directories(path.parent_path());
    git_config* raw_config = nullptr;
    Check(git_config_new(&raw_config), "create repository Git configuration view");
    GitPtr<git_config, git_config_free> config(raw_config);
    const std::string path_text = path.string();
    Check(git_config_add_file_ondisk(
              config.get(), path_text.c_str(), GIT_CONFIG_LEVEL_LOCAL, nullptr, 1),
        "open repository Git configuration");
    Check(git_config_set_bool(config.get(), "extensions.worktreeConfig", 1),
        "enable worktree Git configuration");
}

ConfigValues ReadValues(const std::optional<std::filesystem::path>& repository,
    std::string_view key, std::string_view action)
{
    ConfigValues values;
    values[ScopeIndex(ConfigScope::User)] = ReadValue(GlobalConfigPath(), ConfigScope::User, key, action);
    if (repository.has_value())
    {
        const RepositoryConfigPaths paths = ConfigPaths(*repository);
        values[ScopeIndex(ConfigScope::Repository)] = ReadValue(paths.repository, ConfigScope::Repository, key, action);
        values[ScopeIndex(ConfigScope::Workspace)] = ReadValue(paths.workspace, ConfigScope::Workspace, key, action);
    }
    return values;
}

void WriteValue(const std::optional<std::filesystem::path>& repository, ConfigScope scope,
    std::string_view key, const std::optional<std::string>& value,
    std::string_view set_action, std::string_view unset_action)
{
    if (scope == ConfigScope::Workspace)
    {
        if (!repository.has_value())
            throw std::runtime_error("workspace configuration is unavailable without an open repository");
        EnableWorktreeConfig(*repository);
    }
    const std::filesystem::path path = ConfigPath(repository, scope);
    std::filesystem::create_directories(path.parent_path());
    git_config* raw_config = nullptr;
    Check(git_config_new(&raw_config), "create Git configuration view");
    GitPtr<git_config, git_config_free> config(raw_config);
    const std::string path_text = path.string();
    const git_config_level_t level = scope == ConfigScope::User ? GIT_CONFIG_LEVEL_GLOBAL
        : scope == ConfigScope::Repository ? GIT_CONFIG_LEVEL_LOCAL : GIT_CONFIG_LEVEL_WORKTREE;
    Check(git_config_add_file_ondisk(config.get(), path_text.c_str(), level, nullptr, 1),
        "open Git configuration");
    if (value.has_value())
        Check(git_config_set_string(config.get(), key.data(), value->c_str()), set_action);
    else
    {
        const int result = git_config_delete_entry(config.get(), key.data());
        if (result == GIT_ENOTFOUND)
            git_error_clear();
        else
            Check(result, unset_action);
    }
}

InheritedConfigValue InheritedValue(
    const ConfigValues& values, ConfigScope scope, std::string_view default_value)
{
    for (int index = static_cast<int>(scope) - 1; index >= 0; --index)
        if (values[static_cast<std::size_t>(index)].has_value())
            return {*values[static_cast<std::size_t>(index)], static_cast<ConfigScope>(index)};
    return {std::string(default_value), std::nullopt};
}

} // namespace

std::optional<std::uint64_t> ParseFileSize(std::string_view value)
{
    const std::size_t suffix_begin = value.find_first_not_of("0123456789");
    const std::string_view number = value.substr(0, suffix_begin);
    if (number.empty())
        return std::nullopt;
    std::uint64_t bytes = 0;
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), bytes);
    if (error != std::errc{} || end != number.data() + number.size())
        return std::nullopt;
    std::string suffix(suffix_begin == std::string_view::npos ? std::string_view{} : value.substr(suffix_begin));
    std::ranges::transform(suffix, suffix.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    std::uint64_t multiplier = 1;
    if (suffix.empty() || suffix == "B") multiplier = 1;
    else if (suffix == "K" || suffix == "KB" || suffix == "KIB") multiplier = 1024;
    else if (suffix == "M" || suffix == "MB" || suffix == "MIB") multiplier = 1024 * 1024;
    else if (suffix == "G" || suffix == "GB" || suffix == "GIB") multiplier = 1024ULL * 1024 * 1024;
    else return std::nullopt;
    if (bytes > std::numeric_limits<std::uint64_t>::max() / multiplier)
        return std::nullopt;
    return bytes * multiplier;
}

MaxNewFileSizeValues ReadMaxNewFileSizeValues(const std::optional<std::filesystem::path>& repository)
{
    return ReadValues(repository, kMaxNewFileSizeKey, "read snapshot.max-new-file-size");
}

void WriteMaxNewFileSizeValue(const std::optional<std::filesystem::path>& repository,
    ConfigScope scope, const std::optional<std::string>& value)
{
    if (value.has_value() && !ParseFileSize(*value).has_value())
        throw std::invalid_argument("invalid snapshot.max-new-file-size");
    WriteValue(repository, scope, kMaxNewFileSizeKey, value,
        "set snapshot.max-new-file-size", "unset snapshot.max-new-file-size");
}

InheritedConfigValue InheritedMaxNewFileSize(const MaxNewFileSizeValues& values, ConfigScope scope)
{
    return InheritedValue(values, scope, kDefaultMaxNewFileSize);
}

EditorValues ReadEditorValues(const std::optional<std::filesystem::path>& repository)
{
    return ReadValues(repository, kEditorKey, "read core.editor");
}

void WriteEditorValue(const std::optional<std::filesystem::path>& repository,
    ConfigScope scope, const std::optional<std::string>& value)
{
    WriteValue(repository, scope, kEditorKey, value, "set core.editor", "unset core.editor");
}

InheritedConfigValue InheritedEditor(const EditorValues& values, ConfigScope scope)
{
    return InheritedValue(values, scope, {});
}

std::string EffectiveEditor(const EditorValues& values)
{
    for (auto value = values.rbegin(); value != values.rend(); ++value)
        if (value->has_value())
            return **value;
    return {};
}

const char* ConfigScopeName(ConfigScope scope)
{
    switch (scope)
    {
    case ConfigScope::User: return "User";
    case ConfigScope::Repository: return "Repository";
    case ConfigScope::Workspace: return "Workspace";
    }
    return "Default";
}

} // namespace Ggui
