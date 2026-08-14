// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "RepositoryEngineInternal.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Ggui
{
using namespace RepositoryInternal;

git_remote_callbacks RepositoryEngine::Impl::RemoteCallbacks()
{
    git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;
    callbacks.credentials = CredentialCallback;
    callbacks.transfer_progress = TransferProgress;
    callbacks.payload = this;
    return callbacks;
}

void RepositoryEngine::Impl::FetchRemote(const Fetch& command)
{
    Sync();
    transfer_phase = command.tracked_only ? "pull" : "fetch";
    git_remote* raw_remote = nullptr;
    Check(git_remote_lookup(&raw_remote, git.get(), command.remote.c_str()), "find remote");
    std::unique_ptr<git_remote, decltype(&git_remote_free)> remote(raw_remote, git_remote_free);
    ssh_agent_attempted = false;
    credential_attempts = 0;
    git_remote_callbacks callbacks = RemoteCallbacks();
    Check(git_remote_connect(remote.get(), GIT_DIRECTION_FETCH, &callbacks, nullptr, nullptr),
        "connect to remote");
    const git_remote_head** heads = nullptr;
    std::size_t head_count = 0;
    const int list_result = git_remote_ls(&heads, &head_count, remote.get());
    if (list_result < 0)
    {
        git_remote_disconnect(remote.get());
        Check(list_result, "list remote refs");
    }
    struct Advertised
    {
        std::string name;
        git_oid target{};
        gg_remote_ref_kind kind = GG_REMOTE_BRANCH;
    };
    std::vector<Advertised> advertised;
    advertised.reserve(head_count);
    constexpr std::string_view branch_prefix = "refs/heads/";
    constexpr std::string_view tag_prefix = "refs/tags/";
    for (std::size_t index = 0; index < head_count; ++index)
    {
        const std::string_view reference(heads[index]->name);
        if (reference.starts_with(branch_prefix))
            advertised.push_back({std::string(reference.substr(branch_prefix.size())), heads[index]->oid,
                GG_REMOTE_BRANCH});
        else if (reference.starts_with(tag_prefix) && !reference.ends_with("^{}"))
            advertised.push_back(
                {std::string(reference.substr(tag_prefix.size())), heads[index]->oid, GG_REMOTE_TAG});
    }
    git_remote_disconnect(remote.get());

    std::vector<gg_advertised_ref> refs;
    refs.reserve(advertised.size());
    for (const Advertised& ref : advertised)
        refs.push_back({command.remote.c_str(), ref.name.c_str(), ref.target, ref.kind});
    const std::vector<std::string> remote_names{command.remote};
    const StringArray remotes(remote_names);
    gg_fetch_options options = GG_FETCH_OPTIONS_INIT;
    options.advertised_refs = refs.data();
    options.advertised_ref_count = refs.size();
    options.remotes = remotes.Get();
    options.tracked = command.tracked_only;
    TransportPlan plan;
    Check(gg_repository_plan_fetch(&plan.value, gg, &options), "plan fetch");

    std::vector<std::string> refspec_storage;
    std::vector<char*> refspec_values;
    refspec_storage.reserve(plan.value.refspec_count);
    refspec_values.reserve(plan.value.refspec_count);
    for (std::size_t index = 0; index < plan.value.refspec_count; ++index)
    {
        const gg_refspec& refspec = plan.value.refspecs[index];
        refspec_storage.push_back(
            "+" + std::string(refspec.source) + ":" + std::string(refspec.destination));
    }
    for (std::string& refspec : refspec_storage)
        refspec_values.push_back(refspec.data());
    git_strarray refspecs{refspec_values.data(), refspec_values.size()};
    git_fetch_options fetch_options = GIT_FETCH_OPTIONS_INIT;
    fetch_options.callbacks = RemoteCallbacks();
    fetch_options.prune = GIT_FETCH_PRUNE;
    fetch_options.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;
    Check(git_remote_fetch(remote.get(), refspec_values.empty() ? nullptr : &refspecs, &fetch_options,
              command.tracked_only ? "ggui pull" : "ggui fetch"),
        command.tracked_only ? "pull remote" : "fetch remote");
    Mutation mutation;
    gg_operation_options operation = OperationOptions();
    Check(gg_repository_complete_fetch_ex(
              &mutation.value, gg, &plan.value, command.tracked_only, &operation),
        "complete fetch");
    PublishSnapshot();
}

void RepositoryEngine::Impl::PushBookmark(const Push& command)
{
    Sync();
    transfer_phase = "push";
    const std::vector<std::string> bookmarks{command.bookmark};
    const StringArray bookmark_names(bookmarks);
    gg_push_options options = GG_PUSH_OPTIONS_INIT;
    options.bookmarks = bookmark_names.Get();
    options.remote = command.remote.c_str();
    TransportPlan plan;
    Check(gg_repository_plan_push(&plan.value, gg, &options), "plan push");

    git_remote* raw_remote = nullptr;
    Check(git_remote_lookup(&raw_remote, git.get(), command.remote.c_str()), "find remote");
    std::unique_ptr<git_remote, decltype(&git_remote_free)> remote(raw_remote, git_remote_free);
    std::vector<std::string> refspec_storage;
    std::vector<char*> refspec_values;
    refspec_storage.reserve(plan.value.refspec_count);
    refspec_values.reserve(plan.value.refspec_count);
    for (std::size_t index = 0; index < plan.value.refspec_count; ++index)
    {
        const gg_refspec& refspec = plan.value.refspecs[index];
        refspec_storage.push_back(
            (command.force ? "+" : "") + std::string(refspec.source) + ":" + refspec.destination);
    }
    for (std::string& refspec : refspec_storage)
        refspec_values.push_back(refspec.data());
    git_strarray refspecs{refspec_values.data(), refspec_values.size()};
    git_push_options push_options = GIT_PUSH_OPTIONS_INIT;
    push_options.callbacks = RemoteCallbacks();
    push_options.remote_push_options = {
        plan.value.push_options.strings, plan.value.push_options.count};
    ssh_agent_attempted = false;
    credential_attempts = 0;
    Check(git_remote_push(remote.get(), &refspecs, &push_options), "push bookmark");
    Mutation mutation;
    gg_operation_options operation = OperationOptions();
    Check(gg_repository_complete_push(&mutation.value, gg, &plan.value, &operation), "complete push");
    PublishSnapshot();
}

void RepositoryEngine::Impl::RemoveRemoteBookmark(const RemoteBookmarkDelete& command, bool publish)
{
    Sync();
    transfer_phase = "push";
    std::string remote_name = command.remote;
    git_remote* raw_remote = nullptr;
    Check(git_remote_lookup(&raw_remote, git.get(), remote_name.c_str()), "find remote");
    std::unique_ptr<git_remote, decltype(&git_remote_free)> remote(raw_remote, git_remote_free);
    std::string destination = "refs/heads/" + command.bookmark;
    std::string deletion = ":" + destination;
    char* deletion_value = deletion.data();
    git_strarray refspecs{&deletion_value, 1};
    git_push_options push_options = GIT_PUSH_OPTIONS_INIT;
    push_options.callbacks = RemoteCallbacks();
    ssh_agent_attempted = false;
    credential_attempts = 0;
    Check(git_remote_push(remote.get(), &refspecs, &push_options), "delete remote bookmark");

    std::string source;
    gg_refspec refspec{remote_name.data(), source.data(), destination.data(), {}, false};
    std::array<std::string, 2> deleted_refs{
        "refs/gg/tracking/bookmarks/" + remote_name + "/" + command.bookmark,
        "refs/remotes/" + remote_name + "/" + command.bookmark};
    std::array<char*, 2> deleted_values{deleted_refs[0].data(), deleted_refs[1].data()};
    gg_transport_plan plan{GG_OPTIONS_VERSION, true, &refspec, 1,
        {deleted_values.data(), deleted_values.size()}, {nullptr, 0}};
    Mutation mutation;
    gg_operation_options operation = OperationOptions();
    Check(gg_repository_complete_push(&mutation.value, gg, &plan, &operation), "complete bookmark deletion");
    if (publish)
        PublishSnapshot();
}

} // namespace Ggui
