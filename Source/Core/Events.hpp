// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "Model.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

namespace Ggui
{

struct SnapshotReady { std::shared_ptr<const RepoSnapshot> snapshot; };
struct HistoryReady { std::shared_ptr<const HistoryView> view; };
struct ChangedFilesReady
{
    std::string revision;
    std::string compare_to;
    bool file_comparison = false;
    std::string selected_path;
    std::vector<StatusEntry> files;
    DiffOptions options;
};
struct DiffReady { DiffResult diff; };
struct FileContentReady { std::string revision; std::string path; std::string contents; };
struct BackgroundActivityStarted { std::uint64_t id = 0; std::string name; };
struct BackgroundActivityFinished { std::uint64_t id = 0; };
struct OperationStarted { std::string name; };
struct OperationProgress
{
    std::string phase;
    std::size_t completed = 0;
    std::size_t total = 0;
};
struct OperationFinished { std::string name; };
struct ErrorEvent { std::string operation; std::string message; };
struct WorkspaceRemoved { std::string root; bool current = false; };
struct CredentialRequest
{
    std::string url;
    std::string username;
    unsigned int allowed_types = 0;
};

using Event = std::variant<SnapshotReady, HistoryReady, ChangedFilesReady, DiffReady,
    FileContentReady, BackgroundActivityStarted, BackgroundActivityFinished, OperationStarted,
    OperationProgress, OperationFinished, ErrorEvent, WorkspaceRemoved, CredentialRequest>;

struct CredentialResponse
{
    enum class Method
    {
        UserPass,
        SshAgent,
        SshKey,
    };
    Method method = Method::UserPass;
    std::string username;
    std::string secret;
    std::string private_key;
    std::string public_key;
};

} // namespace Ggui
