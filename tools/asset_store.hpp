#pragma once

#include <filesystem>
#include <string>

#include "libatlas/libatlas.hpp"

namespace libatlas_tool {

struct StoredAssetPaths {
  std::filesystem::path atlas_image_path;
  std::filesystem::path atlas_metadata_path;
  std::filesystem::path cropped_image_path;
  std::filesystem::path cropped_metadata_path;
  std::filesystem::path canonical_image_path;
  std::filesystem::path canonical_metadata_path;
  std::filesystem::path occurrence_metadata_path;
  bool atlas_existed = false;
  bool cropped_existed = false;
  bool canonical_existed = false;
};

class AssetStore {
 public:
  explicit AssetStore(std::filesystem::path root);

  void initialize() const;
  libatlas::Result<void> preload_cache(libatlas::ExtractionIdentityCache& cache) const;
  libatlas::Result<StoredAssetPaths> record_occurrence(const std::string& occurrence_name,
                                                       const std::string& atlas_source_path,
                                                       const std::string& atlas_identifier,
                                                       const libatlas::Image& atlas_image,
                                                       const libatlas::ExtractedTexture& extracted) const;

  const std::filesystem::path& root() const noexcept;

 private:
  std::filesystem::path root_;
};

}  // namespace libatlas_tool
