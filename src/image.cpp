#include "libatlas/image.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

namespace libatlas {

namespace {

Result<void> make_validation_error(std::string message) {
  return Result<void>::failure(ErrorCode::BufferSizeMismatch, std::move(message));
}

bool checked_multiply(std::size_t lhs, std::size_t rhs, std::size_t* out) {
  if (lhs == 0 || rhs == 0) {
    *out = 0;
    return true;
  }
  if (lhs > std::numeric_limits<std::size_t>::max() / rhs) {
    return false;
  }
  *out = lhs * rhs;
  return true;
}

void write_pixel(Image& image, uint32_t x, uint32_t y, const RgbaPixel& pixel) {
  uint8_t* dst = image.row_ptr(y) + x * image.bytes_per_pixel();
  switch (image.pixel_format) {
    case PixelFormat::RGBA8:
      dst[0] = pixel.r;
      dst[1] = pixel.g;
      dst[2] = pixel.b;
      dst[3] = pixel.a;
      break;
    case PixelFormat::BGRA8:
      dst[0] = pixel.b;
      dst[1] = pixel.g;
      dst[2] = pixel.r;
      dst[3] = pixel.a;
      break;
    case PixelFormat::RGB8:
      dst[0] = pixel.r;
      dst[1] = pixel.g;
      dst[2] = pixel.b;
      break;
    case PixelFormat::Gray8: {
      const uint16_t gray =
          static_cast<uint16_t>((static_cast<uint16_t>(pixel.r) * 54U) +
                                (static_cast<uint16_t>(pixel.g) * 183U) +
                                (static_cast<uint16_t>(pixel.b) * 19U));
      dst[0] = static_cast<uint8_t>(gray >> 8U);
      break;
    }
  }
}

}  // namespace

std::size_t bytes_per_pixel(PixelFormat pixel_format) noexcept {
  switch (pixel_format) {
    case PixelFormat::RGBA8:
    case PixelFormat::BGRA8:
      return 4;
    case PixelFormat::RGB8:
      return 3;
    case PixelFormat::Gray8:
      return 1;
  }
  return 0;
}

bool pixel_format_has_alpha(PixelFormat pixel_format) noexcept {
  switch (pixel_format) {
    case PixelFormat::RGBA8:
    case PixelFormat::BGRA8:
      return true;
    case PixelFormat::RGB8:
    case PixelFormat::Gray8:
      return false;
  }
  return false;
}

const char* pixel_format_to_string(PixelFormat pixel_format) noexcept {
  switch (pixel_format) {
    case PixelFormat::RGBA8:
      return "RGBA8";
    case PixelFormat::BGRA8:
      return "BGRA8";
    case PixelFormat::RGB8:
      return "RGB8";
    case PixelFormat::Gray8:
      return "Gray8";
  }
  return "Unknown";
}

std::size_t Image::bytes_per_pixel() const noexcept { return libatlas::bytes_per_pixel(pixel_format); }

bool Image::has_alpha() const noexcept { return pixel_format_has_alpha(pixel_format); }

std::size_t Image::required_buffer_size() const noexcept {
  if (empty()) {
    return 0;
  }
  return row_stride * static_cast<std::size_t>(height);
}

Result<void> Image::validate() const {
  const std::size_t bpp = bytes_per_pixel();
  if (bpp == 0) {
    return Result<void>::failure(ErrorCode::UnsupportedFormat, "unsupported pixel format");
  }

  if (empty()) {
    if (row_stride != 0 || !pixels.empty()) {
      return make_validation_error("empty images must have zero stride and an empty pixel buffer");
    }
    return Result<void>::success();
  }

  std::size_t min_row_stride = 0;
  if (!checked_multiply(static_cast<std::size_t>(width), bpp, &min_row_stride)) {
    return make_validation_error("image row stride overflow");
  }
  if (row_stride < min_row_stride) {
    std::ostringstream stream;
    stream << "row stride " << row_stride << " is too small for width " << width
           << " and format " << pixel_format_to_string(pixel_format);
    return make_validation_error(stream.str());
  }

  std::size_t required_size = 0;
  if (!checked_multiply(row_stride, static_cast<std::size_t>(height), &required_size)) {
    return make_validation_error("image buffer size overflow");
  }
  if (pixels.size() < required_size) {
    std::ostringstream stream;
    stream << "pixel buffer size " << pixels.size() << " is smaller than required size "
           << required_size;
    return make_validation_error(stream.str());
  }
  return Result<void>::success();
}

const uint8_t* Image::row_ptr(uint32_t y) const { return pixels.data() + (row_stride * y); }

uint8_t* Image::row_ptr(uint32_t y) { return pixels.data() + (row_stride * y); }

Result<Image> make_image(uint32_t width, uint32_t height, PixelFormat pixel_format) {
  const std::size_t bpp = bytes_per_pixel(pixel_format);
  if (bpp == 0) {
    return Result<Image>::failure(ErrorCode::UnsupportedFormat, "unsupported pixel format");
  }

  Image image;
  image.width = width;
  image.height = height;
  image.pixel_format = pixel_format;

  if (width == 0 || height == 0) {
    return Result<Image>::success(std::move(image));
  }

  std::size_t row_stride = 0;
  if (!checked_multiply(static_cast<std::size_t>(width), bpp, &row_stride)) {
    return Result<Image>::failure(ErrorCode::InvalidArgument, "image row stride overflow");
  }

  std::size_t buffer_size = 0;
  if (!checked_multiply(row_stride, static_cast<std::size_t>(height), &buffer_size)) {
    return Result<Image>::failure(ErrorCode::InvalidArgument, "image buffer size overflow");
  }

  image.row_stride = row_stride;
  image.pixels.assign(buffer_size, 0);
  return Result<Image>::success(std::move(image));
}

RgbaPixel sample_as_rgba8(const Image& image, uint32_t x, uint32_t y) {
  const uint8_t* src = image.row_ptr(y) + x * image.bytes_per_pixel();
  switch (image.pixel_format) {
    case PixelFormat::RGBA8:
      return RgbaPixel{src[0], src[1], src[2], src[3]};
    case PixelFormat::BGRA8:
      return RgbaPixel{src[2], src[1], src[0], src[3]};
    case PixelFormat::RGB8:
      return RgbaPixel{src[0], src[1], src[2], 255};
    case PixelFormat::Gray8:
      return RgbaPixel{src[0], src[0], src[0], 255};
  }
  return RgbaPixel{};
}

Result<Image> convert_image(const Image& image, PixelFormat target_format) {
  const auto validation = image.validate();
  if (!validation) {
    return Result<Image>::failure(validation.error().code, validation.error().message);
  }

  if (image.pixel_format == target_format) {
    return Result<Image>::success(image);
  }

  auto converted = make_image(image.width, image.height, target_format);
  if (!converted) {
    return converted;
  }

  Image output = std::move(converted.value());
  for (uint32_t y = 0; y < image.height; ++y) {
    for (uint32_t x = 0; x < image.width; ++x) {
      write_pixel(output, x, y, sample_as_rgba8(image, x, y));
    }
  }
  return Result<Image>::success(std::move(output));
}

Result<Image> crop_image(const Image& image, const PixelRect& rect) {
  const auto validation = image.validate();
  if (!validation) {
    return Result<Image>::failure(validation.error().code, validation.error().message);
  }

  if (rect.x < 0 || rect.y < 0 || rect.width < 0 || rect.height < 0) {
    return Result<Image>::failure(ErrorCode::InvalidArgument,
                                  "crop rectangles must use non-negative coordinates");
  }
  if (rect.right() > static_cast<int32_t>(image.width) ||
      rect.bottom() > static_cast<int32_t>(image.height)) {
    return Result<Image>::failure(ErrorCode::InvalidArgument, "crop rectangle exceeds image bounds");
  }

  auto cropped = make_image(static_cast<uint32_t>(rect.width),
                            static_cast<uint32_t>(rect.height),
                            image.pixel_format);
  if (!cropped) {
    return cropped;
  }

  Image output = std::move(cropped.value());
  if (output.empty()) {
    return Result<Image>::success(std::move(output));
  }

  const std::size_t bytes_to_copy =
      static_cast<std::size_t>(rect.width) * image.bytes_per_pixel();
  for (int32_t row = 0; row < rect.height; ++row) {
    const uint8_t* src = image.row_ptr(static_cast<uint32_t>(rect.y + row)) +
                         (static_cast<std::size_t>(rect.x) * image.bytes_per_pixel());
    uint8_t* dst = output.row_ptr(static_cast<uint32_t>(row));
    std::memcpy(dst, src, bytes_to_copy);
  }

  return Result<Image>::success(std::move(output));
}

Result<Image> trim_transparent_borders(const Image& image,
                                       uint8_t alpha_threshold,
                                       PixelRect* trimmed_rect_in_source) {
  const auto validation = image.validate();
  if (!validation) {
    return Result<Image>::failure(validation.error().code, validation.error().message);
  }

  if (image.empty()) {
    if (trimmed_rect_in_source != nullptr) {
      *trimmed_rect_in_source = PixelRect{};
    }
    return Result<Image>::success(image);
  }

  int32_t left = static_cast<int32_t>(image.width);
  int32_t top = static_cast<int32_t>(image.height);
  int32_t right = -1;
  int32_t bottom = -1;

  for (uint32_t y = 0; y < image.height; ++y) {
    for (uint32_t x = 0; x < image.width; ++x) {
      if (sample_as_rgba8(image, x, y).a > alpha_threshold) {
        left = std::min(left, static_cast<int32_t>(x));
        top = std::min(top, static_cast<int32_t>(y));
        right = std::max(right, static_cast<int32_t>(x));
        bottom = std::max(bottom, static_cast<int32_t>(y));
      }
    }
  }

  if (right < left || bottom < top) {
    if (trimmed_rect_in_source != nullptr) {
      *trimmed_rect_in_source = PixelRect{};
    }
    return make_image(0, 0, image.pixel_format);
  }

  const PixelRect kept_rect{
      left,
      top,
      right - left + 1,
      bottom - top + 1,
  };

  if (trimmed_rect_in_source != nullptr) {
    *trimmed_rect_in_source = kept_rect;
  }
  return crop_image(image, kept_rect);
}

double compute_alpha_coverage(const Image& image, uint8_t alpha_threshold) {
  if (!image.validate()) {
    return 0.0;
  }
  if (image.empty()) {
    return 0.0;
  }
  if (!image.has_alpha()) {
    return 1.0;
  }

  std::size_t covered = 0;
  for (uint32_t y = 0; y < image.height; ++y) {
    for (uint32_t x = 0; x < image.width; ++x) {
      if (sample_as_rgba8(image, x, y).a > alpha_threshold) {
        ++covered;
      }
    }
  }

  const std::size_t total = static_cast<std::size_t>(image.width) * image.height;
  return total == 0 ? 0.0 : static_cast<double>(covered) / static_cast<double>(total);
}

}  // namespace libatlas
