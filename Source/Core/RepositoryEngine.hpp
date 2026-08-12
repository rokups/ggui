// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "Commands.hpp"
#include "Events.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Ggui
{

class RepositoryEngine
{
public:
    RepositoryEngine();
    ~RepositoryEngine();
    RepositoryEngine(const RepositoryEngine&) = delete;
    RepositoryEngine& operator=(const RepositoryEngine&) = delete;

    void Enqueue(Command command);
    std::vector<Event> PollEvents();
    void Cancel();
    void SubmitCredential(CredentialResponse response);
    void CancelCredential();
#ifdef GGUI_TESTING
    void ResetCredentialStateForTest(bool ssh_agent_attempted = false, int credential_attempts = 0);
    int AcquireCredentialForTest(
        git_credential** out, const char* url, const char* username, unsigned int allowed_types);
    int TransferProgressForTest(const git_indexer_progress& progress);
    void DispatchMutationForTest(const Command& command);
    void SetCommandsSuppressedForTest(bool suppressed);
    static std::vector<Conflict> ConvertConflictsForTest();
    static void FinishCloneForTest(const std::filesystem::path& temporary, const std::filesystem::path& destination);
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

std::string ShortId(const std::string& value, std::size_t length = 8);
std::vector<std::size_t> UniquePrefixLengths(const std::vector<std::string>& values, std::size_t minimum = 1);
void MarkPushedRevisions(
    std::vector<Revision>& revisions, const std::vector<NamedRef>& refs, git_repository* repository = nullptr);
std::string FirstLine(const std::string& value);

} // namespace Ggui
