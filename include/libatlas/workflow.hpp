#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "libatlas/packing.hpp"

namespace libatlas {

enum class RepresentativeImageKind {
  Trimmed,
  Cropped
};

struct TextureOccurrence {
  std::string occurrence_id;
  ExtractedTexture texture;
};

struct DeduplicationOptions {
  RepresentativeImageKind representative_image_kind = RepresentativeImageKind::Trimmed;
  bool use_full_exact_id_string = true;
};

struct LogicalTexture {
  CanonicalTextureId exact_id;
  std::string entry_id;
  Image image;
  ExtractionMetadata representative_metadata;
  std::size_t representative_occurrence_index = 0;
  std::vector<std::size_t> occurrence_indices;
  std::vector<std::string> occurrence_ids;
};

struct OccurrenceLogicalMapping {
  std::size_t occurrence_index = 0;
  std::string occurrence_id;
  CanonicalTextureId exact_id;
  std::string entry_id;
  std::size_t logical_texture_index = 0;
};

struct DeduplicationResult {
  std::vector<LogicalTexture> logical_textures;
  std::vector<OccurrenceLogicalMapping> occurrence_mappings;
};

struct PackedOccurrenceMapping {
  std::size_t occurrence_index = 0;
  std::string occurrence_id;
  CanonicalTextureId exact_id;
  std::string entry_id;
  std::size_t logical_texture_index = 0;
  std::size_t atlas_index = 0;
  std::string atlas_identifier;
  PixelRect pixel_rect;
  UvRect uv_rect;
};

struct PackedOccurrenceResult {
  std::vector<PackedOccurrenceMapping> occurrence_mappings;
};

struct DeduplicateAndPackResult {
  DeduplicationResult deduplicated;
  AtlasPackResult packed;
  PackedOccurrenceResult remapped;
};

Result<DeduplicationResult> deduplicate_extractions_by_exact_id(
    const std::vector<TextureOccurrence>& occurrences,
    const DeduplicationOptions& options = {});

Result<std::vector<PackItem>> build_pack_items_from_deduplication(
    const DeduplicationResult& deduplicated);

Result<PackedOccurrenceResult> remap_deduplicated_occurrences(
    const DeduplicationResult& deduplicated,
    const AtlasPackResult& packed);

Result<DeduplicateAndPackResult> deduplicate_and_pack_occurrences(
    const std::vector<TextureOccurrence>& occurrences,
    const AtlasPackOptions& pack_options = {},
    const DeduplicationOptions& deduplication_options = {});

}  // namespace libatlas
