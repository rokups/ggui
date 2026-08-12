// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "RepositoryEngineInternal.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace Ggui
{
using namespace RepositoryInternal;

void RepositoryEngine::Impl::Execute(const Command& command)
{
    const std::string name = CommandName(command);
    const bool quiet = std::holds_alternative<Refresh>(command) || std::holds_alternative<LoadDiff>(command);
    if (!quiet)
        Post(OperationStarted{name});
    cancel_requested = false;
    if (std::holds_alternative<OpenRepository>(command) || std::holds_alternative<InitRepository>(command)
        || std::holds_alternative<CloneRepository>(command))
        Close();
    try
    {
        if (const auto* value = std::get_if<OpenRepository>(&command))
            OpenPath(value->path);
        else if (std::holds_alternative<CloseRepository>(command))
        {
            Close();
            {
                std::lock_guard lock(queue_mutex);
                std::erase_if(commands, [](const Command& queued) {
                    return std::holds_alternative<LoadDiff>(queued) || std::holds_alternative<Refresh>(queued);
                });
            }
            {
                std::lock_guard lock(event_mutex);
                std::erase_if(events, [](const Event& queued) {
                    return std::holds_alternative<SnapshotReady>(queued) || std::holds_alternative<DiffReady>(queued);
                });
            }
        }
        else if (const auto* value = std::get_if<InitRepository>(&command))
            InitPath(value->path);
        else if (const auto* value = std::get_if<CloneRepository>(&command))
            ClonePath(*value);
        else if (const auto* value = std::get_if<Refresh>(&command))
        {
            if (value->snapshot_working_copy)
                Sync(false);
            PublishSnapshot();
        }
        else if (const auto* value = std::get_if<Fetch>(&command))
            FetchRemote(*value);
        else if (const auto* value = std::get_if<Push>(&command))
            PushBookmark(*value);
        else if (const auto* value = std::get_if<LoadDiff>(&command))
            LoadPatch(*value);
        else if (const auto* value = std::get_if<ApplyPatch>(&command))
            ApplyPatchText(*value);
        else if (const auto* value = std::get_if<RevertFile>(&command))
            RevertFileChange(*value);
        else if (const auto* value = std::get_if<DeleteFile>(&command))
            DeleteWorkingFile(*value);
        else
            DispatchMutation(command);
        if (!quiet)
            Post(OperationFinished{name});
    }
    catch (const std::exception& error)
    {
        spdlog::error("{} failed: {}", name, error.what());
        Post(ErrorEvent{name, error.what()});
    }
}

void RepositoryEngine::Impl::Run()
{
    while (true)
    {
        std::optional<Command> command;
        {
            std::unique_lock lock(queue_mutex);
            queue_cv.wait(lock, [this] { return stopping || !commands.empty() || watcher.Changed(); });
            if (stopping)
                break;
            if (!commands.empty())
            {
                command = std::move(commands.front());
                commands.pop_front();
            }
        }
        if (command.has_value())
            Execute(*command);
        else if (gg != nullptr && !test_commands_suppressed)
        {
            const RepositoryWatcher::Changes changes = watcher.ConsumeChanges();
            if (changes.worktree || changes.metadata)
                Execute(Refresh{changes.worktree});
        }
    }
}

} // namespace Ggui
