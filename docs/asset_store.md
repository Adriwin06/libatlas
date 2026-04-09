# Asset Store

## Purpose

The persistent asset store is implemented in the tool layer, not the core library.

Its job is to keep a long-lived, deduplicated record of:

- source atlases
- raw extracted crops
- canonical logical textures
- occurrence metadata that maps source requests back to those IDs

This lets later runs reuse prior work instead of rediscovering the same textures from scratch.

## Why It Is Not In The Core Library

`libatlas` is intentionally stateless. That keeps the core reusable and easy to embed in other tools.

Persistence is a policy choice:

- where files live
- how long they are kept
- how provenance is tracked
- whether the store is local, shared, versioned, or disposable

Those decisions belong in the calling program.

## Directory Layout

`libatlas_tool extract --asset-store <dir>` uses a content-addressed layout:

- `atlases/`
  - source atlas PNGs keyed by atlas content ID
- `cropped/`
  - unique raw crops keyed by `cropped_exact_id`
- `canonical/`
  - unique logical textures keyed by `exact_id`
- `metadata/atlases/`
  - atlas metadata records
- `metadata/cropped/`
  - raw crop alias metadata
- `metadata/canonical/`
  - canonical texture metadata
- `metadata/occurrences/`
  - per-occurrence provenance records

## Identity Model

The store keeps two separate identities:

- `cropped_exact_id`
  - exact hash of the raw extracted crop
  - used for the first-pass cache hit
- `exact_id`
  - exact hash of the resolved image after the configured trim policy
  - used as the logical texture identity

This separation matters because several different raw crops can still converge on the same logical texture after transparent-border trimming.

## Reuse Across Runs

When the tool starts with `--asset-store`, it preloads cached identities from `metadata/cropped/` into `ExtractionIdentityCache`.

That gives the same two-pass behavior across runs:

1. Compute `cropped_exact_id`
2. If the raw crop already exists for the same trim policy, reuse the cached resolved image and `exact_id`
3. Otherwise trim once, compute `exact_id`, and add the new alias and canonical texture to the store

## What Gets Deduplicated

The store deduplicates:

- atlases by atlas content ID
- raw crops by `cropped_exact_id`
- canonical textures by `exact_id`

The store does not collapse occurrence records. Many occurrences can point at one canonical texture.

That is important because you usually still need provenance:

- which atlas it came from
- which geometry or request referenced it
- which UVs produced it

The store also does not merge near-duplicate textures. Similar-but-non-exact candidates remain
separate canonical entries until a higher-level workflow chooses a winner.

## Typical Workflow

1. Run `libatlas_tool extract --asset-store <dir> ...`
2. Let the tool populate or reuse the store
3. Optionally generate similarity reports for non-exact candidates
4. Build or update a logical store that maps reviewed near-duplicates onto one editable image
5. Use `libatlas` workflow helpers or your own tool logic to repack logical textures
6. Remap original occurrences back to packed atlas placements

The key boundary is:

- asset store
  - exact, content-addressed cache
- logical store
  - human-reviewed grouping layer used for replacement and repacking

## Limitations

- the store is file-based, not a relational database
- metadata is JSON and optimized for transparency, not extreme scale
- near-duplicate matching is still advisory; the store only auto-deduplicates exact IDs
- the current CLI only writes the store during extraction; logical grouping and review policy remain caller-driven
