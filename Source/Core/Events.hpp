// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "Model.hpp"

#include <memory>
#include <string>
#include <variant>

namespace Ggui
{

struct SnapshotReady { std::shared_ptr<const RepoSnapshot> snapshot; };
struct DiffReady { DiffResult diff; };
struct FileContentReady { std::string revision; std::string path; std::string contents; };
struct OperationStarted { std::string name; };
struct OperationProgress
{
    std::string phase;
    std::size_t completed = 0;
    std::size_t total = 0;
};
struct OperationFinished { std::string name; };
struct ErrorEvent { std::string operation; std::string message; };
struct CredentialRequest
{
    std::string url;
    std::string username;
    unsigned int allowed_types = 0;
};

using Event = std::variant<SnapshotReady, DiffReady, FileContentReady, OperationStarted, OperationProgress,
    OperationFinished, ErrorEvent, CredentialRequest>;

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
