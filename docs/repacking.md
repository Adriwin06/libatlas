# Repacking

## Purpose

Repacking takes chosen texture images and builds one or more new atlases with regenerated UVs.

The library does not assume the images being packed came directly from `ExtractedTexture`. A caller can pack:

- original extracted crops
- trimmed extracted images
- manually edited replacements
- higher-resolution replacements
- merged content from multiple source atlases

## Input Model

Packing uses:

- `PackItem`
  - `entry_id`
  - `image`
  - `source_label`
- `AtlasPackOptions`

`entry_id` is caller-defined. In many workflows it should be the exact canonical ID or another stable logical key.

## Packing Strategy

The first implementation uses a deterministic shelf packer.

Deterministic ordering:

- primary sort: height descending
- secondary sort: width descending
- tertiary sort: entry ID ascending

Placement behavior:

- items are placed left-to-right on a shelf
- new shelves open top-to-bottom
- a new atlas is created when the current one cannot fit the next item
- padding is applied around placements, including atlas edges

This is not the tightest possible packing strategy. It is predictable and easy to audit.

## Output

`AtlasPackResult` returns:

- `atlases`
  - atlas identifier
  - atlas image
- `placements`
  - entry ID
  - source label
  - atlas index
  - packed pixel rectangle
  - regenerated UV rectangle

This keeps the packing workflow usable from another program without relying on file output.

## UV Regeneration

Packed UVs are derived from the final atlas dimensions and the packed pixel rectangle.

For top-left UVs:

- `x_min = x / atlas_width`
- `x_max = (x + width) / atlas_width`
- `y_min = y / atlas_height`
- `y_max = (y + height) / atlas_height`

For bottom-left UVs:

- `x_min = x / atlas_width`
- `x_max = (x + width) / atlas_width`
- `y_min = 1 - ((y + height) / atlas_height)`
- `y_max = 1 - (y / atlas_height)`

## High-Resolution Replacement Workflow

The core library does not perform upscaling. It supports replacement by letting the caller swap image content before packing.

Typical flow:

1. Extract source textures
2. Group them by `metadata.exact_id`
3. Choose one representative or replacement image per logical ID
4. Replace some low-resolution content with higher-resolution art
5. Pack the selected images
6. Use the returned UVs in downstream tools

This keeps the logical mapping stable while allowing image dimensions to change.

## Practical Guidance

Pack trimmed images when:

- you want compact atlases
- you are deduplicating by exact ID
- you will regenerate UVs for downstream consumers

Pack cropped images when:

- you want to preserve original transparent margins
- an external consumer expects those margins to remain part of the asset

Use replacement images when:

- you need a new visual source for an existing logical texture entry

## Limitations

- no rotation support in v1
- no bin-packing optimization beyond deterministic shelves
- zero-sized images are rejected
- atlas dimensions must be large enough for the image plus padding

These limits are deliberate for a first production-quality baseline that favors predictability over opaque heuristics.
