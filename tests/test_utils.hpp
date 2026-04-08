#pragma once

#include <stdexcept>
#include <utility>

#include "libatlas/libatlas.hpp"

namespace libatlas_test {

inline libatlas::Image make_rgba_image(uint32_t width, uint32_t height) {
  auto image = libatlas::make_image(width, height, libatlas::PixelFormat::RGBA8);
  if (!image) {
    throw std::runtime_error(image.error().message);
  }
  return std::move(image.value());
}

inline void set_pixel(libatlas::Image& image, uint32_t x, uint32_t y, libatlas::RgbaPixel pixel) {
  uint8_t* row = image.row_ptr(y);
  uint8_t* dst = row + (static_cast<std::size_t>(x) * 4U);
  dst[0] = pixel.r;
  dst[1] = pixel.g;
  dst[2] = pixel.b;
  dst[3] = pixel.a;
}

inline bool has_warning(const std::vector<libatlas::ExtractionWarning>& warnings,
                        libatlas::ExtractionWarning warning) {
  return std::find(warnings.begin(), warnings.end(), warning) != warnings.end();
}

}  // namespace libatlas_test
