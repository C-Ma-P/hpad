# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hpadv2)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})

find_package(Git QUIET)

set(HPAD_FIRMWARE_VERSION "unknown")

if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always --abbrev=12
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    RESULT_VARIABLE HPAD_GIT_DESCRIBE_RESULT
    OUTPUT_VARIABLE HPAD_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  if(HPAD_GIT_DESCRIBE_RESULT EQUAL 0 AND NOT HPAD_GIT_DESCRIBE STREQUAL "")
    set(HPAD_FIRMWARE_VERSION "${HPAD_GIT_DESCRIBE}")
  endif()
endif()

target_compile_definitions(app PRIVATE
  HPAD_FIRMWARE_VERSION="${HPAD_FIRMWARE_VERSION}"
)
