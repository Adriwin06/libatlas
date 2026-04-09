# libatlas

`libatlas` is a standalone C++17 library for de-atlasing, identifying, comparing, and re-atlasing UI textures.

It is designed for APT/FLAPT-adjacent workflows such as Burnout Paradise HUD and menu recovery, but the core library does not depend on `libapt2`, FLAPT parsers, or any game-specific file format. Callers pass in atlas bitmap data and UV rectangles directly.

## What It Does

- Extracts sub-textures from atlases using plain UV rectangles
- Resolves UVs with explicit origin, rounding, and clamping policies
- Preserves rich extraction metadata for auditability and reconstruction
- Computes both raw-crop and trimmed deterministic IDs
- Computes deterministic exact texture IDs from canonicalized pixel content
- Computes lightweight near-duplicate signatures for candidate matching
- Classifies near-duplicate candidates into auto-merge and review buckets
- Rebuilds one or more deterministic output atlases with regenerated UVs
- Supports replacing extracted content with higher-resolution images before packing
- Provides cached extraction and a persistent asset-store option in the CLI
- Provides PNG convenience I/O and a small JSON-driven CLI for testing pipelines

## Design Constraints

- No dependency on `libapt2` inside the core
- No atlas-name or path coupling in logical texture identity
- Deterministic behavior suitable for automation
- Focus on raw bitmap processing first
- Practical handling of transparent padding, UV rounding drift, and layout differences

## Project Layout

- `include/libatlas/`: public API headers
- `src/`: core library implementation
- `tests/`: unit tests
- `examples/`: small API example
- `tools/`: CLI utility
- `docs/`: architecture and workflow docs
- `third_party/`: vendored lightweight dependencies

## Build

```bash
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build --output-on-failure -C Debug
```

Targets:

- `libatlas`: core library
- `libatlas_tests`: unit tests
- `libatlas_example`: small in-memory demo
- `libatlas_tool`: JSON/PNG CLI

## Public API Overview

Main headers:

- `libatlas/image.hpp`
- `libatlas/extraction.hpp`
- `libatlas/identity.hpp`
- `libatlas/similarity.hpp`
- `libatlas/packing.hpp`
- `libatlas/image_io.hpp`
- `libatlas/workflow.hpp`
- `libatlas/libatlas.hpp`

Typical flow:

```cpp
#include "libatlas/libatlas.hpp"

libatlas::Image atlas = ...;            // raw RGBA/BGRA/RGB/Gray bitmap
libatlas::UvRect uv{0.1, 0.2, 0.3, 0.4};

libatlas::ExtractionOptions extraction;
extraction.source_atlas_identifier = "hud_main";
extraction.trim_transparent_borders = true;

auto extracted = libatlas::extract_texture(atlas, uv, extraction);
if (!extracted) {
  throw std::runtime_error(extracted.error().message);
}

std::string logical_id = extracted.value().metadata.exact_id.hex();
libatlas::Image image_for_packing = extracted.value().trimmed_image;

libatlas::AtlasPackOptions pack_options;
pack_options.max_atlas_width = 1024;
pack_options.max_atlas_height = 1024;
pack_options.padding = 1;

std::vector<libatlas::PackItem> items{
  {logical_id, image_for_packing, "boost_icon"}
};

auto packed = libatlas::pack_atlases(items, pack_options);
```

High-level dedup/remap flow:

```cpp
std::vector<libatlas::TextureOccurrence> occurrences{
  {"geometry_001", extracted_a.value()},
  {"geometry_044", extracted_b.value()}
};

auto workflow = libatlas::deduplicate_and_pack_occurrences(occurrences, pack_options);
if (!workflow) {
  throw std::runtime_error(workflow.error().message);
}

// Each original geometry occurrence now points at the packed placement.
for (const auto& mapping : workflow.value().remapped.occurrence_mappings) {
  // mapping.occurrence_id -> mapping.atlas_identifier + mapping.uv_rect
}
```

Two-pass cached extraction flow:

```cpp
libatlas::ExtractionIdentityCache cache;

auto extracted = libatlas::extract_texture_cached(atlas, uv, extraction, &cache);
if (!extracted) {
  throw std::runtime_error(extracted.error().message);
}

// metadata.cropped_exact_id is the raw crop hash.
// metadata.exact_id is the trimmed logical texture ID.
// metadata.cache_outcome tells you whether this was a raw-crop hit,
// an exact-id hit after trimming, or a new cache entry.
```

## CLI

`libatlas_tool` is a small debugging and validation utility. It is not where the real logic lives. It calls the same public API another tool would call.

### Extract

```bash
libatlas_tool extract \
  --atlas atlas.png \
  --requests requests.json \
  --output-dir out/extracted \
  --metadata out/extracted.json \
  --asset-store asset_store
```

Request JSON:

```json
{
  "atlas_identifier": "hud_main",
  "items": [
    {
      "name": "boost_icon",
      "uv": {
        "x_min": 0.125,
        "x_max": 0.250,
        "y_min": 0.250,
        "y_max": 0.375
      }
    }
  ]
}
```

The tool writes cropped and trimmed PNGs plus metadata JSON containing:

- `cropped_exact_id`
- `exact_id`
- `cache_outcome`
- UVs and pixel rectangles
- similarity signatures
- warnings

If `--asset-store` is provided, the tool also maintains a persistent content-addressed store of:

- source atlases
- unique raw crops keyed by `cropped_exact_id`
- unique canonical images keyed by `exact_id`
- occurrence metadata that maps each request back to those IDs

Running `extract` again with the same store preloads the two-pass cache and can skip trimming when a raw crop already exists.

### Pack

```bash
libatlas_tool pack \
  --manifest pack.json \
  --output-dir out/packed \
  --metadata out/packed.json \
  --max-width 1024 \
  --max-height 1024 \
  --padding 1
```

Pack manifest JSON:

```json
{
  "items": [
    {
      "entry_id": "sha256:v1:...",
      "image": "out/extracted/0_boost_icon_trimmed.png",
      "source_label": "boost_icon"
    }
  ]
}
```

### Similarity Report

```bash
libatlas_tool similarity-report \
  --metadata-dir out/extract_metadata \
  --output out/similarity_report.json \
  --review-min-score 0.90 \
  --auto-min-score 0.92
```

This command reads extraction metadata JSON files, groups strict matches by `exact_id`, and emits
a report of:

- high-confidence non-exact duplicate pairs
- review-only candidate pairs
- connected components for both auto-duplicate and review candidate groups

The classification policy comes from the C++ similarity API rather than the Python fixture script.

### Fixture Smoke Run

The repository also includes `tests/images_files` DDS/TGA fixtures. The core library still does not decode those formats directly, but `tools/run_fixture_pipeline.py` can convert them with Pillow, derive normalized top-left UVs, run `libatlas_tool extract`, and repack the trimmed outputs. By default it extracts one item per qualifying disconnected alpha component, which is useful for fully de-atlasing sprite sheets. If you want the older behavior, `--split-mode auto` only splits simple multi-sprite sheets and `--split-mode bbox` forces one visible-bounds crop per source image.

```bash
cmake --build build --config Debug --target libatlas_tool
python tools/run_fixture_pipeline.py --config Debug
```

By default the script writes converted PNGs, per-image extraction metadata, packed atlas PNGs, and a summary JSON under `build/fixture_pipeline/`.

It also maintains a persistent logical image store under `build/fixture_logical_store/` by default.
That folder contains one editable PNG per logical texture group. When the pipeline packs outputs,
all occurrences mapped to the same logical group reuse that one image.

The logical store layout is:

- `images/`
  - one editable PNG per current logical texture group
- `metadata/logical_groups.json`
  - the current logical groups and which exact IDs they contain
- `review_candidates/groups/`
  - one folder per unresolved review cluster
  - each folder contains:
    - `images/`
    - `contact_sheet.png`
    - `group.json`
    - `decision.json`

`decision.json` is the automation point for manual review. Reviewers do not edit the main
logical-group metadata directly. Instead they record aliases from loser logical IDs to the winner
logical ID inside each review group folder, then rerun the fixture pipeline. The script applies
those decisions automatically, rebuilds the logical groups, updates remap metadata, and removes
resolved review clusters. `decision.json` can also persist `distinct_pairs` so reviewed items that
are confirmed different stop resurfacing as the same unresolved review cluster.

Example review decision:

```json
{
  "group_id": "group__02_items__example",
  "status": "reviewed",
  "notes": "Same icon with compression noise.",
  "aliases": {
    "sha256_v1_loser": "sha256_v1_winner"
  },
  "distinct_pairs": [],
  "available_logical_ids": [
    "sha256_v1_winner",
    "sha256_v1_loser"
  ]
}
```

If you want a desktop wrapper around that workflow, run:

```bash
python tools/fixture_pipeline_ui/main.py
```

The UI lets you select the source atlas folder, choose a workspace root where
`fixture_asset_store/`, `fixture_logical_store/`, and `fixture_pipeline/` will be created, run the
pipeline, and review candidate groups by marking logical images as the same or different.

To use a persistent store across fixture runs:

```bash
python tools/run_fixture_pipeline.py --config Debug --asset-store C:/path/to/libatlas_store
```

## Dependencies

Core library:

- C++17 standard library only

Vendored convenience dependencies:

- `lodepng` for PNG loading/saving
- `picojson` for CLI JSON parsing/writing

## Important Behavior

- Exact IDs are derived from canonical pixel content only
- Raw crop IDs are also available and are derived from the unclipped extracted crop before transparent-border trimming
- Atlas names and source coordinates are informational metadata only
- Trimming fully transparent borders can make two crops with different transparent padding share the same exact ID
- Two-pass cached extraction first checks the raw crop ID, then falls back to the trimmed exact ID
- Near-duplicate matching is advisory, not identity
- The asset store only auto-deduplicates exact IDs
- The logical store can additionally merge reviewed near-duplicates through `decision.json` aliases
- Fully transparent trimmed results canonicalize to `0 x 0`
- The packer is deterministic and shelf-based, not globally optimal
- Zero-sized images are not packable and must be handled by the caller

## Docs

- [Architecture](docs/architecture.md)
- [Extraction Pipeline](docs/extraction_pipeline.md)
- [Repacking](docs/repacking.md)
- [Integration Guide](docs/integration.md)
- [Asset Store](docs/asset_store.md)

## License

This repository is MIT-licensed. Vendored third-party components keep their own upstream licenses.
