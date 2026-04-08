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

### Fixture Smoke Run

The repository also includes `tests/images_files` DDS/TGA fixtures. The core library still does not decode those formats directly, but `tools/run_fixture_pipeline.py` can convert them with Pillow, derive normalized top-left UVs, run `libatlas_tool extract`, and repack the trimmed outputs. By default it extracts one item per qualifying disconnected alpha component, which is useful for fully de-atlasing sprite sheets. If you want the older behavior, `--split-mode auto` only splits simple multi-sprite sheets and `--split-mode bbox` forces one visible-bounds crop per source image.

```bash
cmake --build build --config Debug --target libatlas_tool
python tools/run_fixture_pipeline.py --config Debug
```

By default the script writes converted PNGs, per-image extraction metadata, packed atlas PNGs, and a summary JSON under `build/fixture_pipeline/`.

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
