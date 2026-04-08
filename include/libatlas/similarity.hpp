#pragma once

#include <cstdint>

#include "libatlas/identity.hpp"

namespace libatlas {

struct SimilarityOptions {
  bool trim_transparent_borders = true;
  uint8_t transparent_alpha_threshold = 0;
  uint32_t normalized_width = 8;
  uint32_t normalized_height = 8;
  int max_luminance_distance = 20;
  int max_alpha_distance = 20;
  double max_aspect_ratio_delta = 0.60;
  double min_dimension_ratio = 0.50;
};

struct SimilaritySignature {
  uint32_t canonical_width = 0;
  uint32_t canonical_height = 0;
  double alpha_coverage = 0.0;
  uint64_t luminance_hash = 0;
  uint64_t alpha_hash = 0;
};

struct SimilarityComparison {
  int luminance_distance = 0;
  int alpha_distance = 0;
  double dimension_ratio = 1.0;
  double aspect_ratio_delta = 0.0;
  double score = 1.0;
  bool likely_related = false;
};

Result<SimilaritySignature> compute_similarity_signature(
    const Image& image,
    const SimilarityOptions& options = {});

SimilarityComparison compare_similarity(
    const SimilaritySignature& lhs,
    const SimilaritySignature& rhs,
    const SimilarityOptions& options = {});

}  // namespace libatlas
