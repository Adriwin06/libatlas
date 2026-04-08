#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "libatlas/extraction.hpp"

namespace libatlas {

enum class PackSortOrder {
  HeightWidthId,
  InputOrder
};

struct PackItem {
  std::string entry_id;
  Image image;
  std::string source_label;
};

struct AtlasPackOptions {
  uint32_t max_atlas_width = 1024;
  uint32_t max_atlas_height = 1024;
  uint32_t padding = 1;
  UvOrigin output_uv_origin = UvOrigin::TopLeft;
  PixelFormat atlas_pixel_format = PixelFormat::RGBA8;
  PackSortOrder sort_order = PackSortOrder::HeightWidthId;
  std::string atlas_identifier_prefix = "atlas";
};

struct PackedPlacement {
  std::string entry_id;
  std::string source_label;
  std::size_t atlas_index = 0;
  PixelRect pixel_rect;
  UvRect uv_rect;
};

struct PackedAtlas {
  std::string atlas_identifier;
  Image image;
};

struct AtlasPackResult {
  std::vector<PackedAtlas> atlases;
  std::vector<PackedPlacement> placements;
  std::vector<std::string> warnings;
};

Result<AtlasPackResult> pack_atlases(const std::vector<PackItem>& items,
                                     const AtlasPackOptions& options = {});

}  // namespace libatlas
