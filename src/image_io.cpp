#include "libatlas/image_io.hpp"

#include <utility>

#include "lodepng.h"

namespace libatlas {

Result<Image> load_png(const std::string& path) {
  std::vector<unsigned char> decoded_pixels;
  unsigned width = 0;
  unsigned height = 0;
  const unsigned error = lodepng::decode(decoded_pixels, width, height, path);
  if (error != 0U) {
    return Result<Image>::failure(ErrorCode::DecodeError, lodepng_error_text(error));
  }

  auto image = make_image(width, height, PixelFormat::RGBA8);
  if (!image) {
    return image;
  }

  Image output = std::move(image.value());
  output.pixels.assign(decoded_pixels.begin(), decoded_pixels.end());
  return Result<Image>::success(std::move(output));
}

Result<void> save_png(const Image& image, const std::string& path) {
  auto rgba = convert_image(image, PixelFormat::RGBA8);
  if (!rgba) {
    return Result<void>::failure(rgba.error().code, rgba.error().message);
  }

  const unsigned error = lodepng::encode(path,
                                         rgba.value().pixels,
                                         rgba.value().width,
                                         rgba.value().height);
  if (error != 0U) {
    return Result<void>::failure(ErrorCode::EncodeError, lodepng_error_text(error));
  }

  return Result<void>::success();
}

}  // namespace libatlas
