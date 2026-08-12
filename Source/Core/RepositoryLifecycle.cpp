// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "RepositoryEngineInternal.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace Ggui
{
using namespace RepositoryInternal;

RepositoryEngine::Impl::Impl()
{
    if (git_libgit2_init() < 0)
        throw std::runtime_error("initialize libgit2: " + LastGitError("unknown error")); // GCOV_EXCL_LINE: forced libgit2 bootstrap failure
    try
    {
        worker = std::thread([this] { Run(); });
    }
    catch (...) // GCOV_EXCL_START: forced standard-library thread construction failure
    {
        git_libgit2_shutdown();
        throw;
    }
    // GCOV_EXCL_STOP
}

RepositoryEngine::Impl::~Impl()
{
    {
        std::lock_guard lock(queue_mutex);
        stopping = true;
    }
    {
        std::lock_guard lock(credential_mutex);
        credential_cancelled = true;
    }
    queue_cv.notify_all();
    credential_cv.notify_all();
    if (worker.joinable())
        worker.join();
    Close();
    git_libgit2_shutdown();
}

void RepositoryEngine::Impl::Close()
{
    watcher.Clear();
    if (gg != nullptr)
        gg_repository_free(gg);
    gg = nullptr;
    git.reset();
}

void RepositoryEngine::Impl::Post(Event event)
{
    std::lock_guard lock(event_mutex);
    events.push_back(std::move(event));
}

int RepositoryEngine::Impl::CancelCallback(void* payload)
{
    return static_cast<Impl*>(payload)->cancel_requested.load() ? 1 : 0;
}

void RepositoryEngine::Impl::ProgressCallback(const char* phase, size_t completed, size_t total, void* payload)
{
    static_cast<Impl*>(payload)->Post(
        OperationProgress{phase == nullptr ? "" : phase, completed, total});
}

gg_operation_options RepositoryEngine::Impl::OperationOptions(bool report_progress)
{
    gg_operation_options options = GG_OPERATION_OPTIONS_INIT;
    options.cancel_cb = CancelCallback;
    options.progress_cb = report_progress ? ProgressCallback : nullptr;
    options.payload = this;
    return options;
}

int RepositoryEngine::Impl::CredentialCallback(
    git_credential** out, const char* url, const char* username, unsigned int allowed, void* payload)
{
    return static_cast<Impl*>(payload)->AcquireCredential(out, url, username, allowed);
}

int RepositoryEngine::Impl::AcquireCredential(git_credential** out, const char* url, const char* username, unsigned int allowed)
{
    if (++credential_attempts > 3)
        return GIT_EAUTH;
    if ((allowed & GIT_CREDENTIAL_DEFAULT) != 0)
    {
        if (git_credential_default_new(out) == GIT_OK)
            return GIT_OK;
    }
    if (!ssh_agent_attempted && (allowed & GIT_CREDENTIAL_SSH_KEY) != 0)
    {
        ssh_agent_attempted = true;
        const char* user = username != nullptr && *username != '\0' ? username : "git";
        if (git_credential_ssh_key_from_agent(out, user) == GIT_OK)
            return GIT_OK;
    }

    {
        std::lock_guard lock(credential_mutex);
        credential_response.reset();
        credential_cancelled = false;
    }
    Post(CredentialRequest{url == nullptr ? "" : url, username == nullptr ? "" : username, allowed});

    std::unique_lock lock(credential_mutex);
    credential_cv.wait(lock, [this] {
        return credential_response.has_value() || credential_cancelled || stopping || cancel_requested.load();
    });
    if (!credential_response.has_value())
        return GIT_EUSER;
    CredentialResponse response = std::move(*credential_response);
    credential_response.reset();
    lock.unlock();

    int result = GIT_EUSER;
    if (response.method == CredentialResponse::Method::UserPass && (allowed & GIT_CREDENTIAL_USERPASS_PLAINTEXT))
    {
        result = git_credential_userpass_plaintext_new(out, response.username.c_str(), response.secret.c_str());
    }
    else if (response.method == CredentialResponse::Method::SshAgent && (allowed & GIT_CREDENTIAL_SSH_KEY))
    {
        const char* user = response.username.empty() ? "git" : response.username.c_str();
        result = git_credential_ssh_key_from_agent(out, user);
    }
    else if (response.method == CredentialResponse::Method::SshKey && (allowed & GIT_CREDENTIAL_SSH_KEY))
    {
        const char* user = response.username.empty() ? "git" : response.username.c_str();
        const char* public_key = response.public_key.empty() ? nullptr : response.public_key.c_str();
        result = git_credential_ssh_key_new(
            out, user, public_key, response.private_key.c_str(), response.secret.c_str());
    }
    std::fill(response.secret.begin(), response.secret.end(), '\0');
    return result;
}

int RepositoryEngine::Impl::TransferProgress(const git_indexer_progress* stats, void* payload)
{
    auto* self = static_cast<Impl*>(payload);
    self->Post(OperationProgress{self->transfer_phase, stats->received_objects, stats->total_objects});
    return self->cancel_requested.load() ? GIT_EUSER : GIT_OK;
}

void RepositoryEngine::Impl::Attach(GitRepositoryPtr repository)
{
    Close();
    git = std::move(repository);
    try
    {
        Check(gg_repository_attach(&gg, git.get()), "attach gg repository");
        Sync();
        const char* workdir = git_repository_workdir(git.get());
        watcher.Watch(
            workdir == nullptr ? git_repository_path(git.get()) : workdir, git_repository_commondir(git.get()));
        PublishSnapshot();
    }
    catch (...)
    {
        Close();
        throw;
    }
}

void RepositoryEngine::Impl::OpenPath(const std::string& path)
{
    git_repository* raw = nullptr;
    Check(git_repository_open_ext(&raw, path.c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr), "open repository");
    Attach(GitRepositoryPtr(raw));
}

void RepositoryEngine::Impl::InitPath(const std::string& path)
{
    git_repository_init_options options = GIT_REPOSITORY_INIT_OPTIONS_INIT;
    options.flags = GIT_REPOSITORY_INIT_MKPATH;
    git_repository* raw = nullptr;
    Check(git_repository_init_ext(&raw, path.c_str(), &options), "initialize repository");
    Attach(GitRepositoryPtr(raw));
}

void RepositoryEngine::Impl::ClonePath(const CloneRepository& command)
{
    namespace fs = std::filesystem;
    const fs::path destination = fs::absolute(command.path);
    if (fs::exists(destination))
        throw std::runtime_error("clone destination must not exist");
    const fs::path temporary = destination.string() + ".ggui-clone";
    if (fs::exists(temporary))
        throw std::runtime_error("temporary clone destination already exists: " + temporary.string());

    transfer_phase = "clone";
    git_clone_options options = GIT_CLONE_OPTIONS_INIT;
    options.fetch_opts.callbacks.credentials = CredentialCallback;
    options.fetch_opts.callbacks.transfer_progress = TransferProgress;
    options.fetch_opts.callbacks.payload = this;
    ssh_agent_attempted = false;
    credential_attempts = 0;
    git_repository* raw = nullptr;
    const int result = git_clone(&raw, command.url.c_str(), temporary.string().c_str(), &options);
    if (result < 0)
    {
        if (raw != nullptr)
            git_repository_free(raw); // GCOV_EXCL_LINE: libgit2 normally returns null on clone failure
        std::error_code ignored;
        fs::remove_all(temporary, ignored);
        Check(result, "clone repository");
    }
    GitRepositoryPtr cloned(raw);
    cloned.reset();
    FinishClone(temporary, destination);
    OpenPath(destination.string());
}

} // namespace Ggui
