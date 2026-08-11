cmake_minimum_required(VERSION 3.24)

project(ggui VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(GGUI_BUILD_TESTS "Build ggui tests" ON)
option(GGUI_ENABLE_IMGUI_TEST_ENGINE "Build the real application with Dear ImGui Test Engine" OFF)
option(GGUI_ENABLE_COVERAGE "Instrument ggui sources for coverage" OFF)

if(GGUI_ENABLE_COVERAGE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "GGUI_ENABLE_COVERAGE requires GCC or Clang coverage support")
  endif()
  add_library(ggui_coverage INTERFACE)
  target_compile_options(ggui_coverage INTERFACE --coverage -O0 -g)
  target_link_options(ggui_coverage INTERFACE --coverage)
endif()

include(cmake/CPM.cmake)
find_package(gg 0.1 CONFIG QUIET)
if(NOT gg_FOUND)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../gg/CMakeLists.txt")
    set(GG_BUILD_TESTS OFF CACHE BOOL "Build gg tests")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../gg" "${CMAKE_CURRENT_BINARY_DIR}/gg" EXCLUDE_FROM_ALL)
  else()
    find_package(gg 0.1 CONFIG REQUIRED)
  endif()
endif()

CPMAddPackage(
  NAME SDL3
  GITHUB_REPOSITORY libsdl-org/SDL
  GIT_TAG release-3.4.0
  OPTIONS "SDL_STATIC ON" "SDL_SHARED OFF" "SDL_TEST_LIBRARY OFF" "SDL_X11_XTEST OFF"
  EXCLUDE_FROM_ALL TRUE)

CPMAddPackage(
  NAME imgui
  GITHUB_REPOSITORY ocornut/imgui
  GIT_TAG 84a9d53
  DOWNLOAD_ONLY YES)

find_package(OpenGL REQUIRED)
add_library(imgui STATIC EXCLUDE_FROM_ALL
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_compile_definitions(imgui PUBLIC IMGUI_USER_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/Source/imconfig.h")
target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/misc/cpp
  ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static OpenGL::GL)

if(GGUI_ENABLE_IMGUI_TEST_ENGINE)
  CPMAddPackage(
    NAME stb
    GITHUB_REPOSITORY nothings/stb
    GIT_TAG f1c79c0
    DOWNLOAD_ONLY YES)
  add_library(stb STATIC EXCLUDE_FROM_ALL Tests/stb_impl.cpp)
  target_include_directories(stb PUBLIC ${stb_SOURCE_DIR})

  CPMAddPackage(
    NAME imgui_test_engine
    GITHUB_REPOSITORY ocornut/imgui_test_engine
    GIT_TAG 3fff435
    DOWNLOAD_ONLY YES)
  set(GGUI_TEST_ENGINE_DIR ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine)
  add_library(imgui_test_engine STATIC EXCLUDE_FROM_ALL
    ${GGUI_TEST_ENGINE_DIR}/imgui_te_context.cpp
    ${GGUI_TEST_ENGINE_DIR}/imgui_te_coroutine.cpp
    ${GGUI_TEST_ENGINE_DIR}/imgui_te_engine.cpp
    ${GGUI_TEST_ENGINE_DIR}/imgui_te_exporters.cpp
    ${GGUI_TEST_ENGINE_DIR}/imgui_te_perftool.cpp
    ${GGUI_TEST_ENGINE_DIR}/imgui_te_ui.cpp
    ${GGUI_TEST_ENGINE_DIR}/imgui_te_utils.cpp
    ${GGUI_TEST_ENGINE_DIR}/imgui_capture_tool.cpp)
  target_include_directories(imgui_test_engine PUBLIC ${imgui_test_engine_SOURCE_DIR} ${GGUI_TEST_ENGINE_DIR})
  target_link_libraries(imgui_test_engine PUBLIC imgui stb)
  target_compile_definitions(imgui PUBLIC IMGUI_BUILD_TESTING=1)
  target_include_directories(imgui PUBLIC ${imgui_test_engine_SOURCE_DIR} ${GGUI_TEST_ENGINE_DIR} ${stb_SOURCE_DIR})
endif()

CPMAddPackage(
  NAME nfd
  GITHUB_REPOSITORY btzy/nativefiledialog-extended
  VERSION 1.3.0
  OPTIONS "NFD_BUILD_TESTS OFF" "NFD_INSTALL OFF" "NFD_PORTAL ON"
  EXCLUDE_FROM_ALL TRUE)

CPMAddPackage(
  NAME spdlog
  GITHUB_REPOSITORY gabime/spdlog
  GIT_TAG v1.17.0
  OPTIONS "SPDLOG_BUILD_EXAMPLE OFF" "SPDLOG_BUILD_TESTS OFF" "SPDLOG_INSTALL OFF"
  EXCLUDE_FROM_ALL TRUE)

CPMAddPackage(
  NAME efsw
  GITHUB_REPOSITORY SpartanJ/efsw
  GIT_TAG 1.6.3
  OPTIONS "BUILD_SHARED_LIBS OFF" "BUILD_STATIC_LIBS ON" "BUILD_TEST_APP OFF" "EFSW_INSTALL OFF"
  EXCLUDE_FROM_ALL TRUE)

CPMAddPackage(
  NAME json
  GITHUB_REPOSITORY nlohmann/json
  VERSION 3.12.0
  OPTIONS "JSON_BuildTests OFF"
  EXCLUDE_FROM_ALL TRUE)

set(imgui_color_text_edit_revision fa6fd434d3db973e604653d2147316f41d49ed56)
CPMAddPackage(
  NAME ImGuiColorTextEdit
  GITHUB_REPOSITORY goossens/ImGuiColorTextEdit
  GIT_TAG ${imgui_color_text_edit_revision}
  DOWNLOAD_ONLY YES)
# CPM patch steps are not safe to rerun over an already-patched FetchContent checkout.
# Include exact source hashes in the stamp so stale and partially applied caches self-repair.
set(imgui_color_text_edit_patches
  ImGuiColorTextEdit.patch)
set(imgui_color_text_edit_sources TextDiff.cpp TextDiff.h TextEditor.cpp)
set(imgui_color_text_edit_patch_state ${imgui_color_text_edit_revision})
foreach(patch_file IN LISTS imgui_color_text_edit_patches)
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${patch_file}" patch_hash)
  string(APPEND imgui_color_text_edit_patch_state ";${patch_hash}")
endforeach()
set(imgui_color_text_edit_state "${imgui_color_text_edit_patch_state}")
foreach(source_file IN LISTS imgui_color_text_edit_sources)
  file(SHA256 "${ImGuiColorTextEdit_SOURCE_DIR}/${source_file}" source_hash)
  string(APPEND imgui_color_text_edit_state ";${source_hash}")
endforeach()
set(imgui_color_text_edit_stamp "${CMAKE_CURRENT_BINARY_DIR}/ImGuiColorTextEdit-patches.stamp")
set(imgui_color_text_edit_patched FALSE)
if(EXISTS "${imgui_color_text_edit_stamp}")
  file(READ "${imgui_color_text_edit_stamp}" stamped_state)
  if(stamped_state STREQUAL imgui_color_text_edit_state)
    set(imgui_color_text_edit_patched TRUE)
  endif()
endif()
if(NOT imgui_color_text_edit_patched)
  find_package(Git REQUIRED)
  foreach(source_file IN LISTS imgui_color_text_edit_sources)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${ImGuiColorTextEdit_SOURCE_DIR}" show "HEAD:${source_file}"
      OUTPUT_FILE "${ImGuiColorTextEdit_SOURCE_DIR}/${source_file}"
      RESULT_VARIABLE restore_result)
    if(NOT restore_result EQUAL 0)
      message(FATAL_ERROR "Could not restore ImGuiColorTextEdit ${source_file}")
    endif()
  endforeach()
  foreach(patch_file IN LISTS imgui_color_text_edit_patches)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" apply --unidiff-zero
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${patch_file}"
      WORKING_DIRECTORY "${ImGuiColorTextEdit_SOURCE_DIR}"
      RESULT_VARIABLE patch_result)
    if(NOT patch_result EQUAL 0)
      message(FATAL_ERROR "Could not apply ${patch_file}")
    endif()
  endforeach()
  set(imgui_color_text_edit_state "${imgui_color_text_edit_patch_state}")
  foreach(source_file IN LISTS imgui_color_text_edit_sources)
    file(SHA256 "${ImGuiColorTextEdit_SOURCE_DIR}/${source_file}" source_hash)
    string(APPEND imgui_color_text_edit_state ";${source_hash}")
  endforeach()
  file(WRITE "${imgui_color_text_edit_stamp}" "${imgui_color_text_edit_state}")
endif()
add_library(imgui_color_text_edit STATIC EXCLUDE_FROM_ALL
  ${ImGuiColorTextEdit_SOURCE_DIR}/TextEditor.cpp
  ${ImGuiColorTextEdit_SOURCE_DIR}/TextDiff.cpp)
target_include_directories(imgui_color_text_edit PUBLIC ${ImGuiColorTextEdit_SOURCE_DIR})
target_link_libraries(imgui_color_text_edit PUBLIC imgui)

CPMAddPackage(
  NAME IconFontCppHeaders
  GITHUB_REPOSITORY juliettef/IconFontCppHeaders
  GIT_TAG 4577f2f
  DOWNLOAD_ONLY YES)
add_library(IconFontCppHeaders INTERFACE)
target_include_directories(IconFontCppHeaders INTERFACE ${IconFontCppHeaders_SOURCE_DIR})

add_library(ggui_core STATIC
  Source/Core.cpp
  Source/Core.hpp
  Source/Graph.cpp
  Source/Graph.hpp)
target_include_directories(ggui_core PUBLIC Source)
target_link_libraries(ggui_core PUBLIC gg::gg PRIVATE efsw-static spdlog::spdlog)
target_compile_options(ggui_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(GGUI_ENABLE_COVERAGE)
  target_link_libraries(ggui_core PUBLIC ggui_coverage)
endif()
if(GGUI_ENABLE_COVERAGE OR GGUI_ENABLE_IMGUI_TEST_ENGINE)
  target_compile_definitions(ggui_core PUBLIC GGUI_TESTING=1)
endif()

add_executable(ggui
  Source/Application.cpp
  Source/Application.hpp
  Source/Main.cpp)
if(GGUI_ENABLE_IMGUI_TEST_ENGINE)
  target_sources(ggui PRIVATE Tests/UiTests.cpp)
  target_compile_definitions(ggui PRIVATE IMGUI_BUILD_TESTING=1)
endif()
target_link_libraries(ggui PRIVATE
  ggui_core
  imgui
  imgui_color_text_edit
  IconFontCppHeaders
  SDL3::SDL3-static
  nfd::nfd
  nlohmann_json::nlohmann_json
  spdlog::spdlog)
target_compile_definitions(ggui PRIVATE
  GGUI_ICON_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/Assets/Fonts/MaterialSymbolsOutlined.ttf")
if(GGUI_ENABLE_IMGUI_TEST_ENGINE)
  target_link_libraries(ggui PRIVATE imgui_test_engine)
endif()
if(GGUI_ENABLE_COVERAGE)
  target_link_libraries(ggui PRIVATE ggui_coverage)
endif()
target_compile_options(ggui PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(GGUI_BUILD_TESTS)
  enable_testing()
  CPMAddPackage(
    NAME googletest
    GITHUB_REPOSITORY google/googletest
    VERSION 1.17.0
    OPTIONS "INSTALL_GTEST OFF")
  add_executable(ggui_tests Tests/Tests.cpp)
  target_link_libraries(ggui_tests PRIVATE ggui_core GTest::gtest_main)
  target_compile_options(ggui_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  if(GGUI_ENABLE_COVERAGE)
    target_link_libraries(ggui_tests PRIVATE ggui_coverage)
  endif()
  add_test(NAME ggui_tests COMMAND ggui_tests)
  if(GGUI_ENABLE_IMGUI_TEST_ENGINE)
    add_test(NAME ggui_ui_tests COMMAND ggui --test)
  endif()
endif()

if(UNIX AND NOT APPLE)
  set_target_properties(ggui PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
endif()

install(TARGETS ggui RUNTIME DESTINATION bin)
if(UNIX AND NOT APPLE AND gg_FOUND)
  install(IMPORTED_RUNTIME_ARTIFACTS gg::gg libgit2::libgit2package LIBRARY DESTINATION lib)
endif()
install(FILES README.md LICENSE DESTINATION share/ggui)
install(FILES Assets/Fonts/MaterialSymbolsOutlined.ttf DESTINATION share/ggui/Fonts)
if(WIN32)
  set(CPACK_GENERATOR ZIP)
else()
  set(CPACK_GENERATOR TGZ)
endif()
set(CPACK_PACKAGE_FILE_NAME "ggui-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
include(CPack)
