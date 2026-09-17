// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <string_view>

namespace Ggui::RepositoryInternal
{

using DiagnosticClock = std::chrono::steady_clock;

inline long long DiagnosticMilliseconds(DiagnosticClock::duration duration)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

inline bool TraceDiagnosticsEnabled()
{
    return spdlog::should_log(spdlog::level::trace);
}

inline DiagnosticClock::time_point DiagnosticNow()
{
    return TraceDiagnosticsEnabled() ? DiagnosticClock::now() : DiagnosticClock::time_point{};
}

inline void TraceTaskQueued(std::uint64_t task, std::string_view kind, std::uint64_t request,
    std::uint64_t repository_generation)
{
    if (!TraceDiagnosticsEnabled()) return;
    spdlog::trace("repository task queued task_id={} kind={} request={} repository_generation={}",
        task, kind, request, repository_generation);
}

class TraceTask
{
public:
    TraceTask(std::uint64_t task, std::string_view kind, std::uint64_t request,
        std::uint64_t repository_generation, DiagnosticClock::time_point queued)
        : _enabled(TraceDiagnosticsEnabled())
        , _task(task)
        , _kind(kind)
        , _request(request)
        , _repository_generation(repository_generation)
        , _queued(queued)
        , _started(_enabled ? DiagnosticClock::now() : DiagnosticClock::time_point{})
    {
        if (_enabled)
            spdlog::trace("repository task start task_id={} kind={} request={} repository_generation={} queue_ms={}",
                _task, _kind, _request, _repository_generation,
                DiagnosticMilliseconds(_started - _queued));
    }

    ~TraceTask()
    {
        if (!_finished) Finish("error");
    }

    TraceTask(const TraceTask&) = delete;
    TraceTask& operator=(const TraceTask&) = delete;

    bool Enabled() const { return _enabled; }
    std::uint64_t Id() const { return _task; }

    void Finish(std::string_view outcome)
    {
        if (_finished) return;
        _finished = true;
        if (_enabled)
            spdlog::trace("repository task complete task_id={} kind={} request={} repository_generation={} "
                          "outcome={} total_ms={}",
                _task, _kind, _request, _repository_generation, outcome,
                DiagnosticMilliseconds(DiagnosticClock::now() - _queued));
    }

private:
    bool _enabled = false;
    bool _finished = false;
    std::uint64_t _task = 0;
    std::string_view _kind;
    std::uint64_t _request = 0;
    std::uint64_t _repository_generation = 0;
    DiagnosticClock::time_point _queued;
    DiagnosticClock::time_point _started;
};

class TraceStage
{
public:
    TraceStage(const TraceTask& task, std::string_view name)
        : _enabled(task.Enabled()), _task(task.Id()), _name(name)
        , _started(_enabled ? DiagnosticClock::now() : DiagnosticClock::time_point{})
    {
        if (_enabled)
            spdlog::trace("repository stage start task_id={} stage={}", _task, _name);
    }

    ~TraceStage() { Complete(); }
    TraceStage(const TraceStage&) = delete;
    TraceStage& operator=(const TraceStage&) = delete;

    bool Enabled() const { return _enabled; }

    void Complete(std::string_view details = {})
    {
        if (_finished) return;
        _finished = true;
        if (!_enabled) return;
        if (details.empty())
            spdlog::trace("repository stage complete task_id={} stage={} duration_ms={}",
                _task, _name, DiagnosticMilliseconds(DiagnosticClock::now() - _started));
        else
            spdlog::trace("repository stage complete task_id={} stage={} duration_ms={} {}",
                _task, _name, DiagnosticMilliseconds(DiagnosticClock::now() - _started), details);
    }

private:
    bool _enabled = false;
    bool _finished = false;
    std::uint64_t _task = 0;
    std::string_view _name;
    DiagnosticClock::time_point _started;
};

inline void TraceTaskStale(std::uint64_t task, std::string_view kind, std::uint64_t request,
    std::uint64_t repository_generation, DiagnosticClock::time_point queued)
{
    TraceTask trace(task, kind, request, repository_generation, queued);
    trace.Finish("stale");
}

} // namespace Ggui::RepositoryInternal
