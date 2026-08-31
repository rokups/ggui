// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "Application/Application.hpp"

#include <SDL3/SDL_main.h>

int main(int argc, char** argv)
{
    Ggui::Application app;
    return app.Run(argc, argv);
}
