#pragma once

#include <map>
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

enum class IdentityCacheOutcome {
  NotApplied,
  CroppedExactHit,
  ExactIdHit,
  NewEntry
};

const char* identity_cache_outcome_to_string(IdentityCacheOutcome outcome) noexcept;

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
  bool trim_transparent_borders_applied = false;
  uint8_t transparent_alpha_threshold = 0;
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
  CanonicalTextureId cropped_exact_id;
  CanonicalTextureId exact_id;
  SimilaritySignature similarity_signature;
  bool has_similarity_signature = false;
  IdentityCacheOutcome cache_outcome = IdentityCacheOutcome::NotApplied;
  std::vector<ExtractionWarning> warnings;
};

struct ExtractedTexture {
  Image cropped_image;
  Image trimmed_image;
  ExtractionMetadata metadata;
};

class ExtractionIdentityCache {
 public:
  Result<void> add_cached_identity(const CanonicalTextureId& cropped_exact_id,
                                   const CanonicalTextureId& exact_id,
                                   const Image& resolved_image,
                                   const PixelRect& trimmed_rect_in_crop,
                                   bool trim_transparent_borders,
                                   uint8_t transparent_alpha_threshold,
                                   const SimilaritySignature* similarity_signature = nullptr);

  void clear();
  std::size_t exact_entry_count() const noexcept;
  std::size_t cropped_alias_count() const noexcept;

 private:
  struct ExactEntry {
    CanonicalTextureId exact_id;
    Image resolved_image;
    SimilaritySignature similarity_signature;
    bool has_similarity_signature = false;
  };

  struct CroppedAliasEntry {
    CanonicalTextureId cropped_exact_id;
    std::size_t exact_entry_index = 0;
    PixelRect trimmed_rect_in_crop;
  };

  std::vector<ExactEntry> exact_entries_;
  std::vector<CroppedAliasEntry> cropped_alias_entries_;
  std::map<std::string, std::size_t> exact_index_by_id_;
  std::map<std::string, std::size_t> cropped_index_by_id_;

  friend Result<ExtractedTexture> extract_texture_cached(const Image& atlas,
                                                         const UvRect& uv_rect,
                                                         const ExtractionOptions& options,
                                                         ExtractionIdentityCache* identity_cache);
};

Result<ResolvedUvRect> resolve_uv_rect(const Image& atlas,
                                       const UvRect& uv_rect,
                                       const ExtractionOptions& options = {});

Result<ExtractedTexture> extract_texture(const Image& atlas,
                                        const UvRect& uv_rect,
                                        const ExtractionOptions& options = {});

Result<ExtractedTexture> extract_texture_cached(const Image& atlas,
                                               const UvRect& uv_rect,
                                               const ExtractionOptions& options,
                                               ExtractionIdentityCache* identity_cache);

}  // namespace libatlas
