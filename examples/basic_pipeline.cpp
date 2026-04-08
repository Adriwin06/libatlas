#include <iostream>
#include <vector>

#include "libatlas/libatlas.hpp"

namespace {

void set_pixel(libatlas::Image& image, uint32_t x, uint32_t y, libatlas::RgbaPixel pixel) {
  uint8_t* dst = image.row_ptr(y) + (static_cast<std::size_t>(x) * 4U);
  dst[0] = pixel.r;
  dst[1] = pixel.g;
  dst[2] = pixel.b;
  dst[3] = pixel.a;
}

}  // namespace

int main() {
  auto atlas_result = libatlas::make_image(8, 8, libatlas::PixelFormat::RGBA8);
  if (!atlas_result) {
    std::cerr << atlas_result.error().message << "\n";
    return 1;
  }

  libatlas::Image atlas = std::move(atlas_result.value());
  for (uint32_t y = 2; y < 5; ++y) {
    for (uint32_t x = 1; x < 4; ++x) {
      set_pixel(atlas, x, y, libatlas::RgbaPixel{255, 180, 0, 255});
    }
  }

  libatlas::ExtractionOptions extraction_options;
  extraction_options.source_atlas_identifier = "example_atlas";
  extraction_options.trim_transparent_borders = true;

  auto extracted =
      libatlas::extract_texture(atlas, libatlas::UvRect{0.0, 0.5, 0.125, 0.75}, extraction_options);
  if (!extracted) {
    std::cerr << extracted.error().message << "\n";
    return 1;
  }

  std::vector<libatlas::PackItem> pack_items{
      libatlas::PackItem{extracted.value().metadata.exact_id.hex(),
                         extracted.value().trimmed_image,
                         "trimmed_example"},
  };

  libatlas::AtlasPackOptions pack_options;
  pack_options.max_atlas_width = 64;
  pack_options.max_atlas_height = 64;
  pack_options.padding = 1;

  auto packed = libatlas::pack_atlases(pack_items, pack_options);
  if (!packed) {
    std::cerr << packed.error().message << "\n";
    return 1;
  }

  const auto& placement = packed.value().placements.front();
  std::cout << "Exact ID: " << extracted.value().metadata.exact_id.to_string() << "\n";
  std::cout << "Trimmed size: " << extracted.value().trimmed_image.width << "x"
            << extracted.value().trimmed_image.height << "\n";
  std::cout << "Packed atlas size: " << packed.value().atlases.front().image.width << "x"
            << packed.value().atlases.front().image.height << "\n";
  std::cout << "Packed UVs: [" << placement.uv_rect.x_min << ", " << placement.uv_rect.x_max
            << "] x [" << placement.uv_rect.y_min << ", " << placement.uv_rect.y_max << "]\n";
  return 0;
}
