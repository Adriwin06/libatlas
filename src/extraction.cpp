#include "libatlas/extraction.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace libatlas {

namespace {

void append_warning(std::vector<ExtractionWarning>& warnings, ExtractionWarning warning) {
  if (std::find(warnings.begin(), warnings.end(), warning) == warnings.end()) {
    warnings.push_back(warning);
  }
}

int32_t apply_round(double value, bool is_min_bound, UvRoundingPolicy rounding_policy) {
  switch (rounding_policy) {
    case UvRoundingPolicy::Expand:
      return static_cast<int32_t>(is_min_bound ? std::floor(value) : std::ceil(value));
    case UvRoundingPolicy::Nearest:
      return static_cast<int32_t>(std::llround(value));
    case UvRoundingPolicy::Contract:
      return static_cast<int32_t>(is_min_bound ? std::ceil(value) : std::floor(value));
  }
  return 0;
}

bool same_rect(const PixelRect& lhs, const PixelRect& rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
}

std::string make_cropped_alias_key(const CanonicalTextureId& cropped_exact_id,
                                   bool trim_transparent_borders,
                                   uint8_t transparent_alpha_threshold) {
  return cropped_exact_id.to_string() + "|trim=" + (trim_transparent_borders ? "1" : "0") +
         "|alpha=" + std::to_string(static_cast<unsigned>(transparent_alpha_threshold));
}

CanonicalizationOptions make_untrimmed_canonicalization(uint8_t alpha_threshold) {
  CanonicalizationOptions options;
  options.trim_transparent_borders = false;
  options.transparent_alpha_threshold = alpha_threshold;
  return options;
}

void append_trim_warnings(std::vector<ExtractionWarning>& warnings,
                          const PixelRect& trimmed_rect_in_crop,
                          const Image& cropped_image,
                          const Image& resolved_image) {
  const PixelRect full_crop{
      0,
      0,
      static_cast<int32_t>(cropped_image.width),
      static_cast<int32_t>(cropped_image.height),
  };

  if (!same_rect(trimmed_rect_in_crop, full_crop)) {
    append_warning(warnings, ExtractionWarning::TransparentBordersTrimmed);
  }
  if (resolved_image.empty() && !cropped_image.empty()) {
    append_warning(warnings, ExtractionWarning::TrimmedToEmpty);
  }
}

ExtractedTexture build_extracted_texture(const Image& atlas,
                                         const UvRect& uv_rect,
                                         const ExtractionOptions& options,
                                         const ResolvedUvRect& resolved,
                                         Image cropped_image,
                                         Image resolved_image,
                                         const PixelRect& trimmed_rect_in_crop,
                                         const CanonicalTextureId& cropped_exact_id,
                                         const CanonicalTextureId& exact_id,
                                         const SimilaritySignature& similarity_signature,
                                         bool has_similarity_signature,
                                         IdentityCacheOutcome cache_outcome,
                                         std::vector<ExtractionWarning> warnings) {
  const PixelRect trimmed_pixel_rect{
      resolved.clamped_rect.x + trimmed_rect_in_crop.x,
      resolved.clamped_rect.y + trimmed_rect_in_crop.y,
      trimmed_rect_in_crop.width,
      trimmed_rect_in_crop.height,
  };

  ExtractedTexture result;
  result.cropped_image = std::move(cropped_image);
  result.trimmed_image = std::move(resolved_image);
  result.metadata.source_atlas_identifier = options.source_atlas_identifier;
  result.metadata.source_atlas_width = atlas.width;
  result.metadata.source_atlas_height = atlas.height;
  result.metadata.trim_transparent_borders_applied = options.trim_transparent_borders;
  result.metadata.transparent_alpha_threshold = options.transparent_alpha_threshold;
  result.metadata.requested_uv_rect = uv_rect;
  result.metadata.resolved_pixel_rect = resolved.requested_rect;
  result.metadata.clamped_pixel_rect = resolved.clamped_rect;
  result.metadata.trimmed_pixel_rect = trimmed_pixel_rect;
  result.metadata.trimmed_rect_in_crop = trimmed_rect_in_crop;
  result.metadata.cropped_width = result.cropped_image.width;
  result.metadata.cropped_height = result.cropped_image.height;
  result.metadata.trimmed_width = result.trimmed_image.width;
  result.metadata.trimmed_height = result.trimmed_image.height;
  result.metadata.cropped_alpha_coverage =
      compute_alpha_coverage(result.cropped_image, options.transparent_alpha_threshold);
  result.metadata.trimmed_alpha_coverage =
      compute_alpha_coverage(result.trimmed_image, options.transparent_alpha_threshold);
  result.metadata.cropped_exact_id = cropped_exact_id;
  result.metadata.exact_id = exact_id;
  result.metadata.similarity_signature = similarity_signature;
  result.metadata.has_similarity_signature = has_similarity_signature;
  result.metadata.cache_outcome = cache_outcome;
  result.metadata.warnings = std::move(warnings);
  return result;
}

}  // namespace

const char* extraction_warning_to_string(ExtractionWarning warning) noexcept {
  switch (warning) {
    case ExtractionWarning::SwappedXBounds:
      return "swapped_x_bounds";
    case ExtractionWarning::SwappedYBounds:
      return "swapped_y_bounds";
    case ExtractionWarning::ClampedToAtlasBounds:
      return "clamped_to_atlas_bounds";
    case ExtractionWarning::DegenerateCrop:
      return "degenerate_crop";
    case ExtractionWarning::TransparentBordersTrimmed:
      return "transparent_borders_trimmed";
    case ExtractionWarning::TrimmedToEmpty:
      return "trimmed_to_empty";
  }
  return "unknown";
}

const char* identity_cache_outcome_to_string(IdentityCacheOutcome outcome) noexcept {
  switch (outcome) {
    case IdentityCacheOutcome::NotApplied:
      return "not_applied";
    case IdentityCacheOutcome::CroppedExactHit:
      return "cropped_exact_hit";
    case IdentityCacheOutcome::ExactIdHit:
      return "exact_id_hit";
    case IdentityCacheOutcome::NewEntry:
      return "new_entry";
  }
  return "unknown";
}

Result<void> ExtractionIdentityCache::add_cached_identity(
    const CanonicalTextureId& cropped_exact_id,
    const CanonicalTextureId& exact_id,
    const Image& resolved_image,
    const PixelRect& trimmed_rect_in_crop,
    bool trim_transparent_borders,
    uint8_t transparent_alpha_threshold,
    const SimilaritySignature* similarity_signature) {
  if (cropped_exact_id.empty() || exact_id.empty()) {
    return Result<void>::failure(ErrorCode::InvalidArgument,
                                 "cache entries require non-empty cropped and exact IDs");
  }

  const auto image_validation = resolved_image.validate();
  if (!image_validation) {
    return Result<void>::failure(image_validation.error().code, image_validation.error().message);
  }

  const std::string exact_key = exact_id.to_string();
  std::size_t exact_entry_index = 0;
  auto exact_iterator = exact_index_by_id_.find(exact_key);
  if (exact_iterator == exact_index_by_id_.end()) {
    exact_entry_index = exact_entries_.size();
    ExactEntry exact_entry;
    exact_entry.exact_id = exact_id;
    exact_entry.resolved_image = resolved_image;
    if (similarity_signature != nullptr) {
      exact_entry.similarity_signature = *similarity_signature;
      exact_entry.has_similarity_signature = true;
    }
    exact_entries_.push_back(std::move(exact_entry));
    exact_index_by_id_.emplace(exact_key, exact_entry_index);
  } else {
    exact_entry_index = exact_iterator->second;
    ExactEntry& existing = exact_entries_[exact_entry_index];
    if (similarity_signature != nullptr && !existing.has_similarity_signature) {
      existing.similarity_signature = *similarity_signature;
      existing.has_similarity_signature = true;
    }
  }

  const std::string alias_key =
      make_cropped_alias_key(cropped_exact_id, trim_transparent_borders, transparent_alpha_threshold);
  auto alias_iterator = cropped_index_by_id_.find(alias_key);
  if (alias_iterator != cropped_index_by_id_.end()) {
    const CroppedAliasEntry& existing = cropped_alias_entries_[alias_iterator->second];
    if (existing.exact_entry_index != exact_entry_index ||
        !same_rect(existing.trimmed_rect_in_crop, trimmed_rect_in_crop)) {
      return Result<void>::failure(
          ErrorCode::InvalidArgument,
          "cropped exact ID alias already exists with a different cached identity");
    }
    return Result<void>::success();
  }

  CroppedAliasEntry alias_entry;
  alias_entry.cropped_exact_id = cropped_exact_id;
  alias_entry.exact_entry_index = exact_entry_index;
  alias_entry.trimmed_rect_in_crop = trimmed_rect_in_crop;
  cropped_alias_entries_.push_back(std::move(alias_entry));
  cropped_index_by_id_.emplace(alias_key, cropped_alias_entries_.size() - 1U);
  return Result<void>::success();
}

void ExtractionIdentityCache::clear() {
  exact_entries_.clear();
  cropped_alias_entries_.clear();
  exact_index_by_id_.clear();
  cropped_index_by_id_.clear();
}

std::size_t ExtractionIdentityCache::exact_entry_count() const noexcept {
  return exact_entries_.size();
}

std::size_t ExtractionIdentityCache::cropped_alias_count() const noexcept {
  return cropped_alias_entries_.size();
}

Result<ResolvedUvRect> resolve_uv_rect(const Image& atlas,
                                       const UvRect& uv_rect,
                                       const ExtractionOptions& options) {
  const auto validation = atlas.validate();
  if (!validation) {
    return Result<ResolvedUvRect>::failure(validation.error().code, validation.error().message);
  }

  if (!std::isfinite(uv_rect.x_min) || !std::isfinite(uv_rect.x_max) ||
      !std::isfinite(uv_rect.y_min) || !std::isfinite(uv_rect.y_max)) {
    return Result<ResolvedUvRect>::failure(ErrorCode::InvalidArgument,
                                           "UV rectangles must use finite numeric values");
  }

  double x_min = uv_rect.x_min;
  double x_max = uv_rect.x_max;
  double y_min = uv_rect.y_min;
  double y_max = uv_rect.y_max;

  std::vector<ExtractionWarning> warnings;
  if (x_min > x_max) {
    std::swap(x_min, x_max);
    append_warning(warnings, ExtractionWarning::SwappedXBounds);
  }
  if (y_min > y_max) {
    std::swap(y_min, y_max);
    append_warning(warnings, ExtractionWarning::SwappedYBounds);
  }

  const double left_uv = x_min;
  const double right_uv = x_max;
  const double top_uv = options.uv_origin == UvOrigin::TopLeft ? y_min : (1.0 - y_max);
  const double bottom_uv = options.uv_origin == UvOrigin::TopLeft ? y_max : (1.0 - y_min);

  const int32_t left = apply_round(left_uv * static_cast<double>(atlas.width),
                                   true,
                                   options.rounding_policy);
  const int32_t right = apply_round(right_uv * static_cast<double>(atlas.width),
                                    false,
                                    options.rounding_policy);
  const int32_t top = apply_round(top_uv * static_cast<double>(atlas.height),
                                  true,
                                  options.rounding_policy);
  const int32_t bottom = apply_round(bottom_uv * static_cast<double>(atlas.height),
                                     false,
                                     options.rounding_policy);

  ResolvedUvRect resolved;
  resolved.requested_rect = PixelRect{
      left,
      top,
      std::max<int32_t>(0, right - left),
      std::max<int32_t>(0, bottom - top),
  };

  const int32_t clamped_left = std::clamp(left, 0, static_cast<int32_t>(atlas.width));
  const int32_t clamped_right = std::clamp(right, 0, static_cast<int32_t>(atlas.width));
  const int32_t clamped_top = std::clamp(top, 0, static_cast<int32_t>(atlas.height));
  const int32_t clamped_bottom = std::clamp(bottom, 0, static_cast<int32_t>(atlas.height));

  resolved.clamped_rect = PixelRect{
      clamped_left,
      clamped_top,
      std::max<int32_t>(0, clamped_right - clamped_left),
      std::max<int32_t>(0, clamped_bottom - clamped_top),
  };

  if (left != clamped_left || right != clamped_right || top != clamped_top ||
      bottom != clamped_bottom) {
    append_warning(warnings, ExtractionWarning::ClampedToAtlasBounds);
  }
  if (resolved.clamped_rect.empty()) {
    append_warning(warnings, ExtractionWarning::DegenerateCrop);
  }

  resolved.warnings = std::move(warnings);
  return Result<ResolvedUvRect>::success(std::move(resolved));
}

Result<ExtractedTexture> extract_texture(const Image& atlas,
                                         const UvRect& uv_rect,
                                         const ExtractionOptions& options) {
  return extract_texture_cached(atlas, uv_rect, options, nullptr);
}

Result<ExtractedTexture> extract_texture_cached(const Image& atlas,
                                               const UvRect& uv_rect,
                                               const ExtractionOptions& options,
                                               ExtractionIdentityCache* identity_cache) {
  auto resolved = resolve_uv_rect(atlas, uv_rect, options);
  if (!resolved) {
    return Result<ExtractedTexture>::failure(resolved.error().code, resolved.error().message);
  }

  auto cropped = crop_image(atlas, resolved.value().clamped_rect);
  if (!cropped) {
    return Result<ExtractedTexture>::failure(cropped.error().code, cropped.error().message);
  }

  Image cropped_image = std::move(cropped.value());
  auto cropped_exact_id = compute_canonical_texture_id(
      cropped_image,
      make_untrimmed_canonicalization(options.transparent_alpha_threshold));
  if (!cropped_exact_id) {
    return Result<ExtractedTexture>::failure(cropped_exact_id.error().code,
                                             cropped_exact_id.error().message);
  }

  std::vector<ExtractionWarning> warnings = resolved.value().warnings;
  SimilaritySignature similarity_signature;
  bool has_similarity_signature = false;

  if (identity_cache != nullptr) {
    const std::string alias_key = make_cropped_alias_key(cropped_exact_id.value(),
                                                         options.trim_transparent_borders,
                                                         options.transparent_alpha_threshold);
    const auto alias_iterator = identity_cache->cropped_index_by_id_.find(alias_key);
    if (alias_iterator != identity_cache->cropped_index_by_id_.end()) {
      const auto& alias_entry =
          identity_cache->cropped_alias_entries_[alias_iterator->second];
      auto& exact_entry = identity_cache->exact_entries_[alias_entry.exact_entry_index];

      if (options.compute_similarity_signature) {
        if (exact_entry.has_similarity_signature) {
          similarity_signature = exact_entry.similarity_signature;
          has_similarity_signature = true;
        } else {
          SimilarityOptions similarity_options;
          similarity_options.trim_transparent_borders = false;
          similarity_options.transparent_alpha_threshold = options.transparent_alpha_threshold;

          auto similarity = compute_similarity_signature(exact_entry.resolved_image, similarity_options);
          if (!similarity) {
            return Result<ExtractedTexture>::failure(similarity.error().code,
                                                     similarity.error().message);
          }
          similarity_signature = similarity.value();
          has_similarity_signature = true;
          exact_entry.similarity_signature = similarity.value();
          exact_entry.has_similarity_signature = true;
        }
      }

      append_trim_warnings(warnings, alias_entry.trimmed_rect_in_crop, cropped_image, exact_entry.resolved_image);
      return Result<ExtractedTexture>::success(build_extracted_texture(
          atlas,
          uv_rect,
          options,
          resolved.value(),
          std::move(cropped_image),
          exact_entry.resolved_image,
          alias_entry.trimmed_rect_in_crop,
          cropped_exact_id.value(),
          exact_entry.exact_id,
          similarity_signature,
          has_similarity_signature,
          IdentityCacheOutcome::CroppedExactHit,
          std::move(warnings)));
    }
  }

  Image resolved_image = cropped_image;
  PixelRect trimmed_rect_in_crop{
      0,
      0,
      static_cast<int32_t>(cropped_image.width),
      static_cast<int32_t>(cropped_image.height),
  };

  if (options.trim_transparent_borders) {
    auto trimmed = trim_transparent_borders(cropped_image,
                                            options.transparent_alpha_threshold,
                                            &trimmed_rect_in_crop);
    if (!trimmed) {
      return Result<ExtractedTexture>::failure(trimmed.error().code, trimmed.error().message);
    }
    resolved_image = std::move(trimmed.value());
    append_trim_warnings(warnings, trimmed_rect_in_crop, cropped_image, resolved_image);
  }

  auto exact_id = compute_canonical_texture_id(
      resolved_image,
      make_untrimmed_canonicalization(options.transparent_alpha_threshold));
  if (!exact_id) {
    return Result<ExtractedTexture>::failure(exact_id.error().code, exact_id.error().message);
  }

  IdentityCacheOutcome cache_outcome =
      identity_cache != nullptr ? IdentityCacheOutcome::NewEntry : IdentityCacheOutcome::NotApplied;

  ExtractionIdentityCache::ExactEntry* cached_exact_entry = nullptr;
  if (identity_cache != nullptr) {
    const auto exact_iterator = identity_cache->exact_index_by_id_.find(exact_id.value().to_string());
    if (exact_iterator != identity_cache->exact_index_by_id_.end()) {
      cached_exact_entry = &identity_cache->exact_entries_[exact_iterator->second];
      cache_outcome = IdentityCacheOutcome::ExactIdHit;
    }
  }

  if (options.compute_similarity_signature) {
    if (cached_exact_entry != nullptr && cached_exact_entry->has_similarity_signature) {
      similarity_signature = cached_exact_entry->similarity_signature;
      has_similarity_signature = true;
    } else {
      SimilarityOptions similarity_options;
      similarity_options.trim_transparent_borders = false;
      similarity_options.transparent_alpha_threshold = options.transparent_alpha_threshold;

      auto similarity = compute_similarity_signature(resolved_image, similarity_options);
      if (!similarity) {
        return Result<ExtractedTexture>::failure(similarity.error().code,
                                                 similarity.error().message);
      }
      similarity_signature = similarity.value();
      has_similarity_signature = true;

      if (cached_exact_entry != nullptr && !cached_exact_entry->has_similarity_signature) {
        cached_exact_entry->similarity_signature = similarity.value();
        cached_exact_entry->has_similarity_signature = true;
      }
    }
  }

  if (identity_cache != nullptr) {
    const SimilaritySignature* similarity_ptr = has_similarity_signature ? &similarity_signature : nullptr;
    auto cache_result = identity_cache->add_cached_identity(cropped_exact_id.value(),
                                                            exact_id.value(),
                                                            resolved_image,
                                                            trimmed_rect_in_crop,
                                                            options.trim_transparent_borders,
                                                            options.transparent_alpha_threshold,
                                                            similarity_ptr);
    if (!cache_result) {
      return Result<ExtractedTexture>::failure(cache_result.error().code,
                                               cache_result.error().message);
    }
  }

  return Result<ExtractedTexture>::success(build_extracted_texture(atlas,
                                                                  uv_rect,
                                                                  options,
                                                                  resolved.value(),
                                                                  std::move(cropped_image),
                                                                  std::move(resolved_image),
                                                                  trimmed_rect_in_crop,
                                                                  cropped_exact_id.value(),
                                                                  exact_id.value(),
                                                                  similarity_signature,
                                                                  has_similarity_signature,
                                                                  cache_outcome,
                                                                  std::move(warnings)));
}

}  // namespace libatlas
