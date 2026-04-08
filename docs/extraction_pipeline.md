# Extraction Pipeline

## Purpose

The extraction pipeline turns:

- an atlas bitmap
- a UV rectangle
- explicit policy choices

into:

- a cropped image
- an optionally trimmed image
- deterministic identity data
- near-duplicate signature data
- metadata that records how the result was produced

The library does not fetch UVs from any parser. A caller supplies them directly.

## Inputs

Core inputs:

- `Image atlas`
- `UvRect uv_rect`
- `ExtractionOptions`

Important policies in `ExtractionOptions`:

- `uv_origin`
  - `TopLeft`
  - `BottomLeft`
- `rounding_policy`
  - `Expand`
  - `Nearest`
  - `Contract`
- `trim_transparent_borders`
- `transparent_alpha_threshold`
- `source_atlas_identifier`

## UV Resolution

`libatlas` treats UVs as normalized atlas-space bounds. It converts them into half-open pixel rectangles.

Horizontal conversion:

- `left = round(x_min * atlas_width)`
- `right = round(x_max * atlas_width)`

Vertical conversion depends on origin:

- `TopLeft`
  - `top = round(y_min * atlas_height)`
  - `bottom = round(y_max * atlas_height)`
- `BottomLeft`
  - `top = round((1 - y_max) * atlas_height)`
  - `bottom = round((1 - y_min) * atlas_height)`

If the input UV bounds are reversed, the library swaps them and emits warnings.

## Rounding Policies

`Expand`:

- min bounds use `floor`
- max bounds use `ceil`
- useful when recovery work should prefer not to under-crop

`Nearest`:

- both bounds use nearest-integer rounding
- useful when the caller expects UVs to already match intended pixel edges

`Contract`:

- min bounds use `ceil`
- max bounds use `floor`
- useful when the caller wants a conservative inner crop

## Clamping

Resolved rectangles are clamped to atlas bounds. Clamping is visible in metadata and warnings. It is not folded away silently.

Warnings you should expect in real-world atlas work:

- `swapped_x_bounds`
- `swapped_y_bounds`
- `clamped_to_atlas_bounds`
- `degenerate_crop`
- `transparent_borders_trimmed`
- `trimmed_to_empty`

## Crop And Trim

After UV resolution:

1. The atlas is cropped to the clamped rectangle
2. If trimming is enabled, fully transparent outer rows and columns are removed
3. Both the original crop and the trimmed result are preserved in the `ExtractedTexture`

Why both images are kept:

- `cropped_image` preserves the exact resolved atlas crop
- `trimmed_image` is often the better unit for identity and deduplication

If trimming removes everything, the trimmed image becomes `0 x 0`. That is deliberate and visible.

## Metadata Model

`ExtractionMetadata` stores:

- source atlas identifier
- source atlas dimensions
- original requested UV rectangle
- resolved pixel rectangle before clamp
- clamped crop rectangle
- trimmed rectangle in atlas coordinates
- trimmed rectangle relative to the crop
- cropped and trimmed sizes
- alpha coverage ratios
- exact ID
- similarity signature
- warnings

This makes it possible to recover how the extracted result relates to the original atlas data, even when trimming changed the content bounds.

## Identity And Similarity

After extraction:

- exact identity is computed from canonicalized pixel content
- similarity is computed from lightweight signatures

By default the canonical content used for identity is the trimmed result, not the untrimmed crop. This means transparent border drift does not create different logical IDs.

Near-duplicate matching is intentionally advisory:

- exact match means canonical SHA-256 equality
- likely-related means the signatures and score suggest similar visual content
- likely-related does not mean safe automatic deduplication

## Edge Cases

Fully transparent crops:

- can trim to `0 x 0`
- produce a deterministic exact ID for empty canonical content
- are not packable until the caller supplies non-empty content

Non-alpha formats:

- can still be cropped, hashed, compared, and packed
- transparent trimming does not remove borders from data that has no alpha channel

One-pixel UV drift:

- can change the raw crop
- often still collapses to the same exact ID when the difference is transparent padding
- otherwise tends to remain a likely-related candidate via similarity data

## Caller Guidance

Use `cropped_image` when you need to inspect the exact atlas extraction.

Use `trimmed_image` and `metadata.exact_id` when you want stable identity and cleaner deduplication.

Keep the warnings. In messy atlas datasets they are useful diagnostics, not noise.
