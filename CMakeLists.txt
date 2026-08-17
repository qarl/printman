cmake_minimum_required(VERSION 3.20)
project(printman LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The standalone CLI. The host-independent core is header/source under include/ + src/core
# (added slice by slice); heavy deps (USD, OSL) arrive later as fenced edge modules.
add_executable(printman
  src/cli/main.cpp
)
target_include_directories(printman PRIVATE include)
target_compile_options(printman PRIVATE -Wall -Wextra)
