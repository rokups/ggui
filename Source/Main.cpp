// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "Application/Application.hpp"

#include <SDL3/SDL_main.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <memory>
#include <string>

namespace
{

std::shared_ptr<spdlog::logger> CreateLogger()
{
    const char* file = std::getenv("GGUI_LOG_FILE");
    if (file != nullptr && *file != '\0')
    {
        try
        {
            return spdlog::basic_logger_mt("ggui", std::string(file), true);
        }
        catch (const spdlog::spdlog_ex& error)
        {
            auto logger = spdlog::stderr_logger_mt("ggui");
            logger->error("Could not open GGUI_LOG_FILE: {}", error.what());
            return logger;
        }
    }
    return spdlog::stderr_logger_mt("ggui");
}

} // namespace

int main(int argc, char** argv)
{
    spdlog::cfg::load_env_levels();
    spdlog::set_default_logger(CreateLogger());
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [thread %t] [%l] %v");
    spdlog::flush_on(spdlog::level::trace);
    spdlog::trace("repository timing diagnostics enabled");

    Ggui::Application app;
    return app.Run(argc, argv);
}
