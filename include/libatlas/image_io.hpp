#pragma once

#include <string>

#include "libatlas/image.hpp"

namespace libatlas {

Result<Image> load_png(const std::string& path);
Result<void> save_png(const Image& image, const std::string& path);

}  // namespace libatlas
