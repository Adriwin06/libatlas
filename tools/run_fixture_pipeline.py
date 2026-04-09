#!/usr/bin/env python3

from __future__ import annotations

import argparse
import concurrent.futures
import json
import logging
import os
import shutil
import subprocess
import sys
import time
from collections import defaultdict
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from PIL import Image


LOGGER = logging.getLogger("fixture_pipeline")
SUPPORTED_EXTENSIONS = {".dds", ".tga"}


@dataclass(frozen=True)
class PipelineContext:
    tool_path: Path
    asset_store_dir: Path | None
    converted_dir: Path
    request_dir: Path
    extract_root: Path
    extract_metadata_dir: Path
    split_mode: str
    component_alpha_threshold: int
    min_component_pixels: int
    split_components_max_count: int
    split_components_max_size_ratio: float
    command_retries: int
    retry_delay_seconds: float


@dataclass(frozen=True)
class SourceResult:
    source_index: int
    summary_item: dict[str, object]
    pack_items: list[dict[str, str]]
    duplicate_entries: list[tuple[str, str]]
    extraction_count: int
    deduplicated_outputs: int
    duration_seconds: float


@dataclass(frozen=True)
class OccurrenceRecord:
    source_image: str
    item_name: str
    exact_id: str
    trimmed_image: str | None


@dataclass(frozen=True)
class LogicalGroup:
    logical_id: str
    representative_exact_id: str
    representative_name: str
    representative_source_image: str
    representative_trimmed_image: str | None
    member_exact_ids: list[str]
    occurrence_count: int
    fixtures: list[str]
    source_images: list[str]
    merged_by_similarity: bool


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
        default=repo_root / "build" / "fixture_asset_store",
        help=(
            "Optional persistent asset-store directory passed to libatlas_tool extract. "
            "Unlike --work-dir, this directory is not deleted between runs."
        ),
    )
    parser.add_argument(
        "--logical-store",
        type=Path,
        default=repo_root / "build" / "fixture_logical_store",
        help=(
            "Persistent directory containing one editable image per logical texture group. "
            "This directory is not deleted between runs."
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
    parser.add_argument(
        "--jobs",
        type=int,
        default=None,
        help=(
            "Number of source atlases to process concurrently. "
            "Defaults to the CPU count capped to the number of discovered fixtures."
        ),
    )
    parser.add_argument(
        "--log-level",
        choices=("DEBUG", "INFO", "WARNING", "ERROR"),
        default="INFO",
        help="Minimum logging verbosity. INFO shows progress, DEBUG also logs subprocess details.",
    )
    parser.add_argument(
        "--command-retries",
        type=int,
        default=2,
        help="Retry failed extract/pack subprocesses this many times before aborting.",
    )
    parser.add_argument(
        "--retry-delay",
        type=float,
        default=0.5,
        help="Base delay in seconds before retrying a failed subprocess. Each retry uses delay * attempt.",
    )
    parser.add_argument(
        "--similarity-review-min-score",
        type=float,
        default=0.90,
        help=(
            "Minimum libatlas similarity score for a non-exact pair to be emitted as a "
            "manual-review candidate."
        ),
    )
    parser.add_argument(
        "--similarity-auto-min-score",
        type=float,
        default=0.92,
        help=(
            "Minimum libatlas similarity score for a non-exact pair to be emitted as a "
            "high-confidence duplicate candidate."
        ),
    )
    parser.add_argument(
        "--similarity-auto-max-luminance-distance",
        type=int,
        default=8,
        help="Maximum luminance hash Hamming distance for high-confidence duplicate candidates.",
    )
    parser.add_argument(
        "--similarity-auto-max-alpha-distance",
        type=int,
        default=8,
        help="Maximum alpha hash Hamming distance for high-confidence duplicate candidates.",
    )
    parser.add_argument(
        "--similarity-auto-max-aspect-ratio-delta",
        type=float,
        default=0.10,
        help="Maximum aspect-ratio delta for high-confidence duplicate candidates.",
    )
    parser.add_argument(
        "--similarity-auto-min-dimension-ratio",
        type=float,
        default=0.90,
        help="Minimum width/height ratio product for high-confidence duplicate candidates.",
    )
    parser.add_argument(
        "--similarity-report-max-pairs",
        type=int,
        default=500,
        help="Maximum pairs written per similarity bucket in the generated report.",
    )
    return parser.parse_args()


def configure_logging(level: str) -> None:
    logging.basicConfig(
        level=getattr(logging, level.upper()),
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%H:%M:%S",
        force=True,
    )


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


def format_duration(seconds: float) -> str:
    if seconds < 60.0:
        return f"{seconds:.2f}s"
    minutes, remainder = divmod(seconds, 60.0)
    if minutes < 60.0:
        return f"{int(minutes)}m{remainder:04.1f}s"
    hours, minutes = divmod(int(minutes), 60)
    return f"{hours}h{minutes:02d}m{remainder:04.1f}s"


def format_command(arguments: list[str]) -> str:
    return subprocess.list2cmdline(arguments)


def remove_paths(paths: Iterable[Path]) -> None:
    for path in paths:
        if path.is_dir():
            shutil.rmtree(path, ignore_errors=True)
        elif path.exists():
            path.unlink()


def run_command(
    arguments: list[str],
    *,
    description: str,
    retries: int = 0,
    retry_delay_seconds: float = 0.5,
    cleanup_paths: Iterable[Path] = (),
) -> None:
    total_attempts = retries + 1
    for attempt in range(1, total_attempts + 1):
        if attempt == 1:
            LOGGER.debug("Running %s: %s", description, format_command(arguments))
        else:
            LOGGER.info("Retrying %s (%d/%d).", description, attempt, total_attempts)

        started_at = time.perf_counter()
        try:
            completed = subprocess.run(arguments, check=True, capture_output=True, text=True)
        except subprocess.CalledProcessError as error:
            if error.stdout and error.stdout.strip():
                LOGGER.error("%s stdout:\n%s", description, error.stdout.rstrip())
            if error.stderr and error.stderr.strip():
                LOGGER.error("%s stderr:\n%s", description, error.stderr.rstrip())
            if attempt >= total_attempts:
                raise

            delay_seconds = retry_delay_seconds * attempt
            LOGGER.warning(
                "%s failed on attempt %d/%d with exit code %d; retrying in %.2fs.",
                description,
                attempt,
                total_attempts,
                error.returncode,
                delay_seconds,
            )
            remove_paths(cleanup_paths)
            time.sleep(delay_seconds)
            continue

        if completed.stdout and completed.stdout.strip():
            LOGGER.debug("%s stdout:\n%s", description, completed.stdout.rstrip())
        if completed.stderr and completed.stderr.strip():
            LOGGER.debug("%s stderr:\n%s", description, completed.stderr.rstrip())
        LOGGER.debug("%s finished in %s", description, format_duration(time.perf_counter() - started_at))
        if attempt > 1:
            LOGGER.info("%s succeeded on retry %d/%d.", description, attempt, total_attempts)
        return


def resolve_job_count(requested_jobs: int | None, source_count: int) -> int:
    if requested_jobs is not None and requested_jobs < 1:
        raise ValueError("--jobs must be at least 1")

    effective_jobs = requested_jobs if requested_jobs is not None else (os.cpu_count() or 1)
    return max(1, min(effective_jobs, source_count))


def sanitize_name(text: str) -> str:
    sanitized = []
    for character in text:
        if character.isalnum() or character in {"-", "_"}:
            sanitized.append(character)
        else:
            sanitized.append("_")
    return "".join(sanitized) or "item"


def sorted_unique_strings(values: Iterable[str]) -> list[str]:
    return sorted(set(values))


def logical_id_from_exact_ids(exact_ids: list[str]) -> str:
    return sanitize_name(min(exact_ids))


def collect_occurrences(summary_items: list[dict[str, object]]) -> list[OccurrenceRecord]:
    occurrences: list[OccurrenceRecord] = []
    for fixture in summary_items:
        source_image = fixture.get("source_image")
        items = fixture.get("items")
        if not isinstance(source_image, str) or not isinstance(items, list):
            continue

        for item in items:
            if not isinstance(item, dict):
                continue
            item_name = item.get("name")
            exact_id = item.get("exact_id")
            trimmed_image = item.get("trimmed_image")
            if not isinstance(item_name, str) or not isinstance(exact_id, str):
                continue
            occurrences.append(
                OccurrenceRecord(
                    source_image=source_image,
                    item_name=item_name,
                    exact_id=exact_id,
                    trimmed_image=trimmed_image if isinstance(trimmed_image, str) else None,
                )
            )
    return occurrences


def build_logical_groups(
    occurrences: list[OccurrenceRecord],
    similarity_report: dict[str, object],
) -> list[LogicalGroup]:
    occurrences_by_exact_id: defaultdict[str, list[OccurrenceRecord]] = defaultdict(list)
    for occurrence in occurrences:
        occurrences_by_exact_id[occurrence.exact_id].append(occurrence)

    assigned_exact_ids: set[str] = set()
    logical_groups: list[LogicalGroup] = []
    auto_components = similarity_report.get("auto_duplicate_components")
    if isinstance(auto_components, list):
        for component in auto_components:
            if not isinstance(component, dict):
                continue
            member_exact_ids = component.get("member_exact_ids")
            if not isinstance(member_exact_ids, list):
                continue
            exact_ids = sorted(
                exact_id
                for exact_id in member_exact_ids
                if isinstance(exact_id, str) and exact_id in occurrences_by_exact_id
            )
            if len(exact_ids) < 2:
                continue
            for exact_id in exact_ids:
                assigned_exact_ids.add(exact_id)

            representative_exact_id = min(
                exact_ids,
                key=lambda exact_id: (-len(occurrences_by_exact_id[exact_id]), exact_id),
            )
            representative_occurrence = min(
                occurrences_by_exact_id[representative_exact_id],
                key=lambda occurrence: occurrence.item_name,
            )
            member_occurrences = [occurrence for exact_id in exact_ids for occurrence in occurrences_by_exact_id[exact_id]]
            logical_groups.append(
                LogicalGroup(
                    logical_id=logical_id_from_exact_ids(exact_ids),
                    representative_exact_id=representative_exact_id,
                    representative_name=representative_occurrence.item_name,
                    representative_source_image=representative_occurrence.source_image,
                    representative_trimmed_image=representative_occurrence.trimmed_image,
                    member_exact_ids=exact_ids,
                    occurrence_count=len(member_occurrences),
                    fixtures=sorted_unique_strings(occurrence.item_name for occurrence in member_occurrences),
                    source_images=sorted_unique_strings(occurrence.source_image for occurrence in member_occurrences),
                    merged_by_similarity=True,
                )
            )

    for exact_id in sorted(occurrences_by_exact_id):
        if exact_id in assigned_exact_ids:
            continue
        exact_occurrences = occurrences_by_exact_id[exact_id]
        representative_occurrence = min(exact_occurrences, key=lambda occurrence: occurrence.item_name)
        logical_groups.append(
            LogicalGroup(
                logical_id=logical_id_from_exact_ids([exact_id]),
                representative_exact_id=exact_id,
                representative_name=representative_occurrence.item_name,
                representative_source_image=representative_occurrence.source_image,
                representative_trimmed_image=representative_occurrence.trimmed_image,
                member_exact_ids=[exact_id],
                occurrence_count=len(exact_occurrences),
                fixtures=sorted_unique_strings(occurrence.item_name for occurrence in exact_occurrences),
                source_images=sorted_unique_strings(occurrence.source_image for occurrence in exact_occurrences),
                merged_by_similarity=False,
            )
        )

    logical_groups.sort(key=lambda group: group.logical_id)
    return logical_groups


def materialize_logical_store(
    logical_groups: list[LogicalGroup],
    logical_store_dir: Path,
) -> tuple[list[dict[str, str]], list[dict[str, object]]]:
    images_dir = logical_store_dir / "images"
    metadata_dir = logical_store_dir / "metadata"
    images_dir.mkdir(parents=True, exist_ok=True)
    metadata_dir.mkdir(parents=True, exist_ok=True)

    pack_items: list[dict[str, str]] = []
    metadata_items: list[dict[str, object]] = []
    expected_filenames = {f"{group.logical_id}.png" for group in logical_groups}
    for existing_image in images_dir.glob("*.png"):
        if existing_image.name not in expected_filenames:
            existing_image.unlink()

    for group in logical_groups:
        image_path = images_dir / f"{group.logical_id}.png"
        existed_before_run = image_path.exists()
        if not existed_before_run and group.representative_trimmed_image:
            shutil.copy2(group.representative_trimmed_image, image_path)

        if image_path.exists():
            pack_items.append(
                {
                    "entry_id": group.logical_id,
                    "image": str(image_path),
                    "source_label": group.representative_name,
                }
            )

        metadata_items.append(
            {
                "logical_id": group.logical_id,
                "representative_exact_id": group.representative_exact_id,
                "representative_name": group.representative_name,
                "representative_source_image": group.representative_source_image,
                "representative_trimmed_image": group.representative_trimmed_image,
                "logical_image": str(image_path) if image_path.exists() else None,
                "logical_image_existed_before_run": existed_before_run,
                "member_exact_ids": group.member_exact_ids,
                "member_exact_id_count": len(group.member_exact_ids),
                "occurrence_count": group.occurrence_count,
                "fixtures": group.fixtures,
                "source_images": group.source_images,
                "merged_by_similarity": group.merged_by_similarity,
            }
        )

    return pack_items, metadata_items


def build_occurrence_remap(
    occurrences: list[OccurrenceRecord],
    logical_groups: list[LogicalGroup],
    pack_metadata: dict[str, object],
) -> list[dict[str, object]]:
    logical_id_by_exact_id: dict[str, str] = {}
    for group in logical_groups:
        for exact_id in group.member_exact_ids:
            logical_id_by_exact_id[exact_id] = group.logical_id

    atlases = pack_metadata.get("atlases")
    placements = pack_metadata.get("placements")
    atlas_identifier_by_index: dict[int, str] = {}
    if isinstance(atlases, list):
        for index, atlas in enumerate(atlases):
            if isinstance(atlas, dict) and isinstance(atlas.get("atlas_identifier"), str):
                atlas_identifier_by_index[index] = atlas["atlas_identifier"]

    placement_by_entry_id: dict[str, dict[str, object]] = {}
    if isinstance(placements, list):
        for placement in placements:
            if isinstance(placement, dict) and isinstance(placement.get("entry_id"), str):
                placement_by_entry_id[placement["entry_id"]] = placement

    remap_items: list[dict[str, object]] = []
    for occurrence in occurrences:
        logical_id = logical_id_by_exact_id.get(occurrence.exact_id)
        if logical_id is None:
            continue
        placement = placement_by_entry_id.get(logical_id)
        if placement is None:
            continue
        atlas_index = placement.get("atlas_index")
        remap_items.append(
            {
                "source_image": occurrence.source_image,
                "occurrence_name": occurrence.item_name,
                "exact_id": occurrence.exact_id,
                "logical_id": logical_id,
                "atlas_index": atlas_index,
                "atlas_identifier": atlas_identifier_by_index.get(int(atlas_index)) if isinstance(atlas_index, (int, float)) else None,
                "pixel_rect": placement.get("pixel_rect"),
                "uv_rect": placement.get("uv_rect"),
            }
        )
    remap_items.sort(key=lambda item: (str(item["source_image"]), str(item["occurrence_name"])))
    return remap_items


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


def build_request_items(
    atlas_name: str,
    width: int,
    height: int,
    alpha_bbox: tuple[int, int, int, int],
    selected_components: list[dict[str, int | tuple[int, int, int, int]]],
) -> list[dict[str, object]]:
    request_items: list[dict[str, object]] = []
    if selected_components:
        for component_index, component in enumerate(selected_components):
            component_bbox = component["bbox"]
            request_items.append(
                {
                    "name": f"{atlas_name}_{component_index:02d}",
                    "uv": make_uv_rect(component_bbox, width, height),
                }
            )
        return request_items

    request_items.append(
        {
            "name": atlas_name,
            "uv": make_uv_rect(alpha_bbox, width, height),
        }
    )
    return request_items


def process_source(
    source_index: int,
    total_sources: int,
    source: Path,
    context: PipelineContext,
) -> SourceResult:
    atlas_name = source.stem
    log_prefix = f"[{source_index + 1}/{total_sources}] {atlas_name}"
    source_started_at = time.perf_counter()

    analysis_started_at = time.perf_counter()
    rgba = load_rgba(source)
    alpha_bbox = rgba.getchannel("A").getbbox()
    if alpha_bbox is None:
        alpha_bbox = (0, 0, rgba.width, rgba.height)

    detected_components = detect_components(
        rgba,
        alpha_threshold=context.component_alpha_threshold,
        min_component_pixels=context.min_component_pixels,
    )
    selected_components = choose_components(
        split_mode=context.split_mode,
        components=detected_components,
        max_count=context.split_components_max_count,
        max_size_ratio=context.split_components_max_size_ratio,
    )
    analysis_duration = time.perf_counter() - analysis_started_at

    converted_path = context.converted_dir / f"{atlas_name}.png"
    request_path = context.request_dir / f"{atlas_name}.json"
    extract_output_dir = context.extract_root / atlas_name
    extract_metadata_path = context.extract_metadata_dir / f"{atlas_name}.json"

    rgba.save(converted_path)
    request_items = build_request_items(
        atlas_name=atlas_name,
        width=rgba.width,
        height=rgba.height,
        alpha_bbox=alpha_bbox,
        selected_components=selected_components,
    )
    extraction_strategy = "components" if selected_components else "bbox"
    write_json(
        request_path,
        {
            "atlas_identifier": atlas_name,
            "items": request_items,
        },
    )

    LOGGER.info(
        "%s: extracting %d item(s) from %dx%d atlas after %s analysis "
        "(detected %d component(s), strategy=%s)",
        log_prefix,
        len(request_items),
        rgba.width,
        rgba.height,
        format_duration(analysis_duration),
        len(detected_components),
        extraction_strategy,
    )

    extract_started_at = time.perf_counter()
    run_command(
        [
            str(context.tool_path),
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
        + (["--asset-store", str(context.asset_store_dir)] if context.asset_store_dir else []),
        description=f"{atlas_name} extract",
        retries=context.command_retries,
        retry_delay_seconds=context.retry_delay_seconds,
        cleanup_paths=(extract_output_dir, extract_metadata_path),
    )
    extract_duration = time.perf_counter() - extract_started_at

    postprocess_started_at = time.perf_counter()
    extract_metadata = json.loads(extract_metadata_path.read_text(encoding="utf-8"))
    metadata_changed = False
    deduplicated_outputs = 0
    for item in extract_metadata["items"]:
        if dedupe_identical_extract_outputs(item):
            deduplicated_outputs += 1
            metadata_changed = True
    if metadata_changed:
        write_json(extract_metadata_path, extract_metadata)

    extracted_items: list[dict[str, object]] = []
    pack_items: list[dict[str, str]] = []
    duplicate_entries: list[tuple[str, str]] = []
    for item in extract_metadata["items"]:
        metadata = item["metadata"]
        exact_id = metadata["exact_id"]
        item_name = item["name"]
        duplicate_entries.append((exact_id, item_name))

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
    postprocess_duration = time.perf_counter() - postprocess_started_at

    total_duration = time.perf_counter() - source_started_at
    LOGGER.info(
        "%s: finished in %s (%d extracted, %d identical pairs collapsed, extract=%s)",
        log_prefix,
        format_duration(total_duration),
        len(extracted_items),
        deduplicated_outputs,
        format_duration(extract_duration),
    )

    return SourceResult(
        source_index=source_index,
        summary_item={
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
            "split_mode": context.split_mode,
            "extraction_strategy": extraction_strategy,
            "timing_seconds": {
                "analysis": analysis_duration,
                "extract": extract_duration,
                "postprocess": postprocess_duration,
                "total": total_duration,
            },
            "items": extracted_items,
        },
        pack_items=pack_items,
        duplicate_entries=duplicate_entries,
        extraction_count=len(extracted_items),
        deduplicated_outputs=deduplicated_outputs,
        duration_seconds=total_duration,
    )


def main() -> int:
    overall_started_at = time.perf_counter()
    args = parse_args()
    configure_logging(args.log_level)

    repo_root = Path(__file__).resolve().parents[1]
    input_dir = args.input_dir.resolve()
    work_dir = args.work_dir.resolve()
    tool_path = args.tool.resolve() if args.tool else detect_tool_path(repo_root, args.config)
    asset_store_dir = args.asset_store.resolve() if args.asset_store else None
    logical_store_dir = args.logical_store.resolve() if args.logical_store else None

    if not input_dir.exists():
        raise FileNotFoundError(f"Input directory does not exist: {input_dir}")
    if args.command_retries < 0:
        raise ValueError("--command-retries must be at least 0")
    if args.retry_delay < 0:
        raise ValueError("--retry-delay must be at least 0")
    if not 0.0 <= args.similarity_review_min_score <= 1.0:
        raise ValueError("--similarity-review-min-score must be between 0 and 1")
    if not 0.0 <= args.similarity_auto_min_score <= 1.0:
        raise ValueError("--similarity-auto-min-score must be between 0 and 1")
    if args.similarity_auto_min_score < args.similarity_review_min_score:
        raise ValueError("--similarity-auto-min-score must be at least --similarity-review-min-score")
    if args.similarity_auto_max_luminance_distance < 0:
        raise ValueError("--similarity-auto-max-luminance-distance must be at least 0")
    if args.similarity_auto_max_alpha_distance < 0:
        raise ValueError("--similarity-auto-max-alpha-distance must be at least 0")
    if args.similarity_auto_max_aspect_ratio_delta < 0.0:
        raise ValueError("--similarity-auto-max-aspect-ratio-delta must be at least 0")
    if not 0.0 <= args.similarity_auto_min_dimension_ratio <= 1.0:
        raise ValueError("--similarity-auto-min-dimension-ratio must be between 0 and 1")
    if args.similarity_report_max_pairs < 1:
        raise ValueError("--similarity-report-max-pairs must be at least 1")

    sources = sorted(
        path for path in input_dir.iterdir() if path.is_file() and path.suffix.lower() in SUPPORTED_EXTENSIONS
    )
    if not sources:
        raise FileNotFoundError(f"No DDS/TGA fixtures found in {input_dir}")

    worker_count = resolve_job_count(args.jobs, len(sources))

    LOGGER.info("Fixture pipeline starting with %d source image(s).", len(sources))
    LOGGER.info("Using %d worker thread(s).", worker_count)
    LOGGER.info("Input: %s", input_dir)
    LOGGER.info("Work directory: %s", work_dir)
    LOGGER.info("Tool: %s", tool_path)
    LOGGER.info("Asset store: %s", asset_store_dir if asset_store_dir else "(disabled)")
    LOGGER.info("Logical store: %s", logical_store_dir if logical_store_dir else "(disabled)")
    LOGGER.info(
        "Subprocess retries: %d (base delay %.2fs).",
        args.command_retries,
        args.retry_delay,
    )

    if work_dir.exists():
        LOGGER.info("Removing previous work directory: %s", work_dir)
        shutil.rmtree(work_dir)

    converted_dir = work_dir / "converted_png"
    request_dir = work_dir / "requests"
    extract_root = work_dir / "extract"
    extract_metadata_dir = work_dir / "metadata" / "extract"
    pack_dir = work_dir / "packed"
    pack_manifest_path = work_dir / "metadata" / "pack_manifest.json"
    pack_metadata_path = work_dir / "metadata" / "packed.json"
    occurrence_remap_path = work_dir / "metadata" / "occurrence_remap.json"
    similarity_source_map_path = work_dir / "metadata" / "similarity_source_map.json"
    similarity_report_path = work_dir / "metadata" / "similarity_report.json"
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

    context = PipelineContext(
        tool_path=tool_path,
        asset_store_dir=asset_store_dir,
        converted_dir=converted_dir,
        request_dir=request_dir,
        extract_root=extract_root,
        extract_metadata_dir=extract_metadata_dir,
        split_mode=args.split_mode,
        component_alpha_threshold=args.component_alpha_threshold,
        min_component_pixels=args.min_component_pixels,
        split_components_max_count=args.split_components_max_count,
        split_components_max_size_ratio=args.split_components_max_size_ratio,
        command_retries=args.command_retries,
        retry_delay_seconds=args.retry_delay,
    )

    source_results: dict[int, SourceResult] = {}
    source_stage_started_at = time.perf_counter()
    progress_interval = 1 if len(sources) <= 20 else 25
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=worker_count,
        thread_name_prefix="fixture",
    ) as executor:
        future_to_source = {
            executor.submit(process_source, index, len(sources), source, context): source
            for index, source in enumerate(sources)
        }
        for completed_count, future in enumerate(concurrent.futures.as_completed(future_to_source), start=1):
            result = future.result()
            source_results[result.source_index] = result
            if completed_count == len(sources) or completed_count % progress_interval == 0:
                LOGGER.info("Completed %d/%d source image(s).", completed_count, len(sources))
    source_stage_duration = time.perf_counter() - source_stage_started_at

    summary_items: list[dict[str, object]] = []
    raw_pack_items: list[dict[str, str]] = []
    duplicate_groups: defaultdict[str, list[str]] = defaultdict(list)
    total_extractions = 0
    deduplicated_outputs = 0

    for source_index in range(len(sources)):
        result = source_results[source_index]
        summary_items.append(result.summary_item)
        raw_pack_items.extend(result.pack_items)
        for exact_id, item_name in result.duplicate_entries:
            duplicate_groups[exact_id].append(item_name)
        total_extractions += result.extraction_count
        deduplicated_outputs += result.deduplicated_outputs

    write_json(similarity_source_map_path, {source.stem: str(source) for source in sources})
    run_command(
        [
            str(tool_path),
            "similarity-report",
            "--metadata-dir",
            str(extract_metadata_dir),
            "--output",
            str(similarity_report_path),
            "--source-map",
            str(similarity_source_map_path),
            "--review-min-score",
            str(args.similarity_review_min_score),
            "--auto-min-score",
            str(args.similarity_auto_min_score),
            "--auto-max-luminance-distance",
            str(args.similarity_auto_max_luminance_distance),
            "--auto-max-alpha-distance",
            str(args.similarity_auto_max_alpha_distance),
            "--auto-max-aspect-ratio-delta",
            str(args.similarity_auto_max_aspect_ratio_delta),
            "--auto-min-dimension-ratio",
            str(args.similarity_auto_min_dimension_ratio),
            "--max-pairs",
            str(args.similarity_report_max_pairs),
        ],
        description="build similarity report",
        retries=args.command_retries,
        retry_delay_seconds=args.retry_delay,
        cleanup_paths=(similarity_report_path,),
    )
    similarity_report = json.loads(similarity_report_path.read_text(encoding="utf-8"))

    effective_logical_store_dir = logical_store_dir if logical_store_dir is not None else (work_dir / "logical_store")
    occurrences = collect_occurrences(summary_items)
    logical_groups = build_logical_groups(occurrences, similarity_report)
    logical_pack_items, logical_group_metadata = materialize_logical_store(
        logical_groups,
        effective_logical_store_dir,
    )
    logical_group_metadata_path = effective_logical_store_dir / "metadata" / "logical_groups.json"
    write_json(logical_group_metadata_path, {"logical_groups": logical_group_metadata})

    write_json(pack_manifest_path, {"items": logical_pack_items})

    LOGGER.info(
        "Packing %d logical image(s) with max atlas size %dx%d and padding %d.",
        len(logical_pack_items),
        args.max_width,
        args.max_height,
        args.padding,
    )
    pack_started_at = time.perf_counter()
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
        ],
        description="pack logical images",
        retries=args.command_retries,
        retry_delay_seconds=args.retry_delay,
        cleanup_paths=(pack_dir, pack_metadata_path),
    )
    pack_duration = time.perf_counter() - pack_started_at

    duplicate_summary = [
        {"exact_id": exact_id, "fixtures": fixtures}
        for exact_id, fixtures in sorted(duplicate_groups.items())
        if len(fixtures) > 1
    ]

    pack_metadata = json.loads(pack_metadata_path.read_text(encoding="utf-8"))
    occurrence_remap = build_occurrence_remap(occurrences, logical_groups, pack_metadata)
    write_json(occurrence_remap_path, {"occurrences": occurrence_remap})
    total_duration = time.perf_counter() - overall_started_at
    summary = {
        "tool": str(tool_path),
        "input_dir": str(input_dir),
        "work_dir": str(work_dir),
        "asset_store_dir": str(asset_store_dir) if asset_store_dir else None,
        "logical_store_dir": str(effective_logical_store_dir),
        "worker_count": worker_count,
        "source_count": len(summary_items),
        "extraction_count": total_extractions,
        "raw_pack_item_count": len(raw_pack_items),
        "logical_texture_count": len(logical_groups),
        "logical_pack_item_count": len(logical_pack_items),
        "logical_similarity_merged_group_count": sum(
            1 for group in logical_groups if group.merged_by_similarity
        ),
        "deduplicated_identical_output_count": deduplicated_outputs,
        "packed_atlas_count": len(pack_metadata["atlases"]),
        "duplicate_exact_ids": duplicate_summary,
        "similarity_report_json": str(similarity_report_path),
        "logical_group_metadata_json": str(logical_group_metadata_path),
        "occurrence_remap_json": str(occurrence_remap_path),
        "similarity_candidate_counts": {
            "auto_duplicate_candidates": similarity_report["auto_duplicate_candidate_count"],
            "review_candidates": similarity_report["review_candidate_count"],
            "auto_duplicate_components": similarity_report.get("auto_duplicate_component_count", 0),
        },
        "timing_seconds": {
            "sources": source_stage_duration,
            "pack": pack_duration,
            "total": total_duration,
        },
        "fixtures": summary_items,
        "pack_manifest_json": str(pack_manifest_path),
        "pack_metadata_json": str(pack_metadata_path),
    }
    write_json(summary_path, summary)

    LOGGER.info(
        "Processed %d source image(s) into %d extracted item(s) in %s.",
        len(summary_items),
        total_extractions,
        format_duration(total_duration),
    )
    LOGGER.info("Collapsed %d identical cropped/trimmed pair(s).", deduplicated_outputs)
    LOGGER.info(
        "Packed %d logical image(s) into %d atlas(es) in %s.",
        len(logical_pack_items),
        len(pack_metadata["atlases"]),
        format_duration(pack_duration),
    )
    LOGGER.info(
        "Similarity report: %d high-confidence candidate pair(s), %d review pair(s).",
        similarity_report["auto_duplicate_candidate_count"],
        similarity_report["review_candidate_count"],
    )
    LOGGER.info("Summary: %s", summary_path)
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
