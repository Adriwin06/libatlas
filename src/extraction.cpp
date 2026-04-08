#include "libatlas/extraction.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
  auto resolved = resolve_uv_rect(atlas, uv_rect, options);
  if (!resolved) {
    return Result<ExtractedTexture>::failure(resolved.error().code, resolved.error().message);
  }

  auto cropped = crop_image(atlas, resolved.value().clamped_rect);
  if (!cropped) {
    return Result<ExtractedTexture>::failure(cropped.error().code, cropped.error().message);
  }

  Image cropped_image = std::move(cropped.value());
  Image trimmed_image = cropped_image;
  PixelRect trimmed_rect_in_crop{0,
                                 0,
                                 static_cast<int32_t>(cropped_image.width),
                                 static_cast<int32_t>(cropped_image.height)};
  std::vector<ExtractionWarning> warnings = resolved.value().warnings;

  if (options.trim_transparent_borders) {
    auto trimmed = trim_transparent_borders(cropped_image,
                                            options.transparent_alpha_threshold,
                                            &trimmed_rect_in_crop);
    if (!trimmed) {
      return Result<ExtractedTexture>::failure(trimmed.error().code, trimmed.error().message);
    }
    trimmed_image = std::move(trimmed.value());

    const bool trimmed_changed =
        trimmed_rect_in_crop.x != 0 || trimmed_rect_in_crop.y != 0 ||
        trimmed_rect_in_crop.width != static_cast<int32_t>(cropped_image.width) ||
        trimmed_rect_in_crop.height != static_cast<int32_t>(cropped_image.height);
    if (trimmed_changed) {
      append_warning(warnings, ExtractionWarning::TransparentBordersTrimmed);
    }
    if (trimmed_image.empty() && !cropped_image.empty()) {
      append_warning(warnings, ExtractionWarning::TrimmedToEmpty);
    }
  }

  const Image& identity_source = options.trim_transparent_borders ? trimmed_image : cropped_image;
  CanonicalizationOptions canonicalization_options;
  canonicalization_options.trim_transparent_borders = false;
  canonicalization_options.transparent_alpha_threshold = options.transparent_alpha_threshold;

  auto exact_id = compute_canonical_texture_id(identity_source, canonicalization_options);
  if (!exact_id) {
    return Result<ExtractedTexture>::failure(exact_id.error().code, exact_id.error().message);
  }

  SimilaritySignature similarity_signature;
  bool has_similarity_signature = false;
  if (options.compute_similarity_signature) {
    SimilarityOptions similarity_options;
    similarity_options.trim_transparent_borders = false;
    similarity_options.transparent_alpha_threshold = options.transparent_alpha_threshold;

    auto similarity = compute_similarity_signature(identity_source, similarity_options);
    if (!similarity) {
      return Result<ExtractedTexture>::failure(similarity.error().code,
                                               similarity.error().message);
    }
    similarity_signature = similarity.value();
    has_similarity_signature = true;
  }

  const PixelRect trimmed_pixel_rect{
      resolved.value().clamped_rect.x + trimmed_rect_in_crop.x,
      resolved.value().clamped_rect.y + trimmed_rect_in_crop.y,
      trimmed_rect_in_crop.width,
      trimmed_rect_in_crop.height,
  };

  ExtractedTexture result;
  result.cropped_image = std::move(cropped_image);
  result.trimmed_image = std::move(trimmed_image);
  result.metadata.source_atlas_identifier = options.source_atlas_identifier;
  result.metadata.source_atlas_width = atlas.width;
  result.metadata.source_atlas_height = atlas.height;
  result.metadata.requested_uv_rect = uv_rect;
  result.metadata.resolved_pixel_rect = resolved.value().requested_rect;
  result.metadata.clamped_pixel_rect = resolved.value().clamped_rect;
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
  result.metadata.exact_id = exact_id.value();
  result.metadata.similarity_signature = similarity_signature;
  result.metadata.has_similarity_signature = has_similarity_signature;
  result.metadata.warnings = std::move(warnings);
  return Result<ExtractedTexture>::success(std::move(result));
}

}  // namespace libatlas
