#include "libatlas/identity.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace libatlas {

namespace {

constexpr uint32_t kSha256RoundConstants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

uint32_t rotate_right(uint32_t value, uint32_t shift) {
  return (value >> shift) | (value << (32U - shift));
}

class Sha256 {
 public:
  void update(const uint8_t* data, std::size_t size) {
    total_bits_ += static_cast<uint64_t>(size) * 8ULL;

    std::size_t offset = 0;
    while (offset < size) {
      const std::size_t to_copy = std::min<std::size_t>(kBlockSize - buffer_size_, size - offset);
      std::memcpy(buffer_.data() + buffer_size_, data + offset, to_copy);
      buffer_size_ += to_copy;
      offset += to_copy;
      if (buffer_size_ == kBlockSize) {
        process_block(buffer_.data());
        buffer_size_ = 0;
      }
    }
  }

  std::array<uint8_t, 32> finalize() {
    buffer_[buffer_size_++] = 0x80U;

    if (buffer_size_ > 56U) {
      while (buffer_size_ < kBlockSize) {
        buffer_[buffer_size_++] = 0;
      }
      process_block(buffer_.data());
      buffer_size_ = 0;
    }

    while (buffer_size_ < 56U) {
      buffer_[buffer_size_++] = 0;
    }

    for (int i = 7; i >= 0; --i) {
      buffer_[buffer_size_++] = static_cast<uint8_t>((total_bits_ >> (i * 8U)) & 0xffU);
    }
    process_block(buffer_.data());

    std::array<uint8_t, 32> digest{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
      digest[i * 4U + 0U] = static_cast<uint8_t>((state_[i] >> 24U) & 0xffU);
      digest[i * 4U + 1U] = static_cast<uint8_t>((state_[i] >> 16U) & 0xffU);
      digest[i * 4U + 2U] = static_cast<uint8_t>((state_[i] >> 8U) & 0xffU);
      digest[i * 4U + 3U] = static_cast<uint8_t>(state_[i] & 0xffU);
    }
    return digest;
  }

 private:
  static constexpr std::size_t kBlockSize = 64U;

  void process_block(const uint8_t* block) {
    uint32_t message_schedule[64];
    for (std::size_t i = 0; i < 16U; ++i) {
      const std::size_t base = i * 4U;
      message_schedule[i] = (static_cast<uint32_t>(block[base + 0U]) << 24U) |
                            (static_cast<uint32_t>(block[base + 1U]) << 16U) |
                            (static_cast<uint32_t>(block[base + 2U]) << 8U) |
                            static_cast<uint32_t>(block[base + 3U]);
    }
    for (std::size_t i = 16U; i < 64U; ++i) {
      const uint32_t s0 = rotate_right(message_schedule[i - 15U], 7U) ^
                          rotate_right(message_schedule[i - 15U], 18U) ^
                          (message_schedule[i - 15U] >> 3U);
      const uint32_t s1 = rotate_right(message_schedule[i - 2U], 17U) ^
                          rotate_right(message_schedule[i - 2U], 19U) ^
                          (message_schedule[i - 2U] >> 10U);
      message_schedule[i] = message_schedule[i - 16U] + s0 + message_schedule[i - 7U] + s1;
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];

    for (std::size_t i = 0; i < 64U; ++i) {
      const uint32_t s1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
      const uint32_t choose = (e & f) ^ ((~e) & g);
      const uint32_t temp1 = h + s1 + choose + kSha256RoundConstants[i] + message_schedule[i];
      const uint32_t s0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
      const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t temp2 = s0 + majority;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<uint8_t, kBlockSize> buffer_{};
  std::size_t buffer_size_ = 0;
  uint64_t total_bits_ = 0;
  std::array<uint32_t, 8> state_{
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
};

void append_u32_le(std::vector<uint8_t>& bytes, uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value & 0xffU));
  bytes.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
  bytes.push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
  bytes.push_back(static_cast<uint8_t>((value >> 24U) & 0xffU));
}

}  // namespace

bool CanonicalTextureId::empty() const noexcept {
  return std::all_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte == 0; });
}

std::string CanonicalTextureId::hex() const {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const uint8_t byte : bytes) {
    stream << std::setw(2) << static_cast<int>(byte);
  }
  return stream.str();
}

std::string CanonicalTextureId::to_string() const { return "sha256:v1:" + hex(); }

bool CanonicalTextureId::operator==(const CanonicalTextureId& other) const noexcept {
  return bytes == other.bytes;
}

bool CanonicalTextureId::operator!=(const CanonicalTextureId& other) const noexcept {
  return !(*this == other);
}

Result<CanonicalizedImage> canonicalize_image(const Image& image,
                                              const CanonicalizationOptions& options) {
  const auto validation = image.validate();
  if (!validation) {
    return Result<CanonicalizedImage>::failure(validation.error().code, validation.error().message);
  }

  Image working = image;
  PixelRect retained_rect{0, 0, static_cast<int32_t>(image.width), static_cast<int32_t>(image.height)};

  if (options.trim_transparent_borders) {
    auto trimmed = trim_transparent_borders(image, options.transparent_alpha_threshold, &retained_rect);
    if (!trimmed) {
      return Result<CanonicalizedImage>::failure(trimmed.error().code, trimmed.error().message);
    }
    working = std::move(trimmed.value());
  }

  auto canonical_rgba = convert_image(working, PixelFormat::RGBA8);
  if (!canonical_rgba) {
    return Result<CanonicalizedImage>::failure(canonical_rgba.error().code,
                                               canonical_rgba.error().message);
  }

  CanonicalizedImage result;
  result.image = std::move(canonical_rgba.value());
  result.retained_rect = retained_rect;
  return Result<CanonicalizedImage>::success(std::move(result));
}

Result<CanonicalTextureId> compute_canonical_texture_id(const Image& image,
                                                        const CanonicalizationOptions& options) {
  auto canonical = canonicalize_image(image, options);
  if (!canonical) {
    return Result<CanonicalTextureId>::failure(canonical.error().code, canonical.error().message);
  }

  const auto& canonical_image = canonical.value().image;
  std::vector<uint8_t> serialized;
  serialized.reserve(32U + canonical_image.pixels.size());

  const char* magic = "libatlas-canonical-v1";
  serialized.insert(serialized.end(), magic, magic + std::strlen(magic));
  append_u32_le(serialized, canonical_image.width);
  append_u32_le(serialized, canonical_image.height);
  append_u32_le(serialized, 1U);
  serialized.insert(serialized.end(), canonical_image.pixels.begin(), canonical_image.pixels.end());

  Sha256 sha256;
  sha256.update(serialized.data(), serialized.size());

  CanonicalTextureId id;
  id.bytes = sha256.finalize();
  return Result<CanonicalTextureId>::success(std::move(id));
}

}  // namespace libatlas
