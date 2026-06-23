# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hpadv2)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(HPAD_GENERATED_MAIN "${CMAKE_CURRENT_BINARY_DIR}/generated/main.c")
execute_process(
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_dfu_menu_main.py
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${HPAD_GENERATED_MAIN}
  RESULT_VARIABLE HPAD_DFU_PATCH_RESULT
)
if(NOT HPAD_DFU_PATCH_RESULT EQUAL 0)
  message(FATAL_ERROR "Failed to generate menu-integrated DFU main.c")
endif()

FILE(GLOB app_sources src/*.c)
list(REMOVE_ITEM app_sources
  "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/adafruit_dfu_entry.c"
)
target_sources(app PRIVATE ${app_sources} ${HPAD_GENERATED_MAIN})

find_package(Git QUIET)

set(HPAD_FIRMWARE_VERSION "unknown")

if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --always --abbrev=12
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    RESULT_VARIABLE HPAD_GIT_DESCRIBE_RESULT
    OUTPUT_VARIABLE HPAD_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  if(HPAD_GIT_DESCRIBE_RESULT EQUAL 0 AND NOT HPAD_GIT_DESCRIBE STREQUAL "")
    set(HPAD_FIRMWARE_VERSION "${HPAD_GIT_DESCRIBE}")

    execute_process(
      COMMAND ${GIT_EXECUTABLE} diff-index --quiet HEAD --
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      RESULT_VARIABLE HPAD_GIT_DIRTY_RESULT
      OUTPUT_QUIET
      ERROR_QUIET
    )

    if(HPAD_GIT_DIRTY_RESULT EQUAL 1
       AND NOT HPAD_GIT_DESCRIBE MATCHES "-g[0-9a-fA-F]+$"
       AND NOT HPAD_GIT_DESCRIBE MATCHES "^[0-9a-fA-F]+$")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE HPAD_GIT_HASH_RESULT
        OUTPUT_VARIABLE HPAD_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )

      if(HPAD_GIT_HASH_RESULT EQUAL 0 AND NOT HPAD_GIT_HASH STREQUAL "")
        set(HPAD_FIRMWARE_VERSION "${HPAD_GIT_DESCRIBE}-${HPAD_GIT_HASH}")
      endif()
    endif()
  endif()
endif()

target_compile_definitions(app PRIVATE
  HPAD_FIRMWARE_VERSION="${HPAD_FIRMWARE_VERSION}"
)
