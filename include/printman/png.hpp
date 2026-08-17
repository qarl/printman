#pragma once
#include <string>

#include "printman/raster.hpp"

namespace printman {
// Write an RGB8 frame to a PNG (impl in png.cpp, which owns the stb implementation TU).
bool write_png(const std::string& path, const Frame& fr);
}  // namespace printman
