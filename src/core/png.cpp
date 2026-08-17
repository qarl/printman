#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "printman/png.hpp"

namespace printman {
bool write_png(const std::string& path, const Frame& fr) {
    if (fr.W <= 0 || fr.H <= 0) return false;
    return stbi_write_png(path.c_str(), fr.W, fr.H, 3, fr.rgb.data(), fr.W * 3) != 0;
}
}  // namespace printman
