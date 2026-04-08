#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "picojson.h"

#include "libatlas/libatlas.hpp"

namespace fs = std::filesystem;

namespace {

struct ExtractCommand {
  fs::path atlas_path;
  fs::path requests_path;
  fs::path output_dir;
  fs::path metadata_path;
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

picojson::value to_json_metadata(const libatlas::ExtractionMetadata& metadata) {
  picojson::object object;
  object["source_atlas_identifier"] = picojson::value(metadata.source_atlas_identifier);
  object["source_atlas_width"] = picojson::value(static_cast<double>(metadata.source_atlas_width));
  object["source_atlas_height"] = picojson::value(static_cast<double>(metadata.source_atlas_height));
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
  object["exact_id"] = picojson::value(metadata.exact_id.to_string());

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

void run_extract(const ExtractCommand& command) {
  fs::create_directories(command.output_dir);

  auto atlas = libatlas::load_png(command.atlas_path.string());
  if (!atlas) {
    throw std::runtime_error(atlas.error().message);
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

    auto extracted = libatlas::extract_texture(
        atlas.value(),
        libatlas::UvRect{
            require_number(uv, "x_min", context + ".uv"),
            require_number(uv, "x_max", context + ".uv"),
            require_number(uv, "y_min", context + ".uv"),
            require_number(uv, "y_max", context + ".uv"),
        },
        options);
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

void print_usage() {
  std::cout
      << "libatlas_tool extract --atlas atlas.png --requests requests.json --output-dir out "
         "--metadata metadata.json [--origin top-left|bottom-left] [--rounding expand|nearest|contract] "
         "[--no-trim] [--alpha-threshold N]\n"
      << "libatlas_tool pack --manifest pack.json --output-dir out --metadata metadata.json "
         "[--atlas-prefix atlas] [--max-width N] [--max-height N] [--padding N] "
         "[--origin top-left|bottom-left]\n";
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
