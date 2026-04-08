#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "libatlas/image.hpp"

namespace libatlas {

struct CanonicalizationOptions {
  bool trim_transparent_borders = true;
  uint8_t transparent_alpha_threshold = 0;
};

struct CanonicalizedImage {
  Image image;
  PixelRect retained_rect;
};

struct CanonicalTextureId {
  static constexpr std::size_t kDigestSize = 32;

  std::array<uint8_t, kDigestSize> bytes{};

  bool empty() const noexcept;
  std::string hex() const;
  std::string to_string() const;

  bool operator==(const CanonicalTextureId& other) const noexcept;
  bool operator!=(const CanonicalTextureId& other) const noexcept;
};

Result<CanonicalizedImage> canonicalize_image(const Image& image,
                                              const CanonicalizationOptions& options = {});
Result<CanonicalTextureId> compute_canonical_texture_id(
    const Image& image,
    const CanonicalizationOptions& options = {});

}  // namespace libatlas
