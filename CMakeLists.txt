cmake_minimum_required(VERSION 3.0)
project(crashpad LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED True)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LINUX TRUE)
endif()

include_directories("${CMAKE_CURRENT_SOURCE_DIR}")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/third_party/mini_chromium/mini_chromium")

# These should really be in `compat`, but we want them for the whole project
if(APPLE)
    include_directories(SYSTEM "compat/mac")
else()
    include_directories(PUBLIC "compat/non_mac")
endif()

if(LINUX OR ANDROID)
    include_directories(SYSTEM "compat/linux")
endif()
if(ANDROID)
    include_directories(SYSTEM "compat/android")
endif()

if(WIN32)
    include_directories(SYSTEM "compat/win")
else()
    include_directories(PUBLIC "compat/non_win")
endif()

if(NOT LINUX AND NOT ANDROID)
    include_directories(SYSTEM "compat/non_elf")
endif()

add_subdirectory(client)
add_subdirectory(compat)
add_subdirectory(handler)
add_subdirectory(minidump)
add_subdirectory(snapshot)
add_subdirectory(tools)
add_subdirectory(util)
add_subdirectory(third_party/mini_chromium)