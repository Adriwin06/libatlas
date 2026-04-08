#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "libatlas/geometry.hpp"
#include "libatlas/result.hpp"

namespace libatlas {

enum class PixelFormat {
  RGBA8,
  BGRA8,
  RGB8,
  Gray8
};

struct RgbaPixel {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t a = 0;
};

struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  PixelFormat pixel_format = PixelFormat::RGBA8;
  std::size_t row_stride = 0;
  std::vector<uint8_t> pixels;

  bool empty() const noexcept { return width == 0 || height == 0; }
  std::size_t bytes_per_pixel() const noexcept;
  bool has_alpha() const noexcept;
  std::size_t required_buffer_size() const noexcept;

  Result<void> validate() const;

  const uint8_t* row_ptr(uint32_t y) const;
  uint8_t* row_ptr(uint32_t y);
};

std::size_t bytes_per_pixel(PixelFormat pixel_format) noexcept;
bool pixel_format_has_alpha(PixelFormat pixel_format) noexcept;
const char* pixel_format_to_string(PixelFormat pixel_format) noexcept;

Result<Image> make_image(uint32_t width, uint32_t height, PixelFormat pixel_format);
Result<Image> convert_image(const Image& image, PixelFormat target_format);
Result<Image> crop_image(const Image& image, const PixelRect& rect);
Result<Image> trim_transparent_borders(const Image& image,
                                       uint8_t alpha_threshold,
                                       PixelRect* trimmed_rect_in_source = nullptr);
double compute_alpha_coverage(const Image& image, uint8_t alpha_threshold = 0);
RgbaPixel sample_as_rgba8(const Image& image, uint32_t x, uint32_t y);

}  // namespace libatlas
