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

enum class SimilarityCandidateKind {
  None,
  ReviewCandidate,
  AutoDuplicateCandidate
};

const char* similarity_candidate_kind_to_string(SimilarityCandidateKind kind) noexcept;

struct SimilarityClassificationOptions {
  SimilarityOptions similarity_options;
  double review_min_score = 0.90;
  double auto_min_score = 0.92;
  int auto_max_luminance_distance = 8;
  int auto_max_alpha_distance = 8;
  double auto_max_aspect_ratio_delta = 0.10;
  double auto_min_dimension_ratio = 0.90;
};

struct SimilarityClassification {
  SimilarityComparison comparison;
  double alpha_coverage_delta = 0.0;
  SimilarityCandidateKind candidate_kind = SimilarityCandidateKind::None;
};

Result<SimilaritySignature> compute_similarity_signature(
    const Image& image,
    const SimilarityOptions& options = {});

SimilarityComparison compare_similarity(
    const SimilaritySignature& lhs,
    const SimilaritySignature& rhs,
    const SimilarityOptions& options = {});

SimilarityClassification classify_similarity(
    const SimilaritySignature& lhs,
    const SimilaritySignature& rhs,
    const SimilarityClassificationOptions& options = {});

}  // namespace libatlas
