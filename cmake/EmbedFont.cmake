# Copyright (c) 2026-2026 the ggui project.
# SPDX-License-Identifier: GPL-2.0-only

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
  message(FATAL_ERROR "EmbedFont.cmake requires INPUT and OUTPUT")
endif()

file(SIZE "${INPUT}" input_size)
get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${OUTPUT}"
  "/* Generated from MaterialSymbolsOutlined.ttf. */\n\n"
  "const unsigned char ggui_icon_font_data[] = {\n")

set(offset 0)
while(offset LESS input_size)
  file(READ "${INPUT}" bytes OFFSET ${offset} LIMIT 4096 HEX)
  string(REGEX REPLACE
    "([0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f])"
    "\\1\n" bytes "${bytes}")
  string(REGEX REPLACE "\n$" "" bytes "${bytes}")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${bytes}")
  string(REPLACE "\n" "\n    " bytes "${bytes}")
  file(APPEND "${OUTPUT}" "    ${bytes}\n")
  math(EXPR offset "${offset} + 4096")
endwhile()

file(APPEND "${OUTPUT}"
  "};\nconst unsigned int ggui_icon_font_data_len = sizeof ggui_icon_font_data;\n")
