#include "libatlas/similarity.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace libatlas {

namespace {

int popcount64(uint64_t value) {
  int count = 0;
  while (value != 0U) {
    value &= (value - 1U);
    ++count;
  }
  return count;
}

double bilinear_sample(const Image& image, double x, double y, bool alpha_channel) {
  if (image.empty()) {
    return 0.0;
  }

  const double clamped_x = std::clamp(x, 0.0, static_cast<double>(image.width - 1U));
  const double clamped_y = std::clamp(y, 0.0, static_cast<double>(image.height - 1U));

  const uint32_t x0 = static_cast<uint32_t>(std::floor(clamped_x));
  const uint32_t y0 = static_cast<uint32_t>(std::floor(clamped_y));
  const uint32_t x1 = std::min<uint32_t>(x0 + 1U, image.width - 1U);
  const uint32_t y1 = std::min<uint32_t>(y0 + 1U, image.height - 1U);

  const double tx = clamped_x - static_cast<double>(x0);
  const double ty = clamped_y - static_cast<double>(y0);

  const auto sample_value = [alpha_channel](const RgbaPixel& pixel) {
    if (alpha_channel) {
      return static_cast<double>(pixel.a);
    }
    const double luminance = (0.2126 * static_cast<double>(pixel.r)) +
                             (0.7152 * static_cast<double>(pixel.g)) +
                             (0.0722 * static_cast<double>(pixel.b));
    return luminance * (static_cast<double>(pixel.a) / 255.0);
  };

  const double v00 = sample_value(sample_as_rgba8(image, x0, y0));
  const double v10 = sample_value(sample_as_rgba8(image, x1, y0));
  const double v01 = sample_value(sample_as_rgba8(image, x0, y1));
  const double v11 = sample_value(sample_as_rgba8(image, x1, y1));

  const double top = (v00 * (1.0 - tx)) + (v10 * tx);
  const double bottom = (v01 * (1.0 - tx)) + (v11 * tx);
  return (top * (1.0 - ty)) + (bottom * ty);
}

uint64_t compute_average_hash(const Image& image,
                              uint32_t normalized_width,
                              uint32_t normalized_height,
                              bool alpha_channel) {
  std::vector<double> values;
  values.reserve(static_cast<std::size_t>(normalized_width) * normalized_height);

  for (uint32_t y = 0; y < normalized_height; ++y) {
    for (uint32_t x = 0; x < normalized_width; ++x) {
      const double sample_x =
          ((static_cast<double>(x) + 0.5) * static_cast<double>(image.width) /
           static_cast<double>(normalized_width)) -
          0.5;
      const double sample_y =
          ((static_cast<double>(y) + 0.5) * static_cast<double>(image.height) /
           static_cast<double>(normalized_height)) -
          0.5;
      values.push_back(bilinear_sample(image, sample_x, sample_y, alpha_channel));
    }
  }

  double sum = 0.0;
  for (double value : values) {
    sum += value;
  }
  const double mean = values.empty() ? 0.0 : sum / static_cast<double>(values.size());

  uint64_t hash = 0;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (values[i] >= mean) {
      hash |= (uint64_t{1} << i);
    }
  }
  return hash;
}

double dimension_ratio(uint32_t lhs, uint32_t rhs) {
  if (lhs == 0 && rhs == 0) {
    return 1.0;
  }
  if (lhs == 0 || rhs == 0) {
    return 0.0;
  }
  const double min_value = static_cast<double>(std::min(lhs, rhs));
  const double max_value = static_cast<double>(std::max(lhs, rhs));
  return min_value / max_value;
}

double aspect_ratio(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) {
    return 0.0;
  }
  return static_cast<double>(width) / static_cast<double>(height);
}

}  // namespace

Result<SimilaritySignature> compute_similarity_signature(const Image& image,
                                                         const SimilarityOptions& options) {
  if (options.normalized_width == 0 || options.normalized_height == 0) {
    return Result<SimilaritySignature>::failure(ErrorCode::InvalidArgument,
                                                "normalized hash dimensions must be non-zero");
  }
  if ((static_cast<uint64_t>(options.normalized_width) * options.normalized_height) > 64U) {
    return Result<SimilaritySignature>::failure(
        ErrorCode::InvalidArgument,
        "normalized hash dimensions must produce at most 64 samples");
  }

  CanonicalizationOptions canonicalization;
  canonicalization.trim_transparent_borders = options.trim_transparent_borders;
  canonicalization.transparent_alpha_threshold = options.transparent_alpha_threshold;

  auto canonical = canonicalize_image(image, canonicalization);
  if (!canonical) {
    return Result<SimilaritySignature>::failure(canonical.error().code, canonical.error().message);
  }

  const Image& canonical_image = canonical.value().image;
  SimilaritySignature signature;
  signature.canonical_width = canonical_image.width;
  signature.canonical_height = canonical_image.height;
  signature.alpha_coverage =
      compute_alpha_coverage(canonical_image, options.transparent_alpha_threshold);

  if (!canonical_image.empty()) {
    signature.luminance_hash = compute_average_hash(canonical_image,
                                                    options.normalized_width,
                                                    options.normalized_height,
                                                    false);
    signature.alpha_hash = compute_average_hash(canonical_image,
                                                options.normalized_width,
                                                options.normalized_height,
                                                true);
  }

  return Result<SimilaritySignature>::success(std::move(signature));
}

SimilarityComparison compare_similarity(const SimilaritySignature& lhs,
                                        const SimilaritySignature& rhs,
                                        const SimilarityOptions& options) {
  SimilarityComparison comparison;
  comparison.luminance_distance = popcount64(lhs.luminance_hash ^ rhs.luminance_hash);
  comparison.alpha_distance = popcount64(lhs.alpha_hash ^ rhs.alpha_hash);
  comparison.dimension_ratio =
      dimension_ratio(lhs.canonical_width, rhs.canonical_width) *
      dimension_ratio(lhs.canonical_height, rhs.canonical_height);
  comparison.aspect_ratio_delta =
      std::abs(aspect_ratio(lhs.canonical_width, lhs.canonical_height) -
               aspect_ratio(rhs.canonical_width, rhs.canonical_height));

  const double luminance_score =
      1.0 - (static_cast<double>(comparison.luminance_distance) / 64.0);
  const double alpha_score = 1.0 - (static_cast<double>(comparison.alpha_distance) / 64.0);
  const double aspect_score = 1.0 - std::min(1.0, comparison.aspect_ratio_delta);
  const double dimension_score = comparison.dimension_ratio;

  comparison.score = std::clamp((luminance_score * 0.45) + (alpha_score * 0.35) +
                                    (aspect_score * 0.10) + (dimension_score * 0.10),
                                0.0,
                                1.0);

  const bool strict_threshold_match =
      comparison.luminance_distance <= options.max_luminance_distance &&
      comparison.alpha_distance <= options.max_alpha_distance &&
      comparison.aspect_ratio_delta <= options.max_aspect_ratio_delta &&
      comparison.dimension_ratio >= options.min_dimension_ratio;

  const bool score_threshold_match = comparison.score >= 0.60 &&
                                     comparison.aspect_ratio_delta <= options.max_aspect_ratio_delta &&
                                     comparison.dimension_ratio >= options.min_dimension_ratio;

  comparison.likely_related = strict_threshold_match || score_threshold_match;
  return comparison;
}

const char* similarity_candidate_kind_to_string(SimilarityCandidateKind kind) noexcept {
  switch (kind) {
    case SimilarityCandidateKind::None:
      return "none";
    case SimilarityCandidateKind::ReviewCandidate:
      return "review_candidate";
    case SimilarityCandidateKind::AutoDuplicateCandidate:
      return "auto_duplicate_candidate";
  }
  return "none";
}

SimilarityClassification classify_similarity(const SimilaritySignature& lhs,
                                             const SimilaritySignature& rhs,
                                             const SimilarityClassificationOptions& options) {
  SimilarityClassification classification;
  classification.comparison = compare_similarity(lhs, rhs, options.similarity_options);
  classification.alpha_coverage_delta = std::abs(lhs.alpha_coverage - rhs.alpha_coverage);

  if (classification.comparison.likely_related &&
      classification.comparison.score >= options.auto_min_score &&
      classification.comparison.luminance_distance <= options.auto_max_luminance_distance &&
      classification.comparison.alpha_distance <= options.auto_max_alpha_distance &&
      classification.comparison.aspect_ratio_delta <= options.auto_max_aspect_ratio_delta &&
      classification.comparison.dimension_ratio >= options.auto_min_dimension_ratio) {
    classification.candidate_kind = SimilarityCandidateKind::AutoDuplicateCandidate;
    return classification;
  }

  if (classification.comparison.likely_related &&
      classification.comparison.score >= options.review_min_score) {
    classification.candidate_kind = SimilarityCandidateKind::ReviewCandidate;
  }

  return classification;
}

}  // namespace libatlas
