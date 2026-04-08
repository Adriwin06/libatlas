#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from collections import defaultdict
from collections import deque
from pathlib import Path

from PIL import Image


SUPPORTED_EXTENSIONS = {".dds", ".tga"}


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]

    parser = argparse.ArgumentParser(
        description=(
            "Convert DDS/TGA test fixtures to PNG, derive UVs from visible alpha bounds, "
            "run libatlas extraction, and repack the trimmed outputs."
        )
    )
    parser.add_argument(
        "--input-dir",
        type=Path,
        default=repo_root / "tests" / "images_files",
        help="Directory containing DDS/TGA fixture atlases.",
    )
    parser.add_argument(
        "--work-dir",
        type=Path,
        default=repo_root / "build" / "fixture_pipeline",
        help="Directory where converted PNGs, metadata, and packed atlases are written.",
    )
    parser.add_argument(
        "--tool",
        type=Path,
        default=None,
        help="Path to libatlas_tool. If omitted, the script searches the build tree.",
    )
    parser.add_argument(
        "--config",
        default="Debug",
        help="Build configuration used when auto-detecting libatlas_tool.",
    )
    parser.add_argument(
        "--asset-store",
        type=Path,
        default=None,
        help=(
            "Optional persistent asset-store directory passed to libatlas_tool extract. "
            "Unlike --work-dir, this directory is not deleted between runs."
        ),
    )
    parser.add_argument(
        "--max-width",
        type=int,
        default=1024,
        help="Maximum output atlas width for the pack step.",
    )
    parser.add_argument(
        "--max-height",
        type=int,
        default=1024,
        help="Maximum output atlas height for the pack step.",
    )
    parser.add_argument(
        "--padding",
        type=int,
        default=2,
        help="Padding applied around packed textures.",
    )
    parser.add_argument(
        "--split-mode",
        choices=("components", "auto", "bbox"),
        default="components",
        help=(
            "How to derive extraction requests from each source image. "
            "'components' extracts every qualifying disconnected alpha island, "
            "'auto' only splits simple multi-sprite sheets, and 'bbox' extracts one visible-bounds crop."
        ),
    )
    parser.add_argument(
        "--component-alpha-threshold",
        type=int,
        default=1,
        help=(
            "Alpha threshold used when detecting disconnected sprite components. "
            "Pixels with alpha at or below this value are treated as transparent for splitting."
        ),
    )
    parser.add_argument(
        "--min-component-pixels",
        type=int,
        default=16,
        help="Ignore detected components smaller than this many pixels.",
    )
    parser.add_argument(
        "--split-components-max-count",
        type=int,
        default=8,
        help="Only split a source atlas when the filtered component count is at or below this limit.",
    )
    parser.add_argument(
        "--split-components-max-size-ratio",
        type=float,
        default=2.0,
        help=(
            "Only split a source atlas when detected component widths and heights stay within this ratio. "
            "This avoids shredding text-heavy atlases into one item per glyph."
        ),
    )
    return parser.parse_args()


def detect_tool_path(repo_root: Path, config: str) -> Path:
    candidates = [
        repo_root / "build" / "tools" / config / "libatlas_tool.exe",
        repo_root / "build" / "tools" / "libatlas_tool.exe",
        repo_root / "build" / "tools" / config / "libatlas_tool",
        repo_root / "build" / "tools" / "libatlas_tool",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    joined = "\n".join(f"  - {candidate}" for candidate in candidates)
    raise FileNotFoundError(
        "Could not find libatlas_tool. Checked:\n"
        f"{joined}\n"
        "Build it first with: cmake --build build --config "
        f"{config} --target libatlas_tool"
    )


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2), encoding="utf-8")


def run_command(arguments: list[str]) -> None:
    subprocess.run(arguments, check=True)


def sanitize_name(text: str) -> str:
    sanitized = []
    for character in text:
        if character.isalnum() or character in {"-", "_"}:
            sanitized.append(character)
        else:
            sanitized.append("_")
    return "".join(sanitized) or "item"


def make_uv_rect(bbox: tuple[int, int, int, int], width: int, height: int) -> dict[str, float]:
    left, top, right, bottom = bbox
    return {
        "x_min": left / width,
        "x_max": right / width,
        "y_min": top / height,
        "y_max": bottom / height,
    }


def load_rgba(path: Path) -> Image.Image:
    with Image.open(path) as source:
        source.load()
        return source.convert("RGBA")


def detect_components(
    rgba: Image.Image,
    alpha_threshold: int,
    min_component_pixels: int,
) -> list[dict[str, int | tuple[int, int, int, int]]]:
    width, height = rgba.size
    alpha = list(rgba.getchannel("A").getdata())
    visited = [False] * (width * height)
    components: list[dict[str, int | tuple[int, int, int, int]]] = []

    for y in range(height):
        for x in range(width):
            index = y * width + x
            if visited[index] or alpha[index] <= alpha_threshold:
                continue

            visited[index] = True
            queue: deque[tuple[int, int]] = deque([(x, y)])
            min_x = max_x = x
            min_y = max_y = y
            pixel_count = 0

            while queue:
                current_x, current_y = queue.popleft()
                pixel_count += 1
                min_x = min(min_x, current_x)
                max_x = max(max_x, current_x)
                min_y = min(min_y, current_y)
                max_y = max(max_y, current_y)

                for neighbor_y in range(max(0, current_y - 1), min(height, current_y + 2)):
                    for neighbor_x in range(max(0, current_x - 1), min(width, current_x + 2)):
                        neighbor_index = neighbor_y * width + neighbor_x
                        if visited[neighbor_index] or alpha[neighbor_index] <= alpha_threshold:
                            continue
                        visited[neighbor_index] = True
                        queue.append((neighbor_x, neighbor_y))

            if pixel_count < min_component_pixels:
                continue

            components.append(
                {
                    "bbox": (min_x, min_y, max_x + 1, max_y + 1),
                    "pixel_count": pixel_count,
                    "width": (max_x + 1) - min_x,
                    "height": (max_y + 1) - min_y,
                }
            )

    components.sort(key=lambda component: (component["bbox"][1], component["bbox"][0]))  # type: ignore[index]
    return components


def should_split_components(
    components: list[dict[str, int | tuple[int, int, int, int]]],
    max_count: int,
    max_size_ratio: float,
) -> bool:
    if len(components) <= 1 or len(components) > max_count:
        return False

    widths = [int(component["width"]) for component in components]
    heights = [int(component["height"]) for component in components]
    return (
        max(widths) / min(widths) <= max_size_ratio
        and max(heights) / min(heights) <= max_size_ratio
    )


def choose_components(
    split_mode: str,
    components: list[dict[str, int | tuple[int, int, int, int]]],
    max_count: int,
    max_size_ratio: float,
) -> list[dict[str, int | tuple[int, int, int, int]]]:
    if split_mode == "bbox":
        return []
    if split_mode == "components":
        return components if len(components) > 1 else []
    if split_mode == "auto":
        return (
            components
            if should_split_components(
                components,
                max_count=max_count,
                max_size_ratio=max_size_ratio,
            )
            else []
        )
    raise ValueError(f"unsupported split mode: {split_mode}")


def dedupe_identical_extract_outputs(item: dict[str, object]) -> bool:
    cropped_image = item.get("cropped_image")
    trimmed_image = item.get("trimmed_image")
    metadata = item.get("metadata")

    if not isinstance(cropped_image, str) or not isinstance(trimmed_image, str):
        return False
    if not isinstance(metadata, dict):
        return False

    cropped_width = metadata.get("cropped_width")
    cropped_height = metadata.get("cropped_height")
    trimmed_width = metadata.get("trimmed_width")
    trimmed_height = metadata.get("trimmed_height")
    trimmed_rect_in_crop = metadata.get("trimmed_rect_in_crop")

    if (
        cropped_width != trimmed_width
        or cropped_height != trimmed_height
        or not isinstance(trimmed_rect_in_crop, dict)
        or trimmed_rect_in_crop.get("x") != 0
        or trimmed_rect_in_crop.get("y") != 0
        or trimmed_rect_in_crop.get("width") != cropped_width
        or trimmed_rect_in_crop.get("height") != cropped_height
    ):
        return False

    cropped_path = Path(cropped_image)
    trimmed_path = Path(trimmed_image)
    if cropped_path == trimmed_path or not cropped_path.exists() or not trimmed_path.exists():
        return False
    if cropped_path.read_bytes() != trimmed_path.read_bytes():
        return False

    trimmed_path.unlink()
    item["trimmed_image"] = cropped_path.as_posix()
    return True


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parents[1]
    input_dir = args.input_dir.resolve()
    work_dir = args.work_dir.resolve()
    tool_path = args.tool.resolve() if args.tool else detect_tool_path(repo_root, args.config)
    asset_store_dir = args.asset_store.resolve() if args.asset_store else None

    if not input_dir.exists():
        raise FileNotFoundError(f"Input directory does not exist: {input_dir}")

    sources = sorted(
        path for path in input_dir.iterdir() if path.is_file() and path.suffix.lower() in SUPPORTED_EXTENSIONS
    )
    if not sources:
        raise FileNotFoundError(f"No DDS/TGA fixtures found in {input_dir}")

    if work_dir.exists():
        shutil.rmtree(work_dir)

    converted_dir = work_dir / "converted_png"
    request_dir = work_dir / "requests"
    extract_root = work_dir / "extract"
    extract_metadata_dir = work_dir / "metadata" / "extract"
    pack_dir = work_dir / "packed"
    pack_manifest_path = work_dir / "metadata" / "pack_manifest.json"
    pack_metadata_path = work_dir / "metadata" / "packed.json"
    summary_path = work_dir / "metadata" / "summary.json"

    for directory in (
        converted_dir,
        request_dir,
        extract_root,
        extract_metadata_dir,
        pack_dir,
        summary_path.parent,
    ):
        directory.mkdir(parents=True, exist_ok=True)

    summary_items: list[dict[str, object]] = []
    pack_items: list[dict[str, str]] = []
    duplicate_groups: defaultdict[str, list[str]] = defaultdict(list)
    total_extractions = 0
    deduplicated_outputs = 0

    for source in sources:
        atlas_name = source.stem
        rgba = load_rgba(source)
        alpha_bbox = rgba.getchannel("A").getbbox()
        if alpha_bbox is None:
            alpha_bbox = (0, 0, rgba.width, rgba.height)

        detected_components = detect_components(
            rgba,
            alpha_threshold=args.component_alpha_threshold,
            min_component_pixels=args.min_component_pixels,
        )
        selected_components = choose_components(
            split_mode=args.split_mode,
            components=detected_components,
            max_count=args.split_components_max_count,
            max_size_ratio=args.split_components_max_size_ratio,
        )

        converted_path = converted_dir / f"{atlas_name}.png"
        rgba.save(converted_path)

        request_path = request_dir / f"{atlas_name}.json"
        extract_output_dir = extract_root / atlas_name
        extract_metadata_path = extract_metadata_dir / f"{atlas_name}.json"
        request_items: list[dict[str, object]] = []

        if selected_components:
            for component_index, component in enumerate(selected_components):
                component_bbox = component["bbox"]
                request_items.append(
                    {
                        "name": f"{atlas_name}_{component_index:02d}",
                        "uv": make_uv_rect(component_bbox, rgba.width, rgba.height),
                    }
                )
        else:
            request_items.append(
                {
                    "name": atlas_name,
                    "uv": make_uv_rect(alpha_bbox, rgba.width, rgba.height),
                }
            )

        write_json(
            request_path,
            {
                "atlas_identifier": atlas_name,
                "items": request_items,
            },
        )

        run_command(
            [
                str(tool_path),
                "extract",
                "--atlas",
                str(converted_path),
                "--requests",
                str(request_path),
                "--output-dir",
                str(extract_output_dir),
                "--metadata",
                str(extract_metadata_path),
                "--origin",
                "top-left",
                "--rounding",
                "nearest",
            ]
            + (["--asset-store", str(asset_store_dir)] if asset_store_dir else [])
        )

        extract_metadata = json.loads(extract_metadata_path.read_text(encoding="utf-8"))
        metadata_changed = False
        for item in extract_metadata["items"]:
            if dedupe_identical_extract_outputs(item):
                deduplicated_outputs += 1
                metadata_changed = True
        if metadata_changed:
            write_json(extract_metadata_path, extract_metadata)

        extracted_items: list[dict[str, object]] = []
        for item in extract_metadata["items"]:
            metadata = item["metadata"]
            exact_id = metadata["exact_id"]
            item_name = item["name"]
            duplicate_groups[exact_id].append(item_name)

            trimmed_image = item.get("trimmed_image")
            if trimmed_image and metadata["trimmed_width"] > 0 and metadata["trimmed_height"] > 0:
                pack_items.append(
                    {
                        "entry_id": sanitize_name(item_name),
                        "image": trimmed_image,
                        "source_label": item_name,
                    }
                )

            extracted_items.append(
                {
                    "name": item_name,
                    "uv": metadata["requested_uv_rect"],
                    "cropped_image": item.get("cropped_image"),
                    "trimmed_image": trimmed_image,
                    "cropped_size": {
                        "width": metadata["cropped_width"],
                        "height": metadata["cropped_height"],
                    },
                    "trimmed_size": {
                        "width": metadata["trimmed_width"],
                        "height": metadata["trimmed_height"],
                    },
                    "trimmed_matches_cropped": item.get("trimmed_image") == item.get("cropped_image"),
                    "exact_id": exact_id,
                    "warnings": metadata["warnings"],
                }
            )
            total_extractions += 1

        summary_items.append(
            {
                "source_image": str(source),
                "converted_png": str(converted_path),
                "request_json": str(request_path),
                "extract_metadata_json": str(extract_metadata_path),
                "width": rgba.width,
                "height": rgba.height,
                "alpha_bbox": {
                    "left": alpha_bbox[0],
                    "top": alpha_bbox[1],
                    "right": alpha_bbox[2],
                    "bottom": alpha_bbox[3],
                    "width": alpha_bbox[2] - alpha_bbox[0],
                    "height": alpha_bbox[3] - alpha_bbox[1],
                },
                "detected_component_count": len(detected_components),
                "split_mode": args.split_mode,
                "extraction_strategy": "components" if selected_components else "bbox",
                "items": extracted_items,
            }
        )

    write_json(pack_manifest_path, {"items": pack_items})

    run_command(
        [
            str(tool_path),
            "pack",
            "--manifest",
            str(pack_manifest_path),
            "--output-dir",
            str(pack_dir),
            "--metadata",
            str(pack_metadata_path),
            "--atlas-prefix",
            "fixtures",
            "--max-width",
            str(args.max_width),
            "--max-height",
            str(args.max_height),
            "--padding",
            str(args.padding),
            "--origin",
            "top-left",
        ]
    )

    duplicate_summary = [
        {"exact_id": exact_id, "fixtures": fixtures}
        for exact_id, fixtures in sorted(duplicate_groups.items())
        if len(fixtures) > 1
    ]

    pack_metadata = json.loads(pack_metadata_path.read_text(encoding="utf-8"))
    summary = {
        "tool": str(tool_path),
        "input_dir": str(input_dir),
        "work_dir": str(work_dir),
        "asset_store_dir": str(asset_store_dir) if asset_store_dir else None,
        "source_count": len(summary_items),
        "extraction_count": total_extractions,
        "pack_item_count": len(pack_items),
        "deduplicated_identical_output_count": deduplicated_outputs,
        "packed_atlas_count": len(pack_metadata["atlases"]),
        "duplicate_exact_ids": duplicate_summary,
        "fixtures": summary_items,
        "pack_manifest_json": str(pack_manifest_path),
        "pack_metadata_json": str(pack_metadata_path),
    }
    write_json(summary_path, summary)

    print(f"Processed {len(summary_items)} source images into {total_extractions} extracted items.")
    print(f"Collapsed {deduplicated_outputs} identical cropped/trimmed pairs.")
    print(f"Packed {len(pack_items)} trimmed images into {len(pack_metadata['atlases'])} atlas(es).")
    print(f"Summary: {summary_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        print(f"Command failed with exit code {error.returncode}: {error.cmd}", file=sys.stderr)
        raise SystemExit(error.returncode)
    except Exception as error:  # pragma: no cover - CLI failure path
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
