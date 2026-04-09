#include "asset_store.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <system_error>

#include "picojson.h"

namespace fs = std::filesystem;

namespace libatlas_tool {

namespace {

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

std::string make_id_stem(const libatlas::CanonicalTextureId& id) {
  return std::string("sha256_v1_") + id.hex();
}

std::string path_to_json_string(const fs::path& path) {
  return path.empty() ? std::string() : path.generic_string();
}

bool path_exists(const fs::path& path) {
  std::error_code error;
  return fs::exists(path, error);
}

void remove_file_if_present(const fs::path& path) noexcept {
  std::error_code error;
  fs::remove(path, error);
}

fs::path make_temp_path(const fs::path& path) {
  static std::atomic<uint64_t> counter{0};
  const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const auto ordinal = counter.fetch_add(1, std::memory_order_relaxed);
  return fs::path(path.string() + ".tmp-" + std::to_string(stamp) + "-" + std::to_string(ordinal));
}

void write_text_file(const fs::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("failed to write file: " + path.string());
  }
  output << text;
}

void promote_temp_file(const fs::path& temp_path, const fs::path& final_path) {
  std::error_code last_error;
  for (int attempt = 1; attempt <= 8; ++attempt) {
    std::error_code error;
    fs::rename(temp_path, final_path, error);
    if (!error) {
      return;
    }
    if (path_exists(final_path)) {
      remove_file_if_present(temp_path);
      return;
    }

    last_error = error;
    if (attempt < 8) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25 * attempt));
    }
  }

  remove_file_if_present(temp_path);
  if (path_exists(final_path)) {
    return;
  }

  throw std::system_error(last_error,
                          "failed to move temporary file into place for " + final_path.string());
}

// The fixture pipeline can run multiple extract workers against one shared asset store.
// Write content-addressed files through temp paths so readers never observe partial JSON or PNG bytes.
void write_text_file_if_absent(const fs::path& path, const std::string& text) {
  if (path_exists(path)) {
    return;
  }

  const fs::path temp_path = make_temp_path(path);
  try {
    write_text_file(temp_path, text);
    promote_temp_file(temp_path, path);
  } catch (...) {
    remove_file_if_present(temp_path);
    throw;
  }
}

void save_png_if_absent(const fs::path& path, const libatlas::Image& image) {
  if (path.empty() || path_exists(path)) {
    return;
  }

  const fs::path temp_path = make_temp_path(path);
  try {
    auto saved = libatlas::save_png(image, temp_path.string());
    if (!saved) {
      throw std::runtime_error(saved.error().message);
    }
    promote_temp_file(temp_path, path);
  } catch (...) {
    remove_file_if_present(temp_path);
    throw;
  }
}

std::string read_text_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open file: " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
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

bool require_bool(const picojson::object& object,
                  const std::string& key,
                  const std::string& context) {
  const auto& value = require_field(object, key, context);
  if (!value.is<bool>()) {
    throw std::runtime_error(context + "." + key + " must be a boolean");
  }
  return value.get<bool>();
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

libatlas::PixelRect parse_rect(const picojson::object& object, const std::string& context) {
  return libatlas::PixelRect{
      static_cast<int32_t>(require_number(object, "x", context)),
      static_cast<int32_t>(require_number(object, "y", context)),
      static_cast<int32_t>(require_number(object, "width", context)),
      static_cast<int32_t>(require_number(object, "height", context)),
  };
}

libatlas::SimilaritySignature parse_similarity(const picojson::object& object,
                                               const std::string& context) {
  libatlas::SimilaritySignature signature;
  signature.canonical_width = static_cast<uint32_t>(require_number(object, "canonical_width", context));
  signature.canonical_height =
      static_cast<uint32_t>(require_number(object, "canonical_height", context));
  signature.alpha_coverage = require_number(object, "alpha_coverage", context);

  const auto luminance = require_string(object, "luminance_hash", context);
  const auto alpha = require_string(object, "alpha_hash", context);
  signature.luminance_hash = static_cast<uint64_t>(std::stoull(luminance));
  signature.alpha_hash = static_cast<uint64_t>(std::stoull(alpha));
  return signature;
}

fs::path make_relative_path(const fs::path& root, const fs::path& path) {
  std::error_code error;
  const fs::path relative = fs::relative(path, root, error);
  return error ? path : relative;
}

libatlas::Result<libatlas::Image> load_cached_resolved_image(const fs::path& root,
                                                             const picojson::object& object,
                                                             const std::string& context) {
  const std::string image_path_string = require_string(object, "canonical_image", context);
  const uint32_t width = static_cast<uint32_t>(require_number(object, "resolved_width", context));
  const uint32_t height = static_cast<uint32_t>(require_number(object, "resolved_height", context));

  if (image_path_string.empty()) {
    auto image = libatlas::make_image(width, height, libatlas::PixelFormat::RGBA8);
    if (!image) {
      return libatlas::Result<libatlas::Image>::failure(image.error().code, image.error().message);
    }
    return libatlas::Result<libatlas::Image>::success(std::move(image.value()));
  }

  return libatlas::load_png((root / fs::path(image_path_string)).string());
}

}  // namespace

AssetStore::AssetStore(fs::path root) : root_(std::move(root)) {}

void AssetStore::initialize() const {
  fs::create_directories(root_ / "atlases");
  fs::create_directories(root_ / "cropped");
  fs::create_directories(root_ / "canonical");
  fs::create_directories(root_ / "metadata" / "atlases");
  fs::create_directories(root_ / "metadata" / "cropped");
  fs::create_directories(root_ / "metadata" / "canonical");
  fs::create_directories(root_ / "metadata" / "occurrences");
}

libatlas::Result<void> AssetStore::preload_cache(libatlas::ExtractionIdentityCache& cache) const {
  initialize();

  const fs::path cropped_metadata_dir = root_ / "metadata" / "cropped";
  for (const auto& entry : fs::directory_iterator(cropped_metadata_dir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json") {
      continue;
    }

    try {
      const picojson::object object =
          require_object(load_json_file(entry.path()), "cropped metadata");
      auto cropped_exact_id =
          libatlas::parse_canonical_texture_id(require_string(object, "cropped_exact_id", "cropped metadata"));
      auto exact_id =
          libatlas::parse_canonical_texture_id(require_string(object, "exact_id", "cropped metadata"));
      if (!cropped_exact_id) {
        return libatlas::Result<void>::failure(cropped_exact_id.error().code,
                                               cropped_exact_id.error().message);
      }
      if (!exact_id) {
        return libatlas::Result<void>::failure(exact_id.error().code, exact_id.error().message);
      }

      auto resolved_image = load_cached_resolved_image(root_, object, "cropped metadata");
      if (!resolved_image) {
        return libatlas::Result<void>::failure(resolved_image.error().code,
                                               resolved_image.error().message);
      }

      const picojson::object& trimmed_rect =
          require_object(require_field(object, "trimmed_rect_in_crop", "cropped metadata"),
                         "cropped metadata.trimmed_rect_in_crop");
      libatlas::SimilaritySignature similarity_signature;
      const auto similarity_iterator = object.find("similarity_signature");
      const bool has_similarity_signature = similarity_iterator != object.end();
      if (has_similarity_signature) {
        similarity_signature = parse_similarity(
            require_object(similarity_iterator->second, "cropped metadata.similarity_signature"),
            "cropped metadata.similarity_signature");
      }

      auto add_result = cache.add_cached_identity(cropped_exact_id.value(),
                                                  exact_id.value(),
                                                  resolved_image.value(),
                                                  parse_rect(trimmed_rect, "cropped metadata.trimmed_rect_in_crop"),
                                                  require_bool(object, "trim_transparent_borders", "cropped metadata"),
                                                  static_cast<uint8_t>(
                                                      require_number(object, "transparent_alpha_threshold", "cropped metadata")),
                                                  has_similarity_signature ? &similarity_signature : nullptr);
      if (!add_result) {
        return add_result;
      }
    } catch (const std::exception& exception) {
      return libatlas::Result<void>::failure(libatlas::ErrorCode::ParseError, exception.what());
    }
  }

  return libatlas::Result<void>::success();
}

libatlas::Result<StoredAssetPaths> AssetStore::record_occurrence(const std::string& occurrence_name,
                                                                 const std::string& atlas_source_path,
                                                                 const std::string& atlas_identifier,
                                                                 const libatlas::Image& atlas_image,
                                                                 const libatlas::ExtractedTexture& extracted) const {
  try {
    initialize();

    libatlas::CanonicalizationOptions atlas_hash_options;
    atlas_hash_options.trim_transparent_borders = false;
    auto atlas_id = libatlas::compute_canonical_texture_id(atlas_image, atlas_hash_options);
    if (!atlas_id) {
      return libatlas::Result<StoredAssetPaths>::failure(atlas_id.error().code,
                                                         atlas_id.error().message);
    }

    const std::string atlas_stem = make_id_stem(atlas_id.value());
    const std::string cropped_stem = make_id_stem(extracted.metadata.cropped_exact_id);
    const std::string canonical_stem = make_id_stem(extracted.metadata.exact_id);

    StoredAssetPaths stored;
    stored.atlas_image_path = root_ / "atlases" / (atlas_stem + ".png");
    stored.atlas_metadata_path = root_ / "metadata" / "atlases" / (atlas_stem + ".json");
    stored.cropped_image_path = extracted.cropped_image.empty()
                                    ? fs::path()
                                    : root_ / "cropped" / (cropped_stem + ".png");
    stored.cropped_metadata_path = root_ / "metadata" / "cropped" / (cropped_stem + ".json");
    stored.canonical_image_path = extracted.trimmed_image.empty()
                                      ? fs::path()
                                      : root_ / "canonical" / (canonical_stem + ".png");
    stored.canonical_metadata_path = root_ / "metadata" / "canonical" / (canonical_stem + ".json");
    stored.occurrence_metadata_path =
        root_ / "metadata" / "occurrences" /
        (sanitize_name(atlas_identifier) + "__" + sanitize_name(occurrence_name) + "__" +
         cropped_stem.substr(0, std::min<std::size_t>(cropped_stem.size(), 24U)) + ".json");

    stored.atlas_existed = path_exists(stored.atlas_image_path);
    stored.cropped_existed =
        stored.cropped_image_path.empty() ? false : path_exists(stored.cropped_image_path);
    stored.canonical_existed =
        stored.canonical_image_path.empty() ? false : path_exists(stored.canonical_image_path);

    if (!stored.atlas_existed) {
      save_png_if_absent(stored.atlas_image_path, atlas_image);

      picojson::object atlas_object;
      atlas_object["atlas_id"] = picojson::value(atlas_id.value().to_string());
      atlas_object["atlas_identifier"] = picojson::value(atlas_identifier);
      atlas_object["source_path"] = picojson::value(atlas_source_path);
      atlas_object["image"] =
          picojson::value(path_to_json_string(make_relative_path(root_, stored.atlas_image_path)));
      atlas_object["width"] = picojson::value(static_cast<double>(atlas_image.width));
      atlas_object["height"] = picojson::value(static_cast<double>(atlas_image.height));
      write_text_file_if_absent(stored.atlas_metadata_path,
                                picojson::value(atlas_object).serialize(true));
    }

    if (!stored.cropped_image_path.empty() && !stored.cropped_existed) {
      save_png_if_absent(stored.cropped_image_path, extracted.cropped_image);
    }

    if (!stored.canonical_image_path.empty() && !stored.canonical_existed) {
      save_png_if_absent(stored.canonical_image_path, extracted.trimmed_image);
    }

    if (!path_exists(stored.cropped_metadata_path)) {
      picojson::object cropped_object;
      cropped_object["cropped_exact_id"] = picojson::value(extracted.metadata.cropped_exact_id.to_string());
      cropped_object["exact_id"] = picojson::value(extracted.metadata.exact_id.to_string());
      cropped_object["trim_transparent_borders"] =
          picojson::value(extracted.metadata.trim_transparent_borders_applied);
      cropped_object["transparent_alpha_threshold"] =
          picojson::value(static_cast<double>(extracted.metadata.transparent_alpha_threshold));
      cropped_object["trimmed_rect_in_crop"] = to_json_rect(extracted.metadata.trimmed_rect_in_crop);
      cropped_object["cropped_width"] = picojson::value(static_cast<double>(extracted.metadata.cropped_width));
      cropped_object["cropped_height"] = picojson::value(static_cast<double>(extracted.metadata.cropped_height));
      cropped_object["resolved_width"] = picojson::value(static_cast<double>(extracted.metadata.trimmed_width));
      cropped_object["resolved_height"] = picojson::value(static_cast<double>(extracted.metadata.trimmed_height));
      cropped_object["cropped_image"] =
          picojson::value(path_to_json_string(make_relative_path(root_, stored.cropped_image_path)));
      cropped_object["canonical_image"] =
          picojson::value(path_to_json_string(make_relative_path(root_, stored.canonical_image_path)));
      if (extracted.metadata.has_similarity_signature) {
        cropped_object["similarity_signature"] = to_json_similarity(extracted.metadata.similarity_signature);
      }
      write_text_file_if_absent(stored.cropped_metadata_path,
                                picojson::value(cropped_object).serialize(true));
    }

    if (!path_exists(stored.canonical_metadata_path)) {
      picojson::object canonical_object;
      canonical_object["exact_id"] = picojson::value(extracted.metadata.exact_id.to_string());
      canonical_object["canonical_image"] =
          picojson::value(path_to_json_string(make_relative_path(root_, stored.canonical_image_path)));
      canonical_object["width"] = picojson::value(static_cast<double>(extracted.metadata.trimmed_width));
      canonical_object["height"] = picojson::value(static_cast<double>(extracted.metadata.trimmed_height));
      canonical_object["representative_source_atlas_identifier"] =
          picojson::value(extracted.metadata.source_atlas_identifier);
      canonical_object["representative_cropped_exact_id"] =
          picojson::value(extracted.metadata.cropped_exact_id.to_string());
      if (extracted.metadata.has_similarity_signature) {
        canonical_object["similarity_signature"] = to_json_similarity(extracted.metadata.similarity_signature);
      }
      write_text_file_if_absent(stored.canonical_metadata_path,
                                picojson::value(canonical_object).serialize(true));
    }

    picojson::object occurrence_object;
    occurrence_object["occurrence_id"] = picojson::value(occurrence_name);
    occurrence_object["atlas_identifier"] = picojson::value(atlas_identifier);
    occurrence_object["atlas_id"] = picojson::value(atlas_id.value().to_string());
    occurrence_object["atlas_source_path"] = picojson::value(atlas_source_path);
    occurrence_object["cropped_exact_id"] = picojson::value(extracted.metadata.cropped_exact_id.to_string());
    occurrence_object["exact_id"] = picojson::value(extracted.metadata.exact_id.to_string());
    occurrence_object["requested_uv_rect"] = to_json_uv(extracted.metadata.requested_uv_rect);
    occurrence_object["resolved_pixel_rect"] = to_json_rect(extracted.metadata.resolved_pixel_rect);
    occurrence_object["clamped_pixel_rect"] = to_json_rect(extracted.metadata.clamped_pixel_rect);
    occurrence_object["trimmed_pixel_rect"] = to_json_rect(extracted.metadata.trimmed_pixel_rect);
    occurrence_object["trimmed_rect_in_crop"] = to_json_rect(extracted.metadata.trimmed_rect_in_crop);
    occurrence_object["cropped_width"] = picojson::value(static_cast<double>(extracted.metadata.cropped_width));
    occurrence_object["cropped_height"] = picojson::value(static_cast<double>(extracted.metadata.cropped_height));
    occurrence_object["trimmed_width"] = picojson::value(static_cast<double>(extracted.metadata.trimmed_width));
    occurrence_object["trimmed_height"] = picojson::value(static_cast<double>(extracted.metadata.trimmed_height));
    occurrence_object["cropped_alpha_coverage"] = picojson::value(extracted.metadata.cropped_alpha_coverage);
    occurrence_object["trimmed_alpha_coverage"] = picojson::value(extracted.metadata.trimmed_alpha_coverage);
    occurrence_object["cache_outcome"] =
        picojson::value(libatlas::identity_cache_outcome_to_string(extracted.metadata.cache_outcome));
    occurrence_object["warnings"] = picojson::value(picojson::array{});
    occurrence_object["atlas_image"] =
        picojson::value(path_to_json_string(make_relative_path(root_, stored.atlas_image_path)));
    occurrence_object["cropped_image"] =
        picojson::value(path_to_json_string(make_relative_path(root_, stored.cropped_image_path)));
    occurrence_object["canonical_image"] =
        picojson::value(path_to_json_string(make_relative_path(root_, stored.canonical_image_path)));
    occurrence_object["cropped_metadata"] =
        picojson::value(path_to_json_string(make_relative_path(root_, stored.cropped_metadata_path)));
    occurrence_object["canonical_metadata"] =
        picojson::value(path_to_json_string(make_relative_path(root_, stored.canonical_metadata_path)));

    picojson::array warnings;
    for (const auto warning : extracted.metadata.warnings) {
      warnings.emplace_back(libatlas::extraction_warning_to_string(warning));
    }
    occurrence_object["warnings"] = picojson::value(warnings);
    write_text_file(stored.occurrence_metadata_path, picojson::value(occurrence_object).serialize(true));

    return libatlas::Result<StoredAssetPaths>::success(std::move(stored));
  } catch (const std::exception& exception) {
    return libatlas::Result<StoredAssetPaths>::failure(libatlas::ErrorCode::IoError, exception.what());
  }
}

const fs::path& AssetStore::root() const noexcept { return root_; }

}  // namespace libatlas_tool
