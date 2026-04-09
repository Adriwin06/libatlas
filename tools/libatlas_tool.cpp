#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "asset_store.hpp"
#include "picojson.h"

#include "libatlas/libatlas.hpp"

namespace fs = std::filesystem;

namespace {

struct ExtractCommand {
  fs::path atlas_path;
  fs::path requests_path;
  fs::path output_dir;
  fs::path metadata_path;
  fs::path asset_store_dir;
  libatlas::UvOrigin origin = libatlas::UvOrigin::TopLeft;
  libatlas::UvRoundingPolicy rounding = libatlas::UvRoundingPolicy::Expand;
  bool trim = true;
  uint8_t alpha_threshold = 0;
};

struct PackCommand {
  fs::path manifest_path;
  fs::path output_dir;
  fs::path metadata_path;
  std::string atlas_prefix = "atlas";
  uint32_t max_width = 1024;
  uint32_t max_height = 1024;
  uint32_t padding = 1;
  libatlas::UvOrigin origin = libatlas::UvOrigin::TopLeft;
};

struct SimilarityReportCommand {
  fs::path metadata_dir;
  fs::path output_path;
  fs::path source_map_path;
  std::size_t max_pairs = 500;
  libatlas::SimilarityClassificationOptions classification_options;
};

struct SimilarityReportGroup {
  std::string exact_id;
  std::size_t occurrence_count = 0;
  std::vector<std::string> fixtures;
  std::vector<std::string> source_atlas_identifiers;
  std::vector<std::string> source_images;
  std::string representative_name;
  std::string representative_source_atlas_identifier;
  std::string representative_source_image;
  std::string representative_trimmed_image;
  std::string representative_metadata_json;
  libatlas::SimilaritySignature signature;
};

struct SimilarityPairRecord {
  std::size_t left_index = 0;
  std::size_t right_index = 0;
  libatlas::SimilarityClassification classification;
};

struct DisjointSet {
  std::vector<std::size_t> parent;
  std::vector<std::size_t> rank;

  explicit DisjointSet(std::size_t count) : parent(count), rank(count, 0) {
    for (std::size_t index = 0; index < count; ++index) {
      parent[index] = index;
    }
  }

  std::size_t find(std::size_t value) {
    if (parent[value] != value) {
      parent[value] = find(parent[value]);
    }
    return parent[value];
  }

  void unite(std::size_t lhs, std::size_t rhs) {
    lhs = find(lhs);
    rhs = find(rhs);
    if (lhs == rhs) {
      return;
    }
    if (rank[lhs] < rank[rhs]) {
      std::swap(lhs, rhs);
    }
    parent[rhs] = lhs;
    if (rank[lhs] == rank[rhs]) {
      ++rank[lhs];
    }
  }
};

std::string read_text_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open file: " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void write_text_file(const fs::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("failed to write file: " + path.string());
  }
  output << text;
}

picojson::value load_json_file(const fs::path& path) {
  const std::string text = read_text_file(path);
  picojson::value value;
  const std::string error = picojson::parse(value, text);
  if (!error.empty()) {
    throw std::runtime_error("failed to parse JSON " + path.string() + ": " + error);
  }
  return value;
}

const picojson::object& require_object(const picojson::value& value, const std::string& context) {
  if (!value.is<picojson::object>()) {
    throw std::runtime_error(context + " must be a JSON object");
  }
  return value.get<picojson::object>();
}

const picojson::array& require_array(const picojson::value& value, const std::string& context) {
  if (!value.is<picojson::array>()) {
    throw std::runtime_error(context + " must be a JSON array");
  }
  return value.get<picojson::array>();
}

const picojson::value& require_field(const picojson::object& object,
                                     const std::string& key,
                                     const std::string& context) {
  const auto iterator = object.find(key);
  if (iterator == object.end()) {
    throw std::runtime_error(context + " is missing required field '" + key + "'");
  }
  return iterator->second;
}

std::string require_string(const picojson::object& object,
                           const std::string& key,
                           const std::string& context) {
  const auto& value = require_field(object, key, context);
  if (!value.is<std::string>()) {
    throw std::runtime_error(context + "." + key + " must be a string");
  }
  return value.get<std::string>();
}

double require_number(const picojson::object& object,
                      const std::string& key,
                      const std::string& context) {
  const auto& value = require_field(object, key, context);
  if (!value.is<double>()) {
    throw std::runtime_error(context + "." + key + " must be a number");
  }
  return value.get<double>();
}

std::string optional_string(const picojson::object& object, const std::string& key) {
  const auto iterator = object.find(key);
  if (iterator == object.end()) {
    return {};
  }
  if (!iterator->second.is<std::string>()) {
    throw std::runtime_error("field '" + key + "' must be a string when present");
  }
  return iterator->second.get<std::string>();
}

std::string sanitize_name(std::string text) {
  for (char& character : text) {
    const bool ok = (character >= 'a' && character <= 'z') ||
                    (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9') ||
                    character == '-' || character == '_';
    if (!ok) {
      character = '_';
    }
  }
  return text.empty() ? std::string("item") : text;
}

picojson::value to_json_rect(const libatlas::PixelRect& rect) {
  picojson::object object;
  object["x"] = picojson::value(static_cast<double>(rect.x));
  object["y"] = picojson::value(static_cast<double>(rect.y));
  object["width"] = picojson::value(static_cast<double>(rect.width));
  object["height"] = picojson::value(static_cast<double>(rect.height));
  return picojson::value(object);
}

picojson::value to_json_uv(const libatlas::UvRect& uv) {
  picojson::object object;
  object["x_min"] = picojson::value(uv.x_min);
  object["x_max"] = picojson::value(uv.x_max);
  object["y_min"] = picojson::value(uv.y_min);
  object["y_max"] = picojson::value(uv.y_max);
  return picojson::value(object);
}

picojson::value to_json_similarity(const libatlas::SimilaritySignature& signature) {
  picojson::object object;
  object["canonical_width"] = picojson::value(static_cast<double>(signature.canonical_width));
  object["canonical_height"] = picojson::value(static_cast<double>(signature.canonical_height));
  object["alpha_coverage"] = picojson::value(signature.alpha_coverage);
  object["luminance_hash"] = picojson::value(std::to_string(signature.luminance_hash));
  object["alpha_hash"] = picojson::value(std::to_string(signature.alpha_hash));
  return picojson::value(object);
}

libatlas::SimilaritySignature parse_similarity(const picojson::object& object,
                                               const std::string& context) {
  libatlas::SimilaritySignature signature;
  signature.canonical_width = static_cast<uint32_t>(require_number(object, "canonical_width", context));
  signature.canonical_height =
      static_cast<uint32_t>(require_number(object, "canonical_height", context));
  signature.alpha_coverage = require_number(object, "alpha_coverage", context);
  signature.luminance_hash =
      static_cast<uint64_t>(std::stoull(require_string(object, "luminance_hash", context)));
  signature.alpha_hash =
      static_cast<uint64_t>(std::stoull(require_string(object, "alpha_hash", context)));
  return signature;
}

picojson::value to_json_string_array(const std::vector<std::string>& values) {
  picojson::array array;
  for (const auto& value : values) {
    array.emplace_back(value);
  }
  return picojson::value(array);
}

picojson::value to_json_metadata(const libatlas::ExtractionMetadata& metadata) {
  picojson::object object;
  object["source_atlas_identifier"] = picojson::value(metadata.source_atlas_identifier);
  object["source_atlas_width"] = picojson::value(static_cast<double>(metadata.source_atlas_width));
  object["source_atlas_height"] = picojson::value(static_cast<double>(metadata.source_atlas_height));
  object["trim_transparent_borders_applied"] =
      picojson::value(metadata.trim_transparent_borders_applied);
  object["transparent_alpha_threshold"] =
      picojson::value(static_cast<double>(metadata.transparent_alpha_threshold));
  object["requested_uv_rect"] = to_json_uv(metadata.requested_uv_rect);
  object["resolved_pixel_rect"] = to_json_rect(metadata.resolved_pixel_rect);
  object["clamped_pixel_rect"] = to_json_rect(metadata.clamped_pixel_rect);
  object["trimmed_pixel_rect"] = to_json_rect(metadata.trimmed_pixel_rect);
  object["trimmed_rect_in_crop"] = to_json_rect(metadata.trimmed_rect_in_crop);
  object["cropped_width"] = picojson::value(static_cast<double>(metadata.cropped_width));
  object["cropped_height"] = picojson::value(static_cast<double>(metadata.cropped_height));
  object["trimmed_width"] = picojson::value(static_cast<double>(metadata.trimmed_width));
  object["trimmed_height"] = picojson::value(static_cast<double>(metadata.trimmed_height));
  object["cropped_alpha_coverage"] = picojson::value(metadata.cropped_alpha_coverage);
  object["trimmed_alpha_coverage"] = picojson::value(metadata.trimmed_alpha_coverage);
  object["cropped_exact_id"] = picojson::value(metadata.cropped_exact_id.to_string());
  object["exact_id"] = picojson::value(metadata.exact_id.to_string());
  object["cache_outcome"] =
      picojson::value(libatlas::identity_cache_outcome_to_string(metadata.cache_outcome));

  picojson::array warnings;
  for (const auto warning : metadata.warnings) {
    warnings.emplace_back(libatlas::extraction_warning_to_string(warning));
  }
  object["warnings"] = picojson::value(warnings);

  if (metadata.has_similarity_signature) {
    object["similarity_signature"] = to_json_similarity(metadata.similarity_signature);
  }
  return picojson::value(object);
}

picojson::value to_json_asset_store_paths(const libatlas_tool::StoredAssetPaths& stored) {
  picojson::object object;
  object["atlas_image"] = picojson::value(stored.atlas_image_path.generic_string());
  object["atlas_metadata"] = picojson::value(stored.atlas_metadata_path.generic_string());
  object["cropped_image"] = picojson::value(stored.cropped_image_path.generic_string());
  object["cropped_metadata"] = picojson::value(stored.cropped_metadata_path.generic_string());
  object["canonical_image"] = picojson::value(stored.canonical_image_path.generic_string());
  object["canonical_metadata"] = picojson::value(stored.canonical_metadata_path.generic_string());
  object["occurrence_metadata"] = picojson::value(stored.occurrence_metadata_path.generic_string());
  object["atlas_existed"] = picojson::value(stored.atlas_existed);
  object["cropped_existed"] = picojson::value(stored.cropped_existed);
  object["canonical_existed"] = picojson::value(stored.canonical_existed);
  return picojson::value(object);
}

libatlas::UvOrigin parse_origin(const std::string& text) {
  if (text == "top-left") {
    return libatlas::UvOrigin::TopLeft;
  }
  if (text == "bottom-left") {
    return libatlas::UvOrigin::BottomLeft;
  }
  throw std::runtime_error("invalid origin: " + text);
}

libatlas::UvRoundingPolicy parse_rounding(const std::string& text) {
  if (text == "expand") {
    return libatlas::UvRoundingPolicy::Expand;
  }
  if (text == "nearest") {
    return libatlas::UvRoundingPolicy::Nearest;
  }
  if (text == "contract") {
    return libatlas::UvRoundingPolicy::Contract;
  }
  throw std::runtime_error("invalid rounding policy: " + text);
}

std::vector<std::string> sorted_unique(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

std::map<std::string, std::string> load_source_map(const fs::path& path) {
  if (path.empty()) {
    return {};
  }

  const picojson::object root = require_object(load_json_file(path), "similarity source map");
  std::map<std::string, std::string> source_map;
  for (const auto& entry : root) {
    if (!entry.second.is<std::string>()) {
      throw std::runtime_error("similarity source map values must be strings");
    }
    source_map.emplace(entry.first, entry.second.get<std::string>());
  }
  return source_map;
}

bool pair_rank_better(const SimilarityPairRecord& lhs,
                      const SimilarityPairRecord& rhs,
                      const std::vector<SimilarityReportGroup>& groups) {
  if (lhs.classification.comparison.score != rhs.classification.comparison.score) {
    return lhs.classification.comparison.score > rhs.classification.comparison.score;
  }
  if (lhs.classification.comparison.luminance_distance != rhs.classification.comparison.luminance_distance) {
    return lhs.classification.comparison.luminance_distance <
           rhs.classification.comparison.luminance_distance;
  }
  if (lhs.classification.comparison.alpha_distance != rhs.classification.comparison.alpha_distance) {
    return lhs.classification.comparison.alpha_distance <
           rhs.classification.comparison.alpha_distance;
  }
  if (groups[lhs.left_index].exact_id != groups[rhs.left_index].exact_id) {
    return groups[lhs.left_index].exact_id < groups[rhs.left_index].exact_id;
  }
  return groups[lhs.right_index].exact_id < groups[rhs.right_index].exact_id;
}

void retain_top_pair(std::vector<SimilarityPairRecord>& retained,
                     SimilarityPairRecord candidate,
                     std::size_t limit,
                     const std::vector<SimilarityReportGroup>& groups) {
  if (limit == 0) {
    return;
  }
  if (retained.size() < limit) {
    retained.push_back(std::move(candidate));
    return;
  }

  std::size_t worst_index = 0;
  for (std::size_t index = 1; index < retained.size(); ++index) {
    if (pair_rank_better(retained[worst_index], retained[index], groups)) {
      worst_index = index;
    }
  }

  if (pair_rank_better(candidate, retained[worst_index], groups)) {
    retained[worst_index] = std::move(candidate);
  }
}

picojson::value to_json_similarity_group(const SimilarityReportGroup& group) {
  picojson::object object;
  object["exact_id"] = picojson::value(group.exact_id);
  object["occurrence_count"] = picojson::value(static_cast<double>(group.occurrence_count));
  object["fixtures"] = to_json_string_array(group.fixtures);
  object["source_atlas_identifiers"] = to_json_string_array(group.source_atlas_identifiers);
  object["source_images"] = to_json_string_array(group.source_images);
  object["representative_name"] = picojson::value(group.representative_name);
  object["representative_source_atlas_identifier"] =
      picojson::value(group.representative_source_atlas_identifier);
  object["representative_source_image"] = picojson::value(group.representative_source_image);
  object["representative_trimmed_image"] = picojson::value(group.representative_trimmed_image);
  object["representative_metadata_json"] = picojson::value(group.representative_metadata_json);

  picojson::object canonical_size;
  canonical_size["width"] = picojson::value(static_cast<double>(group.signature.canonical_width));
  canonical_size["height"] = picojson::value(static_cast<double>(group.signature.canonical_height));
  object["canonical_size"] = picojson::value(canonical_size);
  object["alpha_coverage"] = picojson::value(group.signature.alpha_coverage);
  return picojson::value(object);
}

picojson::value to_json_similarity_pair(const SimilarityPairRecord& pair,
                                        const std::vector<SimilarityReportGroup>& groups) {
  picojson::object object;
  object["classification"] =
      picojson::value(libatlas::similarity_candidate_kind_to_string(pair.classification.candidate_kind));
  object["score"] = picojson::value(pair.classification.comparison.score);
  object["luminance_distance"] =
      picojson::value(static_cast<double>(pair.classification.comparison.luminance_distance));
  object["alpha_distance"] =
      picojson::value(static_cast<double>(pair.classification.comparison.alpha_distance));
  object["dimension_ratio"] = picojson::value(pair.classification.comparison.dimension_ratio);
  object["aspect_ratio_delta"] = picojson::value(pair.classification.comparison.aspect_ratio_delta);
  object["alpha_coverage_delta"] = picojson::value(pair.classification.alpha_coverage_delta);
  object["left"] = to_json_similarity_group(groups[pair.left_index]);
  object["right"] = to_json_similarity_group(groups[pair.right_index]);
  return picojson::value(object);
}

picojson::value to_json_similarity_component(const std::vector<std::size_t>& component,
                                             const std::vector<SimilarityReportGroup>& groups) {
  picojson::object object;
  picojson::array exact_ids;
  picojson::array fixtures;
  picojson::array source_atlas_identifiers;
  picojson::array source_images;
  std::size_t occurrence_count = 0;

  std::vector<std::string> all_fixtures;
  std::vector<std::string> all_atlas_identifiers;
  std::vector<std::string> all_source_images;
  for (const auto group_index : component) {
    const auto& group = groups[group_index];
    exact_ids.emplace_back(group.exact_id);
    occurrence_count += group.occurrence_count;
    all_fixtures.insert(all_fixtures.end(), group.fixtures.begin(), group.fixtures.end());
    all_atlas_identifiers.insert(all_atlas_identifiers.end(),
                                 group.source_atlas_identifiers.begin(),
                                 group.source_atlas_identifiers.end());
    all_source_images.insert(all_source_images.end(), group.source_images.begin(), group.source_images.end());
  }

  for (const auto& value : sorted_unique(std::move(all_fixtures))) {
    fixtures.emplace_back(value);
  }
  for (const auto& value : sorted_unique(std::move(all_atlas_identifiers))) {
    source_atlas_identifiers.emplace_back(value);
  }
  for (const auto& value : sorted_unique(std::move(all_source_images))) {
    source_images.emplace_back(value);
  }

  object["member_exact_ids"] = picojson::value(exact_ids);
  object["member_exact_id_count"] = picojson::value(static_cast<double>(component.size()));
  object["member_occurrence_count"] = picojson::value(static_cast<double>(occurrence_count));
  object["fixtures"] = picojson::value(fixtures);
  object["source_atlas_identifiers"] = picojson::value(source_atlas_identifiers);
  object["source_images"] = picojson::value(source_images);
  return picojson::value(object);
}

ExtractCommand parse_extract_command(int argc, char** argv) {
  ExtractCommand command;
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--atlas" && index + 1 < argc) {
      command.atlas_path = argv[++index];
    } else if (argument == "--requests" && index + 1 < argc) {
      command.requests_path = argv[++index];
    } else if (argument == "--output-dir" && index + 1 < argc) {
      command.output_dir = argv[++index];
    } else if (argument == "--metadata" && index + 1 < argc) {
      command.metadata_path = argv[++index];
    } else if (argument == "--asset-store" && index + 1 < argc) {
      command.asset_store_dir = argv[++index];
    } else if (argument == "--origin" && index + 1 < argc) {
      command.origin = parse_origin(argv[++index]);
    } else if (argument == "--rounding" && index + 1 < argc) {
      command.rounding = parse_rounding(argv[++index]);
    } else if (argument == "--alpha-threshold" && index + 1 < argc) {
      command.alpha_threshold = static_cast<uint8_t>(std::stoi(argv[++index]));
    } else if (argument == "--no-trim") {
      command.trim = false;
    } else {
      throw std::runtime_error("unknown extract argument: " + argument);
    }
  }

  if (command.atlas_path.empty() || command.requests_path.empty() || command.output_dir.empty() ||
      command.metadata_path.empty()) {
    throw std::runtime_error("extract requires --atlas, --requests, --output-dir, and --metadata");
  }
  return command;
}

PackCommand parse_pack_command(int argc, char** argv) {
  PackCommand command;
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--manifest" && index + 1 < argc) {
      command.manifest_path = argv[++index];
    } else if (argument == "--output-dir" && index + 1 < argc) {
      command.output_dir = argv[++index];
    } else if (argument == "--metadata" && index + 1 < argc) {
      command.metadata_path = argv[++index];
    } else if (argument == "--atlas-prefix" && index + 1 < argc) {
      command.atlas_prefix = argv[++index];
    } else if (argument == "--max-width" && index + 1 < argc) {
      command.max_width = static_cast<uint32_t>(std::stoul(argv[++index]));
    } else if (argument == "--max-height" && index + 1 < argc) {
      command.max_height = static_cast<uint32_t>(std::stoul(argv[++index]));
    } else if (argument == "--padding" && index + 1 < argc) {
      command.padding = static_cast<uint32_t>(std::stoul(argv[++index]));
    } else if (argument == "--origin" && index + 1 < argc) {
      command.origin = parse_origin(argv[++index]);
    } else {
      throw std::runtime_error("unknown pack argument: " + argument);
    }
  }

  if (command.manifest_path.empty() || command.output_dir.empty() || command.metadata_path.empty()) {
    throw std::runtime_error("pack requires --manifest, --output-dir, and --metadata");
  }
  return command;
}

SimilarityReportCommand parse_similarity_report_command(int argc, char** argv) {
  SimilarityReportCommand command;
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--metadata-dir" && index + 1 < argc) {
      command.metadata_dir = argv[++index];
    } else if (argument == "--output" && index + 1 < argc) {
      command.output_path = argv[++index];
    } else if (argument == "--source-map" && index + 1 < argc) {
      command.source_map_path = argv[++index];
    } else if (argument == "--review-min-score" && index + 1 < argc) {
      command.classification_options.review_min_score = std::stod(argv[++index]);
    } else if (argument == "--auto-min-score" && index + 1 < argc) {
      command.classification_options.auto_min_score = std::stod(argv[++index]);
    } else if (argument == "--auto-max-luminance-distance" && index + 1 < argc) {
      command.classification_options.auto_max_luminance_distance = std::stoi(argv[++index]);
    } else if (argument == "--auto-max-alpha-distance" && index + 1 < argc) {
      command.classification_options.auto_max_alpha_distance = std::stoi(argv[++index]);
    } else if (argument == "--auto-max-aspect-ratio-delta" && index + 1 < argc) {
      command.classification_options.auto_max_aspect_ratio_delta = std::stod(argv[++index]);
    } else if (argument == "--auto-min-dimension-ratio" && index + 1 < argc) {
      command.classification_options.auto_min_dimension_ratio = std::stod(argv[++index]);
    } else if (argument == "--max-pairs" && index + 1 < argc) {
      command.max_pairs = static_cast<std::size_t>(std::stoul(argv[++index]));
    } else {
      throw std::runtime_error("unknown similarity-report argument: " + argument);
    }
  }

  if (command.metadata_dir.empty() || command.output_path.empty()) {
    throw std::runtime_error("similarity-report requires --metadata-dir and --output");
  }
  if (command.classification_options.review_min_score < 0.0 ||
      command.classification_options.review_min_score > 1.0) {
    throw std::runtime_error("--review-min-score must be between 0 and 1");
  }
  if (command.classification_options.auto_min_score < 0.0 ||
      command.classification_options.auto_min_score > 1.0) {
    throw std::runtime_error("--auto-min-score must be between 0 and 1");
  }
  if (command.classification_options.auto_min_score <
      command.classification_options.review_min_score) {
    throw std::runtime_error("--auto-min-score must be at least --review-min-score");
  }
  if (command.classification_options.auto_max_luminance_distance < 0) {
    throw std::runtime_error("--auto-max-luminance-distance must be at least 0");
  }
  if (command.classification_options.auto_max_alpha_distance < 0) {
    throw std::runtime_error("--auto-max-alpha-distance must be at least 0");
  }
  if (command.classification_options.auto_max_aspect_ratio_delta < 0.0) {
    throw std::runtime_error("--auto-max-aspect-ratio-delta must be at least 0");
  }
  if (command.classification_options.auto_min_dimension_ratio < 0.0 ||
      command.classification_options.auto_min_dimension_ratio > 1.0) {
    throw std::runtime_error("--auto-min-dimension-ratio must be between 0 and 1");
  }
  if (command.max_pairs < 1) {
    throw std::runtime_error("--max-pairs must be at least 1");
  }
  return command;
}

void run_extract(const ExtractCommand& command) {
  fs::create_directories(command.output_dir);

  auto atlas = libatlas::load_png(command.atlas_path.string());
  if (!atlas) {
    throw std::runtime_error(atlas.error().message);
  }

  std::optional<libatlas_tool::AssetStore> asset_store;
  libatlas::ExtractionIdentityCache identity_cache;
  if (!command.asset_store_dir.empty()) {
    asset_store.emplace(command.asset_store_dir);
    auto preload = asset_store->preload_cache(identity_cache);
    if (!preload) {
      throw std::runtime_error(preload.error().message);
    }
  }

  const picojson::object root =
      require_object(load_json_file(command.requests_path), "extract request root");
  const std::string atlas_identifier = optional_string(root, "atlas_identifier").empty()
                                           ? command.atlas_path.stem().string()
                                           : optional_string(root, "atlas_identifier");
  const picojson::array& items = require_array(require_field(root, "items", "extract request root"),
                                               "extract request root.items");

  picojson::array output_items;
  for (std::size_t index = 0; index < items.size(); ++index) {
    const std::string context = "extract request item[" + std::to_string(index) + "]";
    const picojson::object& item = require_object(items[index], context);
    const std::string name = require_string(item, "name", context);
    const picojson::object& uv = require_object(require_field(item, "uv", context), context + ".uv");

    libatlas::ExtractionOptions options;
    options.source_atlas_identifier = atlas_identifier;
    options.uv_origin = command.origin;
    options.rounding_policy = command.rounding;
    options.trim_transparent_borders = command.trim;
    options.transparent_alpha_threshold = command.alpha_threshold;

    const libatlas::UvRect uv_rect{
        require_number(uv, "x_min", context + ".uv"),
        require_number(uv, "x_max", context + ".uv"),
        require_number(uv, "y_min", context + ".uv"),
        require_number(uv, "y_max", context + ".uv"),
    };

    auto extracted = asset_store
                         ? libatlas::extract_texture_cached(atlas.value(), uv_rect, options, &identity_cache)
                         : libatlas::extract_texture(atlas.value(), uv_rect, options);
    if (!extracted) {
      throw std::runtime_error(extracted.error().message);
    }

    const std::string file_stem =
        std::to_string(index) + "_" + sanitize_name(name);
    const fs::path cropped_path = command.output_dir / (file_stem + "_cropped.png");
    const fs::path trimmed_path = command.output_dir / (file_stem + "_trimmed.png");

    picojson::object output_item;
    output_item["name"] = picojson::value(name);
    if (!extracted.value().cropped_image.empty()) {
      auto saved = libatlas::save_png(extracted.value().cropped_image, cropped_path.string());
      if (!saved) {
        throw std::runtime_error(saved.error().message);
      }
      output_item["cropped_image"] = picojson::value(cropped_path.generic_string());
    }
    if (!extracted.value().trimmed_image.empty()) {
      auto saved = libatlas::save_png(extracted.value().trimmed_image, trimmed_path.string());
      if (!saved) {
        throw std::runtime_error(saved.error().message);
      }
      output_item["trimmed_image"] = picojson::value(trimmed_path.generic_string());
    }
    output_item["metadata"] = to_json_metadata(extracted.value().metadata);
    if (asset_store) {
      auto stored = asset_store->record_occurrence(name,
                                                   command.atlas_path.string(),
                                                   atlas_identifier,
                                                   atlas.value(),
                                                   extracted.value());
      if (!stored) {
        throw std::runtime_error(stored.error().message);
      }
      output_item["asset_store"] = to_json_asset_store_paths(stored.value());
    }
    output_items.emplace_back(output_item);
  }

  picojson::object output_root;
  output_root["atlas_identifier"] = picojson::value(atlas_identifier);
  output_root["items"] = picojson::value(output_items);
  write_text_file(command.metadata_path, picojson::value(output_root).serialize(true));
}

void run_pack(const PackCommand& command) {
  fs::create_directories(command.output_dir);

  const picojson::object root = require_object(load_json_file(command.manifest_path), "pack manifest");
  const picojson::array& items =
      require_array(require_field(root, "items", "pack manifest"), "pack manifest.items");

  std::vector<libatlas::PackItem> pack_items;
  for (std::size_t index = 0; index < items.size(); ++index) {
    const std::string context = "pack manifest item[" + std::to_string(index) + "]";
    const picojson::object& item = require_object(items[index], context);

    const fs::path image_path = require_string(item, "image", context);
    auto image = libatlas::load_png(image_path.string());
    if (!image) {
      throw std::runtime_error(image.error().message);
    }

    pack_items.push_back(libatlas::PackItem{
        require_string(item, "entry_id", context),
        std::move(image.value()),
        optional_string(item, "source_label"),
    });
  }

  libatlas::AtlasPackOptions options;
  options.max_atlas_width = command.max_width;
  options.max_atlas_height = command.max_height;
  options.padding = command.padding;
  options.output_uv_origin = command.origin;
  options.atlas_identifier_prefix = command.atlas_prefix;

  auto packed = libatlas::pack_atlases(pack_items, options);
  if (!packed) {
    throw std::runtime_error(packed.error().message);
  }

  picojson::array atlases;
  for (const auto& atlas : packed.value().atlases) {
    const fs::path atlas_path = command.output_dir / (atlas.atlas_identifier + ".png");
    auto saved = libatlas::save_png(atlas.image, atlas_path.string());
    if (!saved) {
      throw std::runtime_error(saved.error().message);
    }

    picojson::object atlas_object;
    atlas_object["atlas_identifier"] = picojson::value(atlas.atlas_identifier);
    atlas_object["image"] = picojson::value(atlas_path.generic_string());
    atlas_object["width"] = picojson::value(static_cast<double>(atlas.image.width));
    atlas_object["height"] = picojson::value(static_cast<double>(atlas.image.height));
    atlases.emplace_back(atlas_object);
  }

  picojson::array placements;
  for (const auto& placement : packed.value().placements) {
    picojson::object placement_object;
    placement_object["entry_id"] = picojson::value(placement.entry_id);
    placement_object["source_label"] = picojson::value(placement.source_label);
    placement_object["atlas_index"] = picojson::value(static_cast<double>(placement.atlas_index));
    placement_object["pixel_rect"] = to_json_rect(placement.pixel_rect);
    placement_object["uv_rect"] = to_json_uv(placement.uv_rect);
    placements.emplace_back(placement_object);
  }

  picojson::object output_root;
  output_root["atlases"] = picojson::value(atlases);
  output_root["placements"] = picojson::value(placements);
  write_text_file(command.metadata_path, picojson::value(output_root).serialize(true));
}

void run_similarity_report(const SimilarityReportCommand& command) {
  std::vector<fs::path> metadata_files;
  for (const auto& entry : fs::directory_iterator(command.metadata_dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".json") {
      metadata_files.push_back(entry.path());
    }
  }
  std::sort(metadata_files.begin(), metadata_files.end());

  const auto source_map = load_source_map(command.source_map_path);
  std::map<std::string, SimilarityReportGroup> groups_by_exact_id;

  for (const auto& metadata_path : metadata_files) {
    const picojson::object root =
        require_object(load_json_file(metadata_path), "similarity report metadata root");
    const std::string default_atlas_identifier = metadata_path.stem().string();
    const std::string atlas_identifier = optional_string(root, "atlas_identifier").empty()
                                             ? default_atlas_identifier
                                             : optional_string(root, "atlas_identifier");
    const picojson::array& items = require_array(
        require_field(root, "items", "similarity report metadata root"),
        "similarity report metadata root.items");

    for (std::size_t index = 0; index < items.size(); ++index) {
      const std::string context =
          "similarity report metadata item[" + std::to_string(index) + "]";
      const picojson::object& item = require_object(items[index], context);
      const picojson::object& metadata =
          require_object(require_field(item, "metadata", context), context + ".metadata");
      const auto similarity_iterator = metadata.find("similarity_signature");
      if (similarity_iterator == metadata.end()) {
        continue;
      }

      const std::string exact_id = require_string(metadata, "exact_id", context + ".metadata");
      const std::string item_name = require_string(item, "name", context);
      const std::string source_atlas_identifier = optional_string(metadata, "source_atlas_identifier").empty()
                                                      ? atlas_identifier
                                                      : optional_string(metadata, "source_atlas_identifier");
      const std::string trimmed_image = optional_string(item, "trimmed_image");
      const auto source_iterator = source_map.find(source_atlas_identifier);
      const std::string source_image =
          source_iterator == source_map.end() ? std::string() : source_iterator->second;
      const libatlas::SimilaritySignature signature = parse_similarity(
          require_object(similarity_iterator->second, context + ".metadata.similarity_signature"),
          context + ".metadata.similarity_signature");

      auto [group_iterator, inserted] =
          groups_by_exact_id.emplace(exact_id, SimilarityReportGroup{});
      SimilarityReportGroup& group = group_iterator->second;
      if (inserted) {
        group.exact_id = exact_id;
        group.representative_name = item_name;
        group.representative_source_atlas_identifier = source_atlas_identifier;
        group.representative_source_image = source_image;
        group.representative_trimmed_image = trimmed_image;
        group.representative_metadata_json = metadata_path.generic_string();
        group.signature = signature;
      }

      ++group.occurrence_count;
      group.fixtures.push_back(item_name);
      group.source_atlas_identifiers.push_back(source_atlas_identifier);
      if (!source_image.empty()) {
        group.source_images.push_back(source_image);
      }
    }
  }

  std::vector<SimilarityReportGroup> groups;
  groups.reserve(groups_by_exact_id.size());
  for (auto& entry : groups_by_exact_id) {
    entry.second.fixtures = sorted_unique(std::move(entry.second.fixtures));
    entry.second.source_atlas_identifiers = sorted_unique(std::move(entry.second.source_atlas_identifiers));
    entry.second.source_images = sorted_unique(std::move(entry.second.source_images));
    groups.push_back(std::move(entry.second));
  }

  std::vector<SimilarityPairRecord> auto_pairs;
  std::vector<SimilarityPairRecord> review_pairs;
  std::size_t auto_pair_count = 0;
  std::size_t review_pair_count = 0;
  DisjointSet auto_components(groups.size());
  DisjointSet review_components(groups.size());

  for (std::size_t left_index = 0; left_index < groups.size(); ++left_index) {
    for (std::size_t right_index = left_index + 1; right_index < groups.size(); ++right_index) {
      libatlas::SimilarityClassification classification = libatlas::classify_similarity(
          groups[left_index].signature,
          groups[right_index].signature,
          command.classification_options);
      if (classification.candidate_kind == libatlas::SimilarityCandidateKind::None) {
        continue;
      }

      SimilarityPairRecord pair;
      pair.left_index = left_index;
      pair.right_index = right_index;
      pair.classification = classification;
      if (classification.candidate_kind == libatlas::SimilarityCandidateKind::AutoDuplicateCandidate) {
        auto_components.unite(left_index, right_index);
        ++auto_pair_count;
        retain_top_pair(auto_pairs, std::move(pair), command.max_pairs, groups);
      } else {
        review_components.unite(left_index, right_index);
        ++review_pair_count;
        retain_top_pair(review_pairs, std::move(pair), command.max_pairs, groups);
      }
    }
  }

  std::sort(auto_pairs.begin(),
            auto_pairs.end(),
            [&groups](const SimilarityPairRecord& lhs, const SimilarityPairRecord& rhs) {
              return pair_rank_better(lhs, rhs, groups);
            });
  std::sort(review_pairs.begin(),
            review_pairs.end(),
            [&groups](const SimilarityPairRecord& lhs, const SimilarityPairRecord& rhs) {
              return pair_rank_better(lhs, rhs, groups);
            });

  picojson::array auto_pairs_json;
  for (const auto& pair : auto_pairs) {
    auto_pairs_json.emplace_back(to_json_similarity_pair(pair, groups));
  }
  picojson::array review_pairs_json;
  for (const auto& pair : review_pairs) {
    review_pairs_json.emplace_back(to_json_similarity_pair(pair, groups));
  }

  std::map<std::size_t, std::vector<std::size_t>> component_indices;
  for (std::size_t index = 0; index < groups.size(); ++index) {
    component_indices[auto_components.find(index)].push_back(index);
  }

  std::vector<std::vector<std::size_t>> auto_duplicate_components;
  for (auto& entry : component_indices) {
    if (entry.second.size() <= 1) {
      continue;
    }
    std::sort(
        entry.second.begin(),
        entry.second.end(),
        [&groups](std::size_t lhs, std::size_t rhs) { return groups[lhs].exact_id < groups[rhs].exact_id; });
    auto_duplicate_components.push_back(std::move(entry.second));
  }
  std::sort(auto_duplicate_components.begin(),
            auto_duplicate_components.end(),
            [&groups](const std::vector<std::size_t>& lhs, const std::vector<std::size_t>& rhs) {
              return groups[lhs.front()].exact_id < groups[rhs.front()].exact_id;
            });

  picojson::array auto_components_json;
  std::size_t merged_exact_id_count = 0;
  for (const auto& component : auto_duplicate_components) {
    merged_exact_id_count += component.size();
    auto_components_json.emplace_back(to_json_similarity_component(component, groups));
  }

  std::map<std::size_t, std::vector<std::size_t>> review_component_indices;
  for (std::size_t index = 0; index < groups.size(); ++index) {
    review_component_indices[review_components.find(index)].push_back(index);
  }

  std::vector<std::vector<std::size_t>> review_duplicate_components;
  for (auto& entry : review_component_indices) {
    if (entry.second.size() <= 1) {
      continue;
    }
    std::sort(
        entry.second.begin(),
        entry.second.end(),
        [&groups](std::size_t lhs, std::size_t rhs) { return groups[lhs].exact_id < groups[rhs].exact_id; });
    review_duplicate_components.push_back(std::move(entry.second));
  }
  std::sort(review_duplicate_components.begin(),
            review_duplicate_components.end(),
            [&groups](const std::vector<std::size_t>& lhs, const std::vector<std::size_t>& rhs) {
              return groups[lhs.front()].exact_id < groups[rhs.front()].exact_id;
            });

  picojson::array review_components_json;
  std::size_t review_component_exact_id_count = 0;
  for (const auto& component : review_duplicate_components) {
    review_component_exact_id_count += component.size();
    review_components_json.emplace_back(to_json_similarity_component(component, groups));
  }

  picojson::object library_defaults;
  library_defaults["max_luminance_distance"] = picojson::value(
      static_cast<double>(command.classification_options.similarity_options.max_luminance_distance));
  library_defaults["max_alpha_distance"] = picojson::value(
      static_cast<double>(command.classification_options.similarity_options.max_alpha_distance));
  library_defaults["max_aspect_ratio_delta"] = picojson::value(
      command.classification_options.similarity_options.max_aspect_ratio_delta);
  library_defaults["min_dimension_ratio"] = picojson::value(
      command.classification_options.similarity_options.min_dimension_ratio);

  picojson::object report_buckets;
  report_buckets["review_min_score"] =
      picojson::value(command.classification_options.review_min_score);
  report_buckets["auto_min_score"] =
      picojson::value(command.classification_options.auto_min_score);
  report_buckets["auto_max_luminance_distance"] = picojson::value(
      static_cast<double>(command.classification_options.auto_max_luminance_distance));
  report_buckets["auto_max_alpha_distance"] = picojson::value(
      static_cast<double>(command.classification_options.auto_max_alpha_distance));
  report_buckets["auto_max_aspect_ratio_delta"] = picojson::value(
      command.classification_options.auto_max_aspect_ratio_delta);
  report_buckets["auto_min_dimension_ratio"] = picojson::value(
      command.classification_options.auto_min_dimension_ratio);
  report_buckets["max_pairs"] = picojson::value(static_cast<double>(command.max_pairs));

  picojson::object thresholds;
  thresholds["library_defaults"] = picojson::value(library_defaults);
  thresholds["report_buckets"] = picojson::value(report_buckets);

  picojson::object output_root;
  output_root["comparison_method"] = picojson::value("libatlas similarity signature");
  output_root["pair_search_strategy"] = picojson::value("all_pairs_by_exact_id_group");
  output_root["thresholds"] = picojson::value(thresholds);
  output_root["exact_id_group_count"] = picojson::value(static_cast<double>(groups.size()));
  output_root["auto_duplicate_candidate_count"] = picojson::value(static_cast<double>(auto_pair_count));
  output_root["review_candidate_count"] = picojson::value(static_cast<double>(review_pair_count));
  output_root["auto_duplicate_candidate_omitted_count"] = picojson::value(
      static_cast<double>(auto_pair_count > auto_pairs.size() ? auto_pair_count - auto_pairs.size() : 0));
  output_root["review_candidate_omitted_count"] = picojson::value(
      static_cast<double>(review_pair_count > review_pairs.size() ? review_pair_count - review_pairs.size() : 0));
  output_root["auto_duplicate_component_count"] =
      picojson::value(static_cast<double>(auto_duplicate_components.size()));
  output_root["auto_duplicate_component_exact_id_count"] =
      picojson::value(static_cast<double>(merged_exact_id_count));
  output_root["review_component_count"] =
      picojson::value(static_cast<double>(review_duplicate_components.size()));
  output_root["review_component_exact_id_count"] =
      picojson::value(static_cast<double>(review_component_exact_id_count));
  output_root["auto_duplicate_components"] = picojson::value(auto_components_json);
  output_root["review_components"] = picojson::value(review_components_json);
  output_root["auto_duplicate_candidates"] = picojson::value(auto_pairs_json);
  output_root["review_candidates"] = picojson::value(review_pairs_json);

  if (!command.output_path.parent_path().empty()) {
    fs::create_directories(command.output_path.parent_path());
  }
  write_text_file(command.output_path, picojson::value(output_root).serialize(true));
}

void print_usage() {
  std::cout
      << "libatlas_tool extract --atlas atlas.png --requests requests.json --output-dir out "
         "--metadata metadata.json [--asset-store DIR] [--origin top-left|bottom-left] "
         "[--rounding expand|nearest|contract] [--no-trim] [--alpha-threshold N]\n"
      << "libatlas_tool pack --manifest pack.json --output-dir out --metadata metadata.json "
         "[--atlas-prefix atlas] [--max-width N] [--max-height N] [--padding N] "
         "[--origin top-left|bottom-left]\n"
      << "libatlas_tool similarity-report --metadata-dir dir --output report.json "
         "[--source-map source_map.json] [--review-min-score X] [--auto-min-score X] "
         "[--auto-max-luminance-distance N] [--auto-max-alpha-distance N] "
         "[--auto-max-aspect-ratio-delta X] [--auto-min-dimension-ratio X] [--max-pairs N]\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      print_usage();
      return 1;
    }

    const std::string command = argv[1];
    if (command == "extract") {
      run_extract(parse_extract_command(argc, argv));
      return 0;
    }
    if (command == "pack") {
      run_pack(parse_pack_command(argc, argv));
      return 0;
    }
    if (command == "similarity-report") {
      run_similarity_report(parse_similarity_report_command(argc, argv));
      return 0;
    }
    if (command == "--help" || command == "-h") {
      print_usage();
      return 0;
    }

    throw std::runtime_error("unknown command: " + command);
  } catch (const std::exception& exception) {
    std::cerr << "error: " << exception.what() << "\n";
    return 1;
  }
}
