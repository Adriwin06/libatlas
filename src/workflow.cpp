#include "libatlas/workflow.hpp"

#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace libatlas {

namespace {

const Image& choose_representative_image(const TextureOccurrence& occurrence,
                                         RepresentativeImageKind image_kind) {
  return image_kind == RepresentativeImageKind::Trimmed ? occurrence.texture.trimmed_image
                                                        : occurrence.texture.cropped_image;
}

std::string make_entry_id(const CanonicalTextureId& exact_id, bool use_full_exact_id_string) {
  return use_full_exact_id_string ? exact_id.to_string() : exact_id.hex();
}

std::string choose_source_label(const LogicalTexture& logical_texture) {
  for (const std::string& occurrence_id : logical_texture.occurrence_ids) {
    if (!occurrence_id.empty()) {
      return occurrence_id;
    }
  }
  if (!logical_texture.representative_metadata.source_atlas_identifier.empty()) {
    return logical_texture.representative_metadata.source_atlas_identifier;
  }
  return logical_texture.entry_id;
}

}  // namespace

Result<DeduplicationResult> deduplicate_extractions_by_exact_id(
    const std::vector<TextureOccurrence>& occurrences,
    const DeduplicationOptions& options) {
  DeduplicationResult result;
  result.occurrence_mappings.reserve(occurrences.size());

  std::map<std::string, std::size_t> logical_texture_index_by_entry_id;
  for (std::size_t occurrence_index = 0; occurrence_index < occurrences.size(); ++occurrence_index) {
    const TextureOccurrence& occurrence = occurrences[occurrence_index];
    const CanonicalTextureId& exact_id = occurrence.texture.metadata.exact_id;
    if (exact_id.empty()) {
      return Result<DeduplicationResult>::failure(
          ErrorCode::InvalidArgument,
          "each occurrence must contain a non-empty exact_id computed by extract_texture");
    }

    const Image& representative_image =
        choose_representative_image(occurrence, options.representative_image_kind);
    const auto representative_validation = representative_image.validate();
    if (!representative_validation) {
      return Result<DeduplicationResult>::failure(representative_validation.error().code,
                                                  representative_validation.error().message);
    }

    const std::string entry_id = make_entry_id(exact_id, options.use_full_exact_id_string);
    auto iterator = logical_texture_index_by_entry_id.find(entry_id);
    std::size_t logical_texture_index = 0;

    if (iterator == logical_texture_index_by_entry_id.end()) {
      logical_texture_index = result.logical_textures.size();

      LogicalTexture logical_texture;
      logical_texture.exact_id = exact_id;
      logical_texture.entry_id = entry_id;
      logical_texture.image = representative_image;
      logical_texture.representative_metadata = occurrence.texture.metadata;
      logical_texture.representative_occurrence_index = occurrence_index;
      logical_texture.occurrence_indices.push_back(occurrence_index);
      logical_texture.occurrence_ids.push_back(occurrence.occurrence_id);
      result.logical_textures.push_back(std::move(logical_texture));

      logical_texture_index_by_entry_id.emplace(entry_id, logical_texture_index);
    } else {
      logical_texture_index = iterator->second;
      LogicalTexture& logical_texture = result.logical_textures[logical_texture_index];
      logical_texture.occurrence_indices.push_back(occurrence_index);
      logical_texture.occurrence_ids.push_back(occurrence.occurrence_id);
    }

    OccurrenceLogicalMapping mapping;
    mapping.occurrence_index = occurrence_index;
    mapping.occurrence_id = occurrence.occurrence_id;
    mapping.exact_id = exact_id;
    mapping.entry_id = entry_id;
    mapping.logical_texture_index = logical_texture_index;
    result.occurrence_mappings.push_back(std::move(mapping));
  }

  return Result<DeduplicationResult>::success(std::move(result));
}

Result<std::vector<PackItem>> build_pack_items_from_deduplication(
    const DeduplicationResult& deduplicated) {
  std::vector<PackItem> pack_items;
  pack_items.reserve(deduplicated.logical_textures.size());

  std::set<std::string> seen_entry_ids;
  for (const LogicalTexture& logical_texture : deduplicated.logical_textures) {
    if (logical_texture.entry_id.empty()) {
      return Result<std::vector<PackItem>>::failure(ErrorCode::InvalidArgument,
                                                    "logical texture entry IDs must be non-empty");
    }
    if (!seen_entry_ids.insert(logical_texture.entry_id).second) {
      return Result<std::vector<PackItem>>::failure(ErrorCode::InvalidArgument,
                                                    "logical texture entry IDs must be unique");
    }

    const auto image_validation = logical_texture.image.validate();
    if (!image_validation) {
      return Result<std::vector<PackItem>>::failure(image_validation.error().code,
                                                    image_validation.error().message);
    }
    if (logical_texture.image.empty()) {
      std::ostringstream stream;
      stream << "logical texture " << logical_texture.entry_id
             << " has an empty representative image and cannot be packed";
      return Result<std::vector<PackItem>>::failure(ErrorCode::InvalidArgument, stream.str());
    }

    PackItem item;
    item.entry_id = logical_texture.entry_id;
    item.image = logical_texture.image;
    item.source_label = choose_source_label(logical_texture);
    pack_items.push_back(std::move(item));
  }

  return Result<std::vector<PackItem>>::success(std::move(pack_items));
}

Result<PackedOccurrenceResult> remap_deduplicated_occurrences(
    const DeduplicationResult& deduplicated,
    const AtlasPackResult& packed) {
  std::map<std::string, const PackedPlacement*> placement_by_entry_id;
  for (const PackedPlacement& placement : packed.placements) {
    placement_by_entry_id.emplace(placement.entry_id, &placement);
  }

  PackedOccurrenceResult result;
  result.occurrence_mappings.reserve(deduplicated.occurrence_mappings.size());

  for (const OccurrenceLogicalMapping& mapping : deduplicated.occurrence_mappings) {
    const auto placement_iterator = placement_by_entry_id.find(mapping.entry_id);
    if (placement_iterator == placement_by_entry_id.end()) {
      std::ostringstream stream;
      stream << "no packed placement exists for logical entry ID " << mapping.entry_id;
      return Result<PackedOccurrenceResult>::failure(ErrorCode::InvalidArgument, stream.str());
    }

    const PackedPlacement& placement = *placement_iterator->second;
    if (placement.atlas_index >= packed.atlases.size()) {
      return Result<PackedOccurrenceResult>::failure(
          ErrorCode::InvalidArgument,
          "packed placement references an atlas index outside the atlas list");
    }

    PackedOccurrenceMapping remapped;
    remapped.occurrence_index = mapping.occurrence_index;
    remapped.occurrence_id = mapping.occurrence_id;
    remapped.exact_id = mapping.exact_id;
    remapped.entry_id = mapping.entry_id;
    remapped.logical_texture_index = mapping.logical_texture_index;
    remapped.atlas_index = placement.atlas_index;
    remapped.atlas_identifier = packed.atlases[placement.atlas_index].atlas_identifier;
    remapped.pixel_rect = placement.pixel_rect;
    remapped.uv_rect = placement.uv_rect;
    result.occurrence_mappings.push_back(std::move(remapped));
  }

  return Result<PackedOccurrenceResult>::success(std::move(result));
}

Result<DeduplicateAndPackResult> deduplicate_and_pack_occurrences(
    const std::vector<TextureOccurrence>& occurrences,
    const AtlasPackOptions& pack_options,
    const DeduplicationOptions& deduplication_options) {
  auto deduplicated = deduplicate_extractions_by_exact_id(occurrences, deduplication_options);
  if (!deduplicated) {
    return Result<DeduplicateAndPackResult>::failure(deduplicated.error().code,
                                                     deduplicated.error().message);
  }

  auto pack_items = build_pack_items_from_deduplication(deduplicated.value());
  if (!pack_items) {
    return Result<DeduplicateAndPackResult>::failure(pack_items.error().code,
                                                     pack_items.error().message);
  }

  auto packed = pack_atlases(pack_items.value(), pack_options);
  if (!packed) {
    return Result<DeduplicateAndPackResult>::failure(packed.error().code, packed.error().message);
  }

  auto remapped = remap_deduplicated_occurrences(deduplicated.value(), packed.value());
  if (!remapped) {
    return Result<DeduplicateAndPackResult>::failure(remapped.error().code,
                                                     remapped.error().message);
  }

  DeduplicateAndPackResult result;
  result.deduplicated = std::move(deduplicated.value());
  result.packed = std::move(packed.value());
  result.remapped = std::move(remapped.value());
  return Result<DeduplicateAndPackResult>::success(std::move(result));
}

}  // namespace libatlas
