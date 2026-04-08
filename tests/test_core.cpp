#include "test_harness.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using libatlas::AtlasPackOptions;
using libatlas::ExtractionOptions;
using libatlas::ExtractionWarning;
using libatlas::Image;
using libatlas::PackItem;
using libatlas::PixelRect;
using libatlas::RgbaPixel;
using libatlas::SimilarityOptions;
using libatlas::UvOrigin;
using libatlas::UvRect;
using libatlas::UvRoundingPolicy;

std::map<std::string, libatlas::PackedPlacement> placements_by_id(
    const libatlas::AtlasPackResult& result) {
  std::map<std::string, libatlas::PackedPlacement> placements;
  for (const auto& placement : result.placements) {
    placements.emplace(placement.entry_id, placement);
  }
  return placements;
}

bool nearly_equal(double lhs, double rhs, double epsilon = 1e-9) {
  return std::abs(lhs - rhs) <= epsilon;
}

}  // namespace

LIBATLAS_TEST(resolve_uv_rect_top_left_expand) {
  Image atlas = libatlas_test::make_rgba_image(100, 50);
  ExtractionOptions options;
  options.uv_origin = UvOrigin::TopLeft;
  options.rounding_policy = UvRoundingPolicy::Expand;

  auto resolved = libatlas::resolve_uv_rect(atlas, UvRect{0.10, 0.20, 0.20, 0.60}, options);
  REQUIRE_OK(resolved);

  EXPECT_EQ(resolved.value().requested_rect.x, 10);
  EXPECT_EQ(resolved.value().requested_rect.y, 10);
  EXPECT_EQ(resolved.value().requested_rect.width, 10);
  EXPECT_EQ(resolved.value().requested_rect.height, 20);
  EXPECT_TRUE(resolved.value().warnings.empty());
}

LIBATLAS_TEST(resolve_uv_rect_bottom_left_clamps) {
  Image atlas = libatlas_test::make_rgba_image(10, 10);
  ExtractionOptions options;
  options.uv_origin = UvOrigin::BottomLeft;
  options.rounding_policy = UvRoundingPolicy::Nearest;

  auto resolved = libatlas::resolve_uv_rect(atlas, UvRect{-0.10, 1.10, 0.20, 0.60}, options);
  REQUIRE_OK(resolved);

  EXPECT_EQ(resolved.value().requested_rect.x, -1);
  EXPECT_EQ(resolved.value().requested_rect.y, 4);
  EXPECT_EQ(resolved.value().requested_rect.width, 12);
  EXPECT_EQ(resolved.value().requested_rect.height, 4);
  EXPECT_EQ(resolved.value().clamped_rect.x, 0);
  EXPECT_EQ(resolved.value().clamped_rect.y, 4);
  EXPECT_EQ(resolved.value().clamped_rect.width, 10);
  EXPECT_EQ(resolved.value().clamped_rect.height, 4);
  EXPECT_TRUE(
      libatlas_test::has_warning(resolved.value().warnings, ExtractionWarning::ClampedToAtlasBounds));
}

LIBATLAS_TEST(trim_transparent_borders_finds_tight_bounds) {
  Image image = libatlas_test::make_rgba_image(4, 4);
  libatlas_test::set_pixel(image, 1, 2, RgbaPixel{255, 0, 0, 255});
  libatlas_test::set_pixel(image, 2, 2, RgbaPixel{255, 0, 0, 255});

  PixelRect kept_rect;
  auto trimmed = libatlas::trim_transparent_borders(image, 0, &kept_rect);
  REQUIRE_OK(trimmed);

  EXPECT_EQ(kept_rect.x, 1);
  EXPECT_EQ(kept_rect.y, 2);
  EXPECT_EQ(kept_rect.width, 2);
  EXPECT_EQ(kept_rect.height, 1);
  EXPECT_EQ(trimmed.value().width, 2U);
  EXPECT_EQ(trimmed.value().height, 1U);
}

LIBATLAS_TEST(canonical_hash_ignores_transparent_padding_when_trim_enabled) {
  Image inner = libatlas_test::make_rgba_image(3, 3);
  libatlas_test::set_pixel(inner, 1, 1, RgbaPixel{255, 255, 255, 255});

  Image padded = libatlas_test::make_rgba_image(5, 5);
  libatlas_test::set_pixel(padded, 2, 2, RgbaPixel{255, 255, 255, 255});

  libatlas::CanonicalizationOptions options;
  options.trim_transparent_borders = true;

  auto id_a = libatlas::compute_canonical_texture_id(inner, options);
  auto id_b = libatlas::compute_canonical_texture_id(padded, options);
  REQUIRE_OK(id_a);
  REQUIRE_OK(id_b);

  EXPECT_EQ(id_a.value(), id_b.value());
}

LIBATLAS_TEST(extract_texture_stable_id_survives_extra_transparent_border) {
  Image atlas = libatlas_test::make_rgba_image(8, 4);
  for (uint32_t y = 1; y <= 2; ++y) {
    for (uint32_t x = 2; x <= 3; ++x) {
      libatlas_test::set_pixel(atlas, x, y, RgbaPixel{255, 0, 0, 255});
    }
  }

  ExtractionOptions options;
  options.trim_transparent_borders = true;

  auto exact_crop = libatlas::extract_texture(atlas, UvRect{0.25, 0.50, 0.25, 0.75}, options);
  auto padded_crop = libatlas::extract_texture(atlas, UvRect{0.125, 0.50, 0.25, 0.75}, options);
  REQUIRE_OK(exact_crop);
  REQUIRE_OK(padded_crop);

  EXPECT_EQ(exact_crop.value().metadata.exact_id, padded_crop.value().metadata.exact_id);
  EXPECT_EQ(padded_crop.value().metadata.cropped_width, 3U);
  EXPECT_EQ(padded_crop.value().metadata.trimmed_width, 2U);
  EXPECT_TRUE(libatlas_test::has_warning(padded_crop.value().metadata.warnings,
                                         ExtractionWarning::TransparentBordersTrimmed));
}

LIBATLAS_TEST(similarity_detects_likely_related_non_exact_variants) {
  Image lhs = libatlas_test::make_rgba_image(4, 4);
  Image rhs = libatlas_test::make_rgba_image(4, 4);

  for (uint32_t y = 1; y <= 2; ++y) {
    for (uint32_t x = 1; x <= 2; ++x) {
      libatlas_test::set_pixel(lhs, x, y, RgbaPixel{255, 255, 255, 255});
      libatlas_test::set_pixel(rhs, x, y, RgbaPixel{255, 255, 255, 255});
    }
  }
  libatlas_test::set_pixel(rhs, 0, 1, RgbaPixel{255, 255, 255, 255});

  libatlas::CanonicalizationOptions canonicalization;
  canonicalization.trim_transparent_borders = true;

  auto lhs_id = libatlas::compute_canonical_texture_id(lhs, canonicalization);
  auto rhs_id = libatlas::compute_canonical_texture_id(rhs, canonicalization);
  REQUIRE_OK(lhs_id);
  REQUIRE_OK(rhs_id);
  EXPECT_FALSE(lhs_id.value() == rhs_id.value());

  SimilarityOptions similarity_options;
  auto lhs_sig = libatlas::compute_similarity_signature(lhs, similarity_options);
  auto rhs_sig = libatlas::compute_similarity_signature(rhs, similarity_options);
  REQUIRE_OK(lhs_sig);
  REQUIRE_OK(rhs_sig);

  const auto comparison =
      libatlas::compare_similarity(lhs_sig.value(), rhs_sig.value(), similarity_options);
  if (!comparison.likely_related) {
    throw std::runtime_error("expected likely_related=true but got false; luminance_distance=" +
                             std::to_string(comparison.luminance_distance) +
                             ", alpha_distance=" + std::to_string(comparison.alpha_distance) +
                             ", dimension_ratio=" + std::to_string(comparison.dimension_ratio) +
                             ", aspect_ratio_delta=" +
                             std::to_string(comparison.aspect_ratio_delta) +
                             ", score=" + std::to_string(comparison.score));
  }
}

LIBATLAS_TEST(pack_atlases_is_deterministic_and_regenerates_uvs) {
  Image large = libatlas_test::make_rgba_image(4, 2);
  Image small = libatlas_test::make_rgba_image(2, 2);
  libatlas_test::set_pixel(large, 0, 0, RgbaPixel{0, 255, 0, 255});
  libatlas_test::set_pixel(small, 0, 0, RgbaPixel{0, 0, 255, 255});

  AtlasPackOptions options;
  options.max_atlas_width = 16;
  options.max_atlas_height = 16;
  options.padding = 1;

  std::vector<PackItem> order_a{
      PackItem{"small", small, "small"},
      PackItem{"large", large, "large"},
  };
  std::vector<PackItem> order_b{
      PackItem{"large", large, "large"},
      PackItem{"small", small, "small"},
  };

  auto packed_a = libatlas::pack_atlases(order_a, options);
  auto packed_b = libatlas::pack_atlases(order_b, options);
  REQUIRE_OK(packed_a);
  REQUIRE_OK(packed_b);

  EXPECT_EQ(packed_a.value().atlases.size(), 1U);
  EXPECT_EQ(packed_a.value().atlases[0].image.width, 9U);
  EXPECT_EQ(packed_a.value().atlases[0].image.height, 4U);

  const auto placements_a = placements_by_id(packed_a.value());
  const auto placements_b = placements_by_id(packed_b.value());

  EXPECT_EQ(placements_a.at("large").pixel_rect.x, 1);
  EXPECT_EQ(placements_a.at("large").pixel_rect.y, 1);
  EXPECT_EQ(placements_a.at("small").pixel_rect.x, 6);
  EXPECT_EQ(placements_a.at("small").pixel_rect.y, 1);

  EXPECT_EQ(placements_a.at("large").pixel_rect.x, placements_b.at("large").pixel_rect.x);
  EXPECT_EQ(placements_a.at("small").pixel_rect.x, placements_b.at("small").pixel_rect.x);

  EXPECT_TRUE(nearly_equal(placements_a.at("large").uv_rect.x_min, 1.0 / 9.0));
  EXPECT_TRUE(nearly_equal(placements_a.at("large").uv_rect.x_max, 5.0 / 9.0));
  EXPECT_TRUE(nearly_equal(placements_a.at("large").uv_rect.y_min, 1.0 / 4.0));
  EXPECT_TRUE(nearly_equal(placements_a.at("large").uv_rect.y_max, 3.0 / 4.0));
}

LIBATLAS_TEST(pack_atlases_spills_to_multiple_atlases_when_needed) {
  Image tile = libatlas_test::make_rgba_image(3, 3);
  AtlasPackOptions options;
  options.max_atlas_width = 8;
  options.max_atlas_height = 5;
  options.padding = 1;

  std::vector<PackItem> items{
      PackItem{"tile_a", tile, "tile_a"},
      PackItem{"tile_b", tile, "tile_b"},
  };

  auto packed = libatlas::pack_atlases(items, options);
  REQUIRE_OK(packed);

  EXPECT_EQ(packed.value().atlases.size(), 2U);
  EXPECT_EQ(packed.value().placements.size(), 2U);
  EXPECT_EQ(packed.value().placements[0].atlas_index, 0U);
  EXPECT_EQ(packed.value().placements[1].atlas_index, 1U);
}
