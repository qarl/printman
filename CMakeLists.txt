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

# --- USD front-end (fenced heavy dep) -----------------------------------------------------------
# Loads an arbitrary cage + material from a USD stage. Monolithic static libusd_m needs -force_load
# so USD's static type/plugin registries are not stripped, plus explicit plugin registration.
option(PRINTMAN_USD "Build with the USD front-end" OFF)
if(PRINTMAN_USD)
  set(USD_ROOT "/Users/qarl/src/OrcaSlicer/deps/build/arm64/OrcaSlicer_dep/usr/local" CACHE PATH "OpenUSD install prefix")
  target_sources(printman PRIVATE src/usd/usd_read.cpp)
  target_compile_definitions(printman PRIVATE PRINTMAN_USD PRINTMAN_USD_PLUGINDIR="${USD_ROOT}/lib/usd")
  target_include_directories(printman PRIVATE ${USD_ROOT}/include)
  file(GLOB PM_BOOST "${USD_ROOT}/lib/libboost_*.a")
  target_link_libraries(printman PRIVATE
    -Wl,-force_load,${USD_ROOT}/lib/libusd_m.a
    ${PM_BOOST} ${USD_ROOT}/lib/libtbb.a
    "-framework CoreFoundation" "-framework Cocoa")
endif()

# --- Tests: the crack-free + band-invariance gates as an enforced ctest suite -------------------
# The unit harnesses build their cages in code (no USD/OSL), so they compile and run everywhere and
# guard the crown-jewel invariants: crack-free adaptive dicing and pixel-exact band-count invariance.
enable_testing()
foreach(t poly_watertight adaptive_cc open_cc crease_invariance)
  add_executable(${t}_test tests/${t}_test.cpp)
  target_include_directories(${t}_test PRIVATE include third_party)
  target_compile_options(${t}_test PRIVATE -Wall -Wextra)
  add_test(NAME ${t} COMMAND ${t}_test)
endforeach()
# The built-in procedural sphere band-invariance gate (no USD needed).
add_test(NAME sphere_gate COMMAND printman --gate --bands 8)
# The USD band-invariance gate on a committed all-quad Catmull-Clark fixture (only when USD is built).
if(PRINTMAN_USD)
  add_test(NAME box_cc_gate
    COMMAND printman ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/box_cc.usda --gate --bands 8 --subdiv-level 6)
  add_test(NAME creased_wall_gate
    COMMAND printman ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/creased_wall.usda --gate --bands 12 --subdiv-level 5)
endif()
