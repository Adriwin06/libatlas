#include "libatlas/packing.hpp"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace libatlas {

namespace {

struct PreparedItem {
  std::string entry_id;
  std::string source_label;
  Image image;
  std::size_t input_index = 0;
};

struct MutableAtlas {
  Image image;
  uint32_t cursor_x = 0;
  uint32_t cursor_y = 0;
  uint32_t shelf_height = 0;
  uint32_t used_width = 0;
  uint32_t used_height = 0;
};

Result<MutableAtlas> make_mutable_atlas(uint32_t width, uint32_t height, uint32_t padding) {
  auto atlas_image = make_image(width, height, PixelFormat::RGBA8);
  if (!atlas_image) {
    return Result<MutableAtlas>::failure(atlas_image.error().code, atlas_image.error().message);
  }

  MutableAtlas atlas;
  atlas.image = std::move(atlas_image.value());
  atlas.cursor_x = padding;
  atlas.cursor_y = padding;
  return Result<MutableAtlas>::success(std::move(atlas));
}

void blit_rgba8(const Image& source, Image& destination, uint32_t dst_x, uint32_t dst_y) {
  const std::size_t row_bytes = static_cast<std::size_t>(source.width) * source.bytes_per_pixel();
  for (uint32_t y = 0; y < source.height; ++y) {
    const uint8_t* src = source.row_ptr(y);
    uint8_t* dst = destination.row_ptr(dst_y + y) +
                   (static_cast<std::size_t>(dst_x) * destination.bytes_per_pixel());
    std::memcpy(dst, src, row_bytes);
  }
}

UvRect make_uv_rect(const PixelRect& rect, const Image& atlas, UvOrigin origin) {
  UvRect uv_rect;
  uv_rect.x_min = static_cast<double>(rect.x) / static_cast<double>(atlas.width);
  uv_rect.x_max = static_cast<double>(rect.right()) / static_cast<double>(atlas.width);

  if (origin == UvOrigin::TopLeft) {
    uv_rect.y_min = static_cast<double>(rect.y) / static_cast<double>(atlas.height);
    uv_rect.y_max = static_cast<double>(rect.bottom()) / static_cast<double>(atlas.height);
  } else {
    uv_rect.y_min = 1.0 - (static_cast<double>(rect.bottom()) / static_cast<double>(atlas.height));
    uv_rect.y_max = 1.0 - (static_cast<double>(rect.y) / static_cast<double>(atlas.height));
  }

  return uv_rect;
}

std::string make_atlas_identifier(const std::string& prefix, std::size_t index) {
  std::ostringstream stream;
  stream << prefix << "_" << std::setw(4) << std::setfill('0') << index;
  return stream.str();
}

}  // namespace

Result<AtlasPackResult> pack_atlases(const std::vector<PackItem>& items,
                                     const AtlasPackOptions& options) {
  if (options.max_atlas_width == 0 || options.max_atlas_height == 0) {
    return Result<AtlasPackResult>::failure(ErrorCode::InvalidArgument,
                                            "maximum atlas size must be non-zero");
  }

  if (items.empty()) {
    return Result<AtlasPackResult>::success(AtlasPackResult{});
  }

  std::set<std::string> ids;
  std::vector<PreparedItem> prepared;
  prepared.reserve(items.size());

  for (std::size_t index = 0; index < items.size(); ++index) {
    const PackItem& item = items[index];
    if (item.entry_id.empty()) {
      return Result<AtlasPackResult>::failure(ErrorCode::InvalidArgument,
                                              "pack item entry IDs must be non-empty");
    }
    if (!ids.insert(item.entry_id).second) {
      return Result<AtlasPackResult>::failure(ErrorCode::InvalidArgument,
                                              "pack item entry IDs must be unique");
    }

    const auto validation = item.image.validate();
    if (!validation) {
      return Result<AtlasPackResult>::failure(validation.error().code, validation.error().message);
    }
    if (item.image.empty()) {
      return Result<AtlasPackResult>::failure(ErrorCode::InvalidArgument,
                                              "pack items must have non-zero image dimensions");
    }

    auto rgba = convert_image(item.image, PixelFormat::RGBA8);
    if (!rgba) {
      return Result<AtlasPackResult>::failure(rgba.error().code, rgba.error().message);
    }

    const uint32_t padded_width = rgba.value().width + (options.padding * 2U);
    const uint32_t padded_height = rgba.value().height + (options.padding * 2U);
    if (padded_width > options.max_atlas_width || padded_height > options.max_atlas_height) {
      return Result<AtlasPackResult>::failure(
          ErrorCode::PackingFailed,
          "at least one image is larger than the allowed atlas size once padding is applied");
    }

    PreparedItem prepared_item;
    prepared_item.entry_id = item.entry_id;
    prepared_item.source_label = item.source_label;
    prepared_item.image = std::move(rgba.value());
    prepared_item.input_index = index;
    prepared.push_back(std::move(prepared_item));
  }

  if (options.sort_order == PackSortOrder::HeightWidthId) {
    std::stable_sort(prepared.begin(),
                     prepared.end(),
                     [](const PreparedItem& lhs, const PreparedItem& rhs) {
                       if (lhs.image.height != rhs.image.height) {
                         return lhs.image.height > rhs.image.height;
                       }
                       if (lhs.image.width != rhs.image.width) {
                         return lhs.image.width > rhs.image.width;
                       }
                       return lhs.entry_id < rhs.entry_id;
                     });
  }

  std::vector<MutableAtlas> atlases;
  std::vector<PackedPlacement> placements;

  auto start_new_atlas = [&]() -> Result<void> {
    auto atlas = make_mutable_atlas(options.max_atlas_width,
                                    options.max_atlas_height,
                                    options.padding);
    if (!atlas) {
      return Result<void>::failure(atlas.error().code, atlas.error().message);
    }
    atlases.push_back(std::move(atlas.value()));
    return Result<void>::success();
  };

  for (const PreparedItem& item : prepared) {
    if (atlases.empty()) {
      auto started = start_new_atlas();
      if (!started) {
        return Result<AtlasPackResult>::failure(started.error().code, started.error().message);
      }
    }

    bool placed = false;
    while (!placed) {
      MutableAtlas& atlas = atlases.back();
      if (atlas.cursor_x + item.image.width + options.padding > options.max_atlas_width) {
        atlas.cursor_x = options.padding;
        atlas.cursor_y += atlas.shelf_height + options.padding;
        atlas.shelf_height = 0;
      }

      if (atlas.cursor_y + item.image.height + options.padding > options.max_atlas_height) {
        auto started = start_new_atlas();
        if (!started) {
          return Result<AtlasPackResult>::failure(started.error().code, started.error().message);
        }
        continue;
      }

      const uint32_t x = atlas.cursor_x;
      const uint32_t y = atlas.cursor_y;
      blit_rgba8(item.image, atlas.image, x, y);

      atlas.cursor_x += item.image.width + options.padding;
      atlas.shelf_height = std::max(atlas.shelf_height, item.image.height);
      atlas.used_width = std::max(atlas.used_width, x + item.image.width + options.padding);
      atlas.used_height = std::max(atlas.used_height, y + item.image.height + options.padding);

      PackedPlacement placement;
      placement.entry_id = item.entry_id;
      placement.source_label = item.source_label;
      placement.atlas_index = atlases.size() - 1U;
      placement.pixel_rect = PixelRect{
          static_cast<int32_t>(x),
          static_cast<int32_t>(y),
          static_cast<int32_t>(item.image.width),
          static_cast<int32_t>(item.image.height),
      };
      placements.push_back(std::move(placement));
      placed = true;
    }
  }

  AtlasPackResult result;
  result.placements = std::move(placements);

  for (std::size_t index = 0; index < atlases.size(); ++index) {
    MutableAtlas& atlas = atlases[index];
    const PixelRect used_rect{
        0,
        0,
        static_cast<int32_t>(atlas.used_width),
        static_cast<int32_t>(atlas.used_height),
    };

    auto cropped = crop_image(atlas.image, used_rect);
    if (!cropped) {
      return Result<AtlasPackResult>::failure(cropped.error().code, cropped.error().message);
    }

    Image final_atlas = std::move(cropped.value());
    if (options.atlas_pixel_format != PixelFormat::RGBA8) {
      auto converted = convert_image(final_atlas, options.atlas_pixel_format);
      if (!converted) {
        return Result<AtlasPackResult>::failure(converted.error().code, converted.error().message);
      }
      final_atlas = std::move(converted.value());
    }

    PackedAtlas packed_atlas;
    packed_atlas.atlas_identifier = make_atlas_identifier(options.atlas_identifier_prefix, index);
    packed_atlas.image = std::move(final_atlas);
    result.atlases.push_back(std::move(packed_atlas));
  }

  for (PackedPlacement& placement : result.placements) {
    placement.uv_rect = make_uv_rect(placement.pixel_rect,
                                     result.atlases[placement.atlas_index].image,
                                     options.output_uv_origin);
  }

  return Result<AtlasPackResult>::success(std::move(result));
}

}  // namespace libatlas
