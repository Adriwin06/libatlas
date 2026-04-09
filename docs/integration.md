# Integration Guide

## Why `libatlas` Is Independent

`libatlas` is intentionally not a parser for APT, FLAPT, or any other container format.

That separation matters because it lets you:

- keep parser-specific logic in one layer
- reuse the same image/extraction/packing logic from multiple tools
- compare atlases that came from different sources
- migrate between parser libraries without rewriting the texture workflow

In practice:

- `libapt2` or a FLAPT parser discovers atlas references and UVs
- your tool loads or decodes atlas bitmaps
- your tool passes those UVs and bitmaps into `libatlas`

## Minimal Call Pattern

```cpp
#include "libatlas/libatlas.hpp"

libatlas::Image atlas_image = decode_or_load_atlas_somehow();
libatlas::UvRect uv{node.x_min, node.x_max, node.y_min, node.y_max};

libatlas::ExtractionOptions options;
options.source_atlas_identifier = atlas_name;
options.uv_origin = libatlas::UvOrigin::TopLeft;
options.rounding_policy = libatlas::UvRoundingPolicy::Expand;
options.trim_transparent_borders = true;

auto extracted = libatlas::extract_texture(atlas_image, uv, options);
if (!extracted) {
  throw std::runtime_error(extracted.error().message);
}

std::string exact_id = extracted.value().metadata.exact_id.hex();
```

The parser layer stays outside the library. `libatlas` only needs the atlas bitmap and the UV rectangle.

If you are processing many repeated crops, use the cached variant:

```cpp
libatlas::ExtractionIdentityCache cache;
auto extracted = libatlas::extract_texture_cached(atlas_image, uv, options, &cache);
```

That performs a raw-crop ID pass first and only trims when the raw crop is new under the same trim policy.

## Mapping From Parser Data

Typical parser-owned data:

- atlas filename or identifier
- atlas slot index
- bitmap bytes or decoded pixels
- UV rectangle
- UI symbol or widget reference

Typical `libatlas` inputs:

- `Image`
- `UvRect`
- `ExtractionOptions`

Typical `libatlas` outputs your parser-side tool may store:

- cropped exact ID
- exact ID
- similarity signature
- similarity classification results
- crop and trim rectangles
- cache outcome
- warnings
- packed atlas placements and regenerated UVs

If you want the library to handle exact-ID grouping and geometry remapping directly, use the workflow helpers in `libatlas/workflow.hpp`:

- `TextureOccurrence`
- `deduplicate_extractions_by_exact_id(...)`
- `deduplicate_and_pack_occurrences(...)`

That yields:

- one logical texture per exact ID
- one packed placement per logical texture
- one final mapping from each original occurrence ID back to packed atlas and UVs

## Dedup Across Multiple Atlases

For atlas comparison or merge workflows:

1. Extract all relevant atlas entries
2. Optionally use `metadata.cropped_exact_id` as a raw-crop fast path
3. Group by `metadata.exact_id`
4. Treat those groups as strict duplicate candidates
5. Compare `metadata.similarity_signature` for non-exact candidates
6. Classify those candidates into auto-merge and review buckets
7. Record any manual review aliases from loser logical IDs to the winner logical ID
8. Rebuild the logical groups and repack from the surviving logical IDs

That works even when:

- the same icon appears in different atlases
- one version has extra transparent padding
- UV rounding produced slightly different crops

The library and CLI now support that split explicitly:

- `compare_similarity(...)`
  - raw score plus `likely_related`
- `classify_similarity(...)`
  - applies the higher-level candidate thresholds
- `libatlas_tool similarity-report`
  - emits connected components for auto-duplicate and review clusters

## Replacement Workflow

If you want to replace low-resolution art:

1. Extract and identify textures
2. Choose a logical key for each texture
   - exact-only workflow: usually `exact_id`
   - reviewed workflow: a logical-store ID that may merge several reviewed exact IDs
3. Attach a replacement image to that key
4. Feed the replacement image into `PackItem`
5. Pack and update downstream UV references

`libatlas` does not need to know whether the replacement came from manual art, an upscale pass, or a remastered build.

## Direct Geometry Remap Workflow

If your tool already has stable geometry identifiers, the high-level path is:

1. Extract each geometry reference into an `ExtractedTexture`
2. Wrap each result as `TextureOccurrence { geometry_id, extracted_texture }`
3. Call `deduplicate_and_pack_occurrences(...)`
4. Write back:
   - `occurrence_id`
   - `atlas_identifier`
   - `uv_rect`

That is the "shared texture edited once, all matching geometry updated" workflow for APT/FLAPT-style content.

## Persistent Asset Store

If you want a long-lived deduplicated image database, keep that in your tool layer rather than the core library.

The intended pattern is:

1. Use `libatlas` to extract and identify textures
2. Store unique raw crops by `cropped_exact_id`
3. Store unique canonical textures by `exact_id`
4. Store occurrence records that map source geometry back to those IDs
5. Preload those stored IDs into `ExtractionIdentityCache` on later runs

`libatlas_tool extract --asset-store <dir>` implements exactly that pattern for PNG-based debugging and validation workflows.

If you also want a human-reviewable replacement workflow, keep a second persistent logical store:

1. Start from exact IDs and similarity reports
2. Materialize one editable image per logical group
3. Materialize review-group folders with contact sheets and manifests
4. Record decisions in per-group `decision.json` files
5. Re-run the pipeline to apply aliases and rebuild packed placements

## Migration From Parser-Coupled Tools

If an existing tool currently:

- reads APT or FLAPT data
- resolves UVs internally
- crops images directly
- writes new atlases in the same codepath

move toward this split:

1. Parser layer
   - decode the source format
   - expose atlas pixels and UV rectangles
2. `libatlas`
   - extract
   - identify
   - compare
   - repack
3. Output layer
   - write updated UVs back into your target format

This makes the texture pipeline reusable without tying it to one parser or one game build.

## Notes For Burnout-Style HUD Work

The intended APT/FLAPT-style use cases are exactly the sort of cases where this split helps:

- comparing E3, demo, 1.0, and remastered atlases
- recovering shared HUD fragments spread across several atlases
- replacing individual UI elements without rebuilding entire legacy atlases by hand
- generating compact atlases that contain only actually referenced content

None of those require `libatlas` to understand the APT container itself. They require stable image handling and explicit metadata, which is what the library provides.
