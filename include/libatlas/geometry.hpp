#pragma once

#include <cstdint>

namespace libatlas {

enum class UvOrigin {
  TopLeft,
  BottomLeft
};

enum class UvRoundingPolicy {
  Expand,
  Nearest,
  Contract
};

struct UvRect {
  double x_min = 0.0;
  double x_max = 0.0;
  double y_min = 0.0;
  double y_max = 0.0;
};

struct PixelRect {
  int32_t x = 0;
  int32_t y = 0;
  int32_t width = 0;
  int32_t height = 0;

  bool empty() const noexcept { return width <= 0 || height <= 0; }
  int32_t right() const noexcept { return x + width; }
  int32_t bottom() const noexcept { return y + height; }
};

}  // namespace libatlas
