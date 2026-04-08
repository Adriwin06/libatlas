#pragma once

#include <string>
#include <vector>

#include "libatlas/identity.hpp"
#include "libatlas/similarity.hpp"

namespace libatlas {

enum class ExtractionWarning {
  SwappedXBounds,
  SwappedYBounds,
  ClampedToAtlasBounds,
  DegenerateCrop,
  TransparentBordersTrimmed,
  TrimmedToEmpty
};

const char* extraction_warning_to_string(ExtractionWarning warning) noexcept;

struct ExtractionOptions {
  UvOrigin uv_origin = UvOrigin::TopLeft;
  UvRoundingPolicy rounding_policy = UvRoundingPolicy::Expand;
  bool trim_transparent_borders = true;
  uint8_t transparent_alpha_threshold = 0;
  bool compute_similarity_signature = true;
  std::string source_atlas_identifier;
};

struct ResolvedUvRect {
  PixelRect requested_rect;
  PixelRect clamped_rect;
  std::vector<ExtractionWarning> warnings;
};

struct ExtractionMetadata {
  std::string source_atlas_identifier;
  uint32_t source_atlas_width = 0;
  uint32_t source_atlas_height = 0;
  UvRect requested_uv_rect;
  PixelRect resolved_pixel_rect;
  PixelRect clamped_pixel_rect;
  PixelRect trimmed_pixel_rect;
  PixelRect trimmed_rect_in_crop;
  uint32_t cropped_width = 0;
  uint32_t cropped_height = 0;
  uint32_t trimmed_width = 0;
  uint32_t trimmed_height = 0;
  double cropped_alpha_coverage = 0.0;
  double trimmed_alpha_coverage = 0.0;
  CanonicalTextureId exact_id;
  SimilaritySignature similarity_signature;
  bool has_similarity_signature = false;
  std::vector<ExtractionWarning> warnings;
};

struct ExtractedTexture {
  Image cropped_image;
  Image trimmed_image;
  ExtractionMetadata metadata;
};

Result<ResolvedUvRect> resolve_uv_rect(const Image& atlas,
                                       const UvRect& uv_rect,
                                       const ExtractionOptions& options = {});

Result<ExtractedTexture> extract_texture(const Image& atlas,
                                        const UvRect& uv_rect,
                                        const ExtractionOptions& options = {});

}  // namespace libatlas
