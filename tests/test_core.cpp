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
using libatlas::ExtractionIdentityCache;
using libatlas::ExtractionOptions;
using libatlas::ExtractionWarning;
using libatlas::IdentityCacheOutcome;
using libatlas::Image;
using libatlas::PackItem;
using libatlas::PixelRect;
using libatlas::RgbaPixel;
using libatlas::SimilarityOptions;
using libatlas::TextureOccurrence;
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

LIBATLAS_TEST(extract_texture_reports_distinct_cropped_id_and_shared_exact_id) {
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

  EXPECT_FALSE(exact_crop.value().metadata.cropped_exact_id ==
               padded_crop.value().metadata.cropped_exact_id);
  EXPECT_EQ(exact_crop.value().metadata.exact_id, padded_crop.value().metadata.exact_id);
}

LIBATLAS_TEST(extract_texture_cached_short_circuits_identical_raw_crops) {
  Image atlas = libatlas_test::make_rgba_image(8, 4);
  for (uint32_t y = 1; y <= 2; ++y) {
    for (uint32_t x = 2; x <= 3; ++x) {
      libatlas_test::set_pixel(atlas, x, y, RgbaPixel{255, 0, 0, 255});
    }
  }

  ExtractionOptions options;
  options.trim_transparent_borders = true;

  ExtractionIdentityCache cache;
  auto first = libatlas::extract_texture_cached(atlas, UvRect{0.125, 0.50, 0.25, 0.75}, options, &cache);
  auto second = libatlas::extract_texture_cached(atlas, UvRect{0.125, 0.50, 0.25, 0.75}, options, &cache);
  REQUIRE_OK(first);
  REQUIRE_OK(second);

  EXPECT_EQ(first.value().metadata.cache_outcome, IdentityCacheOutcome::NewEntry);
  EXPECT_EQ(second.value().metadata.cache_outcome, IdentityCacheOutcome::CroppedExactHit);
  EXPECT_EQ(cache.exact_entry_count(), 1U);
  EXPECT_EQ(cache.cropped_alias_count(), 1U);
  EXPECT_EQ(first.value().metadata.exact_id, second.value().metadata.exact_id);
  EXPECT_EQ(first.value().metadata.cropped_exact_id, second.value().metadata.cropped_exact_id);
}

LIBATLAS_TEST(extract_texture_cached_uses_exact_id_hit_for_new_raw_crop_alias) {
  Image atlas = libatlas_test::make_rgba_image(8, 4);
  for (uint32_t y = 1; y <= 2; ++y) {
    for (uint32_t x = 2; x <= 3; ++x) {
      libatlas_test::set_pixel(atlas, x, y, RgbaPixel{255, 0, 0, 255});
    }
  }

  ExtractionOptions options;
  options.trim_transparent_borders = true;

  ExtractionIdentityCache cache;
  auto exact_crop = libatlas::extract_texture_cached(atlas, UvRect{0.25, 0.50, 0.25, 0.75}, options, &cache);
  auto padded_crop = libatlas::extract_texture_cached(atlas, UvRect{0.125, 0.50, 0.25, 0.75}, options, &cache);
  auto padded_crop_again = libatlas::extract_texture_cached(atlas, UvRect{0.125, 0.50, 0.25, 0.75}, options, &cache);
  REQUIRE_OK(exact_crop);
  REQUIRE_OK(padded_crop);
  REQUIRE_OK(padded_crop_again);

  EXPECT_EQ(exact_crop.value().metadata.cache_outcome, IdentityCacheOutcome::NewEntry);
  EXPECT_EQ(padded_crop.value().metadata.cache_outcome, IdentityCacheOutcome::ExactIdHit);
  EXPECT_EQ(padded_crop_again.value().metadata.cache_outcome, IdentityCacheOutcome::CroppedExactHit);
  EXPECT_EQ(cache.exact_entry_count(), 1U);
  EXPECT_EQ(cache.cropped_alias_count(), 2U);
  EXPECT_EQ(exact_crop.value().metadata.exact_id, padded_crop.value().metadata.exact_id);
  EXPECT_FALSE(exact_crop.value().metadata.cropped_exact_id ==
               padded_crop.value().metadata.cropped_exact_id);
}

LIBATLAS_TEST(parse_canonical_texture_id_roundtrips) {
  Image image = libatlas_test::make_rgba_image(2, 2);
  libatlas_test::set_pixel(image, 0, 0, RgbaPixel{255, 255, 255, 255});

  libatlas::CanonicalizationOptions options;
  options.trim_transparent_borders = false;

  auto id = libatlas::compute_canonical_texture_id(image, options);
  REQUIRE_OK(id);

  auto parsed_full = libatlas::parse_canonical_texture_id(id.value().to_string());
  auto parsed_hex = libatlas::parse_canonical_texture_id(id.value().hex());
  REQUIRE_OK(parsed_full);
  REQUIRE_OK(parsed_hex);

  EXPECT_EQ(parsed_full.value(), id.value());
  EXPECT_EQ(parsed_hex.value(), id.value());
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

LIBATLAS_TEST(deduplicate_extractions_by_exact_id_groups_shared_elements) {
  Image atlas_a = libatlas_test::make_rgba_image(6, 4);
  Image atlas_b = libatlas_test::make_rgba_image(6, 4);

  for (uint32_t y = 1; y <= 2; ++y) {
    for (uint32_t x = 1; x <= 2; ++x) {
      libatlas_test::set_pixel(atlas_a, x, y, RgbaPixel{255, 0, 0, 255});
      libatlas_test::set_pixel(atlas_b, x + 2, y, RgbaPixel{255, 0, 0, 255});
    }
  }
  for (uint32_t y = 0; y <= 1; ++y) {
    for (uint32_t x = 0; x <= 1; ++x) {
      libatlas_test::set_pixel(atlas_b, x, y, RgbaPixel{0, 0, 255, 255});
    }
  }

  ExtractionOptions options;
  options.trim_transparent_borders = true;
  options.source_atlas_identifier = "atlas_a";
  auto red_a = libatlas::extract_texture(atlas_a, UvRect{1.0 / 6.0, 3.0 / 6.0, 1.0 / 4.0, 3.0 / 4.0}, options);
  REQUIRE_OK(red_a);

  options.source_atlas_identifier = "atlas_b";
  auto red_b = libatlas::extract_texture(atlas_b, UvRect{3.0 / 6.0, 5.0 / 6.0, 1.0 / 4.0, 3.0 / 4.0}, options);
  auto blue_b = libatlas::extract_texture(atlas_b, UvRect{0.0, 2.0 / 6.0, 0.0, 2.0 / 4.0}, options);
  REQUIRE_OK(red_b);
  REQUIRE_OK(blue_b);

  std::vector<TextureOccurrence> occurrences{
      TextureOccurrence{"geom_a", red_a.value()},
      TextureOccurrence{"geom_b", red_b.value()},
      TextureOccurrence{"geom_c", blue_b.value()},
  };

  auto deduplicated = libatlas::deduplicate_extractions_by_exact_id(occurrences);
  REQUIRE_OK(deduplicated);

  EXPECT_EQ(deduplicated.value().logical_textures.size(), 2U);
  EXPECT_EQ(deduplicated.value().occurrence_mappings.size(), 3U);
  EXPECT_EQ(deduplicated.value().occurrence_mappings[0].logical_texture_index,
            deduplicated.value().occurrence_mappings[1].logical_texture_index);
  EXPECT_FALSE(deduplicated.value().occurrence_mappings[0].logical_texture_index ==
               deduplicated.value().occurrence_mappings[2].logical_texture_index);
  EXPECT_EQ(deduplicated.value().logical_textures
                [deduplicated.value().occurrence_mappings[0].logical_texture_index]
                    .occurrence_indices.size(),
            2U);
}

LIBATLAS_TEST(deduplicate_and_pack_occurrences_remaps_shared_elements_to_one_placement) {
  Image atlas_a = libatlas_test::make_rgba_image(8, 4);
  Image atlas_b = libatlas_test::make_rgba_image(8, 4);

  for (uint32_t y = 1; y <= 2; ++y) {
    for (uint32_t x = 1; x <= 2; ++x) {
      libatlas_test::set_pixel(atlas_a, x, y, RgbaPixel{255, 255, 0, 255});
      libatlas_test::set_pixel(atlas_b, x + 3, y, RgbaPixel{255, 255, 0, 255});
    }
  }
  for (uint32_t y = 1; y <= 2; ++y) {
    for (uint32_t x = 6; x <= 7; ++x) {
      libatlas_test::set_pixel(atlas_b, x, y, RgbaPixel{0, 255, 255, 255});
    }
  }

  ExtractionOptions extraction_options;
  extraction_options.trim_transparent_borders = true;
  extraction_options.source_atlas_identifier = "atlas_a";
  auto yellow_a = libatlas::extract_texture(atlas_a, UvRect{1.0 / 8.0, 3.0 / 8.0, 1.0 / 4.0, 3.0 / 4.0}, extraction_options);
  REQUIRE_OK(yellow_a);

  extraction_options.source_atlas_identifier = "atlas_b";
  auto yellow_b = libatlas::extract_texture(atlas_b, UvRect{4.0 / 8.0, 6.0 / 8.0, 1.0 / 4.0, 3.0 / 4.0}, extraction_options);
  auto cyan_b = libatlas::extract_texture(atlas_b, UvRect{6.0 / 8.0, 1.0, 1.0 / 4.0, 3.0 / 4.0}, extraction_options);
  REQUIRE_OK(yellow_b);
  REQUIRE_OK(cyan_b);

  std::vector<TextureOccurrence> occurrences{
      TextureOccurrence{"hud_speed", yellow_a.value()},
      TextureOccurrence{"hud_boost", yellow_b.value()},
      TextureOccurrence{"hud_other", cyan_b.value()},
  };

  AtlasPackOptions pack_options;
  pack_options.max_atlas_width = 16;
  pack_options.max_atlas_height = 16;
  pack_options.padding = 1;

  auto workflow = libatlas::deduplicate_and_pack_occurrences(occurrences, pack_options);
  REQUIRE_OK(workflow);

  EXPECT_EQ(workflow.value().deduplicated.logical_textures.size(), 2U);
  EXPECT_EQ(workflow.value().packed.placements.size(), 2U);
  EXPECT_EQ(workflow.value().remapped.occurrence_mappings.size(), 3U);

  const auto& geom_speed = workflow.value().remapped.occurrence_mappings[0];
  const auto& geom_boost = workflow.value().remapped.occurrence_mappings[1];
  const auto& geom_other = workflow.value().remapped.occurrence_mappings[2];

  EXPECT_EQ(geom_speed.entry_id, geom_boost.entry_id);
  EXPECT_EQ(geom_speed.atlas_identifier, geom_boost.atlas_identifier);
  EXPECT_TRUE(nearly_equal(geom_speed.uv_rect.x_min, geom_boost.uv_rect.x_min));
  EXPECT_TRUE(nearly_equal(geom_speed.uv_rect.x_max, geom_boost.uv_rect.x_max));
  EXPECT_TRUE(nearly_equal(geom_speed.uv_rect.y_min, geom_boost.uv_rect.y_min));
  EXPECT_TRUE(nearly_equal(geom_speed.uv_rect.y_max, geom_boost.uv_rect.y_max));
  EXPECT_FALSE(geom_speed.entry_id == geom_other.entry_id);
}
