cmake_minimum_required(VERSION 3.20)
project(printman LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The standalone CLI. The host-independent core is header-first under include/printman; heavy deps
# (USD, OSL) arrive later as fenced edge modules. png.cpp owns the single stb implementation TU.
add_executable(printman
  src/cli/main.cpp
  src/core/png.cpp
)
target_include_directories(printman PRIVATE include third_party)
target_compile_options(printman PRIVATE -Wall -Wextra)

# --- OSL shading backend (fenced heavy dep) -----------------------------------------------------
# The host-independent core knows only the Shader interface; OSL lives entirely behind it. Dev
# paths point at the locally-built OSL (~/tmp/osl) + brew OIIO; a proper find_package lands later.
option(PRINTMAN_OSL "Build with the OSL shading backend" OFF)
if(PRINTMAN_OSL)
  set(OSL_ROOT "$ENV{HOME}/tmp/osl/OpenShadingLanguage" CACHE PATH "OSL build root")
  target_compile_definitions(printman PRIVATE PRINTMAN_OSL)
  target_include_directories(printman PRIVATE
    ${OSL_ROOT}/src/include
    ${OSL_ROOT}/build/include
    /opt/homebrew/opt/openimageio/include
    /opt/homebrew/opt/imath/include
    /opt/homebrew/opt/fmt/include)
  target_link_libraries(printman PRIVATE
    ${OSL_ROOT}/build/lib/liboslexec.dylib
    /opt/homebrew/opt/openimageio/lib/libOpenImageIO.dylib
    /opt/homebrew/opt/openimageio/lib/libOpenImageIO_Util.dylib)
endif()
