# libatlas Architecture

## Goals

`libatlas` is a standalone C++ library for extracting, identifying, comparing, and repacking textures stored inside atlases. It is designed for APT/FLAPT-oriented workflows, but it does not know anything about APT, FLAPT, libapt2, or game-specific file formats. Callers provide atlas bitmap data and UV rectangles explicitly.

The first implementation prioritizes:

- deterministic behavior for automation pipelines
- correctness around messy atlas data
- a clean reusable API
- low dependency weight
- clear metadata and warnings instead of hidden heuristics

## Scope Split

The project is split into three layers:

1. Core library
   - in-memory image model
   - UV-to-pixel resolution
   - extraction and trimming
   - deterministic canonical IDs
   - near-duplicate signatures
   - deterministic atlas packing

2. Convenience I/O
   - PNG loading and saving
   - JSON parsing and writing for the CLI

3. Tooling
   - CLI for extraction and repacking
   - tests
   - example program

The core library will not depend on libapt2 or any atlas metadata parser.

## Public API Shape

Public headers:

- `include/libatlas/result.hpp`
  - `ErrorCode`
  - `Error`
  - `Result<T>`

- `include/libatlas/image.hpp`
  - `PixelFormat`
  - `Image`
  - image helpers for format normalization and pixel access

- `include/libatlas/image_io.hpp`
  - `load_png`
  - `save_png`
  - PNG-focused convenience API

- `include/libatlas/geometry.hpp`
  - `UvRect`
  - `PixelRect`
  - `UvOrigin`
  - `UvRoundingPolicy`

- `include/libatlas/extraction.hpp`
  - `ExtractionOptions`
  - `AtlasSourceInfo`
  - `ExtractionWarning`
  - `ExtractionMetadata`
  - `ExtractedTexture`
  - `resolve_uv_rect`
  - `extract_texture`

- `include/libatlas/identity.hpp`
  - `CanonicalizationOptions`
  - `CanonicalTextureId`
  - `compute_canonical_texture_id`

- `include/libatlas/similarity.hpp`
  - `SimilarityOptions`
  - `SimilaritySignature`
  - `SimilarityComparison`
  - `compute_similarity_signature`
  - `compare_similarity`

- `include/libatlas/packing.hpp`
  - `PackItem`
  - `AtlasPackOptions`
  - `PackedPlacement`
  - `PackedAtlas`
  - `AtlasPackResult`
  - `pack_atlases`

## Core Data Structures

### `Image`

`Image` is a row-major bitmap container:

- `width`
- `height`
- `pixel_format`
- `row_stride`
- `pixels`

The first version will fully support:

- `RGBA8`
- `BGRA8`
- `RGB8`
- `Gray8`

Canonicalization and similarity operate on normalized `RGBA8` images.

### `UvRect`

Normalized UV rectangle:

- `x_min`
- `x_max`
- `y_min`
- `y_max`

The API does not assume the rectangle is already ordered. If min/max are reversed, the resolver normalizes the values and emits a warning.

### `PixelRect`

Half-open pixel rectangle in atlas space:

- `x`
- `y`
- `width`
- `height`

Internally, extraction resolves UVs into integer pixel bounds, clamps them to atlas extents, and stores the resulting half-open region as a `PixelRect`.

### `ExtractionMetadata`

Tracks the relationship between source data and extracted images:

- optional `source_atlas_identifier` for traceability only
- source atlas width and height
- original requested UV rectangle
- resolved pixel rectangle before clamping
- clamped crop rectangle
- trimmed rectangle in atlas coordinates
- trimmed rectangle relative to the crop
- crop size and trimmed size
- alpha coverage statistics
- exact ID
- similarity signature
- warnings

### `ExtractedTexture`

Contains both images that matter for downstream workflows:

- `cropped_image`
  - exact clamped crop from the source atlas
- `trimmed_image`
  - crop after optional transparent-border trimming
- `metadata`

This split is intentional. Callers can use `trimmed_image` for identity and deduplication, but still keep `cropped_image` when layout-preserving behavior is needed.

## Extraction Pipeline

The extraction pipeline is policy-driven and deterministic:

1. Validate atlas image
2. Normalize UV ordering
3. Convert UVs to pixel bounds using the selected origin convention
4. Apply the selected rounding policy
5. Clamp to atlas bounds
6. Crop the atlas image
7. Optionally trim fully transparent borders using a configurable alpha threshold
8. Normalize the image to canonical `RGBA8`
9. Compute deterministic exact ID from canonical bytes
10. Compute near-duplicate signature
11. Return images, metadata, and warnings

Important policies:

- UV origin
  - `TopLeft`
  - `BottomLeft`

- UV rounding
  - `Expand`
    - floor min bounds and ceil max bounds
    - best default for recovery workflows because it avoids accidental under-cropping
  - `Nearest`
    - round both bounds to nearest integer
  - `Contract`
    - ceil min bounds and floor max bounds
    - useful for callers that want conservative inner crops

- Clamping
  - always enabled
  - out-of-range UVs become warnings, not silent undefined behavior

## Canonicalization Rules

Exact identity is derived from a canonical image representation, not from filenames or atlas positions.

Version 1 canonicalization rules:

1. Start from the extraction crop
2. If `trim_transparent_borders` is enabled, remove fully transparent outer rows and columns
3. Convert the result to `RGBA8`
4. Serialize deterministically as:
   - magic string
   - canonicalization version
   - width
   - height
   - pixel format tag for canonical `RGBA8`
   - raw pixel bytes in row-major order

Consequences:

- the same icon from different atlases gets the same exact ID if the canonical pixels match
- transparent padding differences do not affect exact IDs when trimming is enabled
- crop origin in the source atlas does not affect exact IDs
- atlas names, paths, and UV values do not affect exact IDs

If trimming collapses the image to empty content, the canonical image becomes `0 x 0` `RGBA8`. That makes fully transparent results comparable and explicit instead of special-cased by atlas source.

## Exact Hash Strategy

The first version will use an internal SHA-256 implementation over the canonical serialized byte stream.

Why SHA-256:

- deterministic and stable across platforms
- strong collision resistance for asset identity work
- easy to serialize as a hex string

The library will expose the raw 32-byte digest plus a hex formatter.

## Near-Duplicate Strategy

The library separates exact matching from similarity.

Exact matching:

- based on canonical SHA-256
- intended for strict deduplication

Near-duplicate matching:

- based on lightweight signatures, not identity claims
- intended to help callers group candidates for review

Version 1 signature plan:

- canonical trimmed dimensions
- alpha coverage ratio
- `8 x 8` luminance average hash
- `8 x 8` alpha average hash

Version 1 comparison plan:

- compute Hamming distance between luminance hashes
- compute Hamming distance between alpha hashes
- compare aspect ratio and normalized size ratios
- produce a similarity score and `likely_related` boolean
  - strict bit-distance thresholds remain available
  - a score-based fallback is also used for small assets where one-pixel differences can move many normalized hash bits

This is intentionally conservative. Resized textures or shifted crops may score as likely related, but the library will not claim they are exact duplicates unless the canonical hashes match.

## Atlas Packing Approach

The first implementation uses a deterministic shelf packer.

Why shelf packing first:

- easy to reason about
- deterministic
- good enough for UI assets
- simpler to validate than more aggressive packers

Packing rules:

- caller supplies `PackItem` entries with an ID and image
- items are sorted deterministically before placement
  - primary key: height descending
  - secondary key: width descending
  - tertiary key: entry ID ascending
- each atlas is filled left-to-right on shelves
- new shelves are opened top-to-bottom
- new atlases are created when an item no longer fits
- padding is applied around placed items
- UVs are regenerated from packed pixel rectangles

This yields deterministic placements for identical inputs and options. The packer returns both atlas images and per-item placement metadata.

## Dependency Choices

Core library dependencies:

- C++17 standard library only

Convenience dependencies vendored into the repository:

- `lodepng`
  - PNG decode and encode
  - used only by PNG I/O helpers and CLI paths that load or save PNG files

- `picojson`
  - small header-only JSON library
  - used by the CLI and tests that emit or parse JSON fixtures

The core library remains usable without JSON or APT-related code.

## Error Handling Strategy

The library will use explicit result objects:

- `Result<T>` for operations that can fail
- `ErrorCode` plus message for failure detail

Examples:

- invalid image buffer size
- unsupported pixel format conversion
- impossible pack request where an image is larger than the maximum atlas size
- PNG decode or encode failure

Warnings are not errors. Extraction warnings are returned inside metadata when the operation still produced a meaningful result:

- swapped UV bounds
- clamping occurred
- degenerate crop
- crop trimmed to empty

This keeps failure handling predictable in automation pipelines while preserving visibility into messy source data.

## Assumptions For Version 1

- UV rectangles are normalized in atlas space
- atlases are treated as unrotated
- exact identity is based on pixel equality after canonicalization, not semantic meaning
- near-duplicate matching is advisory and deliberately conservative
- atlas packing does not rotate textures
- DDS is not implemented in v1, but the image abstraction is designed so future codec helpers can feed the same core API

## Extension Points

The current implementation deliberately leaves room for future work:

1. DDS and other codec helpers on top of the existing `Image` abstraction
2. More advanced candidate grouping or visual comparison utilities
3. Alternate packers when tighter packing matters more than simple determinism
4. Additional export helpers for pipeline metadata formats
