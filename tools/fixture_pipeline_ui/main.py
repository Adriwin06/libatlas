#!/usr/bin/env python3

from __future__ import annotations

import json
import subprocess
import sys
import threading
import tkinter as tk
from dataclasses import dataclass
from itertools import combinations
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from PIL import Image, ImageTk


def canonical_pair(left: str, right: str) -> tuple[str, str]:
    if left <= right:
        return left, right
    return right, left


@dataclass(frozen=True)
class ReviewGroupRecord:
    group_id: str
    group_path: Path
    contact_sheet: Path
    decision_json: Path
    logical_id_count: int
    member_occurrence_count: int


class FixturePipelineUi:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.repo_root = Path(__file__).resolve().parents[2]
        self.pipeline_script = self.repo_root / "tools" / "run_fixture_pipeline.py"

        self.source_dir_var = tk.StringVar(value=str(self.repo_root / "tests" / "images_files"))
        self.workspace_root_var = tk.StringVar(value=str(self.repo_root / "build"))
        self.tool_path_var = tk.StringVar(value="")
        self.config_var = tk.StringVar(value="Debug")
        self.split_mode_var = tk.StringVar(value="components")
        self.max_pairs_var = tk.StringVar(value="20000")
        self.status_var = tk.StringVar(value="Idle")

        self.group_records: list[ReviewGroupRecord] = []
        self.member_editors: list[dict[str, object]] = []
        self.current_group_record: ReviewGroupRecord | None = None
        self.current_group_manifest: dict[str, object] | None = None
        self.current_contact_sheet_photo: ImageTk.PhotoImage | None = None
        self.member_thumbnail_photos: list[ImageTk.PhotoImage] = []
        self.pipeline_thread: threading.Thread | None = None

        self.root.title("libatlas Fixture Pipeline UI")
        self.root.geometry("1500x980")
        self.root.minsize(1200, 760)

        self._build_ui()
        self._refresh_workspace_paths()
        self.refresh_review_groups()

    @property
    def asset_store_dir(self) -> Path:
        return Path(self.workspace_root_var.get()).expanduser().resolve() / "fixture_asset_store"

    @property
    def logical_store_dir(self) -> Path:
        return Path(self.workspace_root_var.get()).expanduser().resolve() / "fixture_logical_store"

    @property
    def pipeline_work_dir(self) -> Path:
        return Path(self.workspace_root_var.get()).expanduser().resolve() / "fixture_pipeline"

    def _build_ui(self) -> None:
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(1, weight=1)
        self.root.rowconfigure(2, weight=0)

        settings = ttk.Frame(self.root, padding=12)
        settings.grid(row=0, column=0, sticky="nsew")
        settings.columnconfigure(1, weight=1)

        ttk.Label(settings, text="Source atlas folder").grid(row=0, column=0, sticky="w")
        ttk.Entry(settings, textvariable=self.source_dir_var).grid(row=0, column=1, sticky="ew", padx=(8, 8))
        ttk.Button(settings, text="Browse", command=self.choose_source_dir).grid(row=0, column=2, sticky="ew")

        ttk.Label(settings, text="Workspace root").grid(row=1, column=0, sticky="w", pady=(8, 0))
        workspace_entry = ttk.Entry(settings, textvariable=self.workspace_root_var)
        workspace_entry.grid(row=1, column=1, sticky="ew", padx=(8, 8), pady=(8, 0))
        workspace_entry.bind("<FocusOut>", lambda _event: self._refresh_workspace_paths())
        ttk.Button(settings, text="Browse", command=self.choose_workspace_root).grid(row=1, column=2, sticky="ew", pady=(8, 0))

        ttk.Label(settings, text="Tool path").grid(row=2, column=0, sticky="w", pady=(8, 0))
        ttk.Entry(settings, textvariable=self.tool_path_var).grid(row=2, column=1, sticky="ew", padx=(8, 8), pady=(8, 0))
        ttk.Button(settings, text="Browse", command=self.choose_tool_path).grid(row=2, column=2, sticky="ew", pady=(8, 0))

        options = ttk.Frame(settings)
        options.grid(row=3, column=0, columnspan=3, sticky="ew", pady=(10, 0))
        for column in range(6):
            options.columnconfigure(column, weight=1 if column in {1, 3, 5} else 0)

        ttk.Label(options, text="Config").grid(row=0, column=0, sticky="w")
        ttk.Combobox(options, textvariable=self.config_var, values=("Debug", "Release"), state="readonly").grid(
            row=0, column=1, sticky="ew", padx=(8, 16)
        )
        ttk.Label(options, text="Split mode").grid(row=0, column=2, sticky="w")
        ttk.Combobox(
            options,
            textvariable=self.split_mode_var,
            values=("components", "auto", "bbox"),
            state="readonly",
        ).grid(row=0, column=3, sticky="ew", padx=(8, 16))
        ttk.Label(options, text="Similarity max pairs").grid(row=0, column=4, sticky="w")
        ttk.Entry(options, textvariable=self.max_pairs_var).grid(row=0, column=5, sticky="ew", padx=(8, 0))

        self.workspace_paths_label = ttk.Label(settings, justify="left")
        self.workspace_paths_label.grid(row=4, column=0, columnspan=3, sticky="w", pady=(10, 0))

        actions = ttk.Frame(settings)
        actions.grid(row=5, column=0, columnspan=3, sticky="ew", pady=(10, 0))
        actions.columnconfigure(3, weight=1)
        ttk.Button(actions, text="Run pipeline", command=self.run_pipeline).grid(row=0, column=0, sticky="w")
        ttk.Button(actions, text="Refresh review groups", command=self.refresh_review_groups).grid(
            row=0, column=1, sticky="w", padx=(8, 0)
        )
        ttk.Button(actions, text="Open logical store", command=self.open_logical_store).grid(
            row=0, column=2, sticky="w", padx=(8, 0)
        )
        ttk.Label(actions, textvariable=self.status_var).grid(row=0, column=3, sticky="e")

        content = ttk.Panedwindow(self.root, orient=tk.HORIZONTAL)
        content.grid(row=1, column=0, sticky="nsew", padx=12, pady=(0, 12))

        left = ttk.Frame(content, padding=8)
        left.columnconfigure(0, weight=1)
        left.rowconfigure(1, weight=1)
        content.add(left, weight=1)

        ttk.Label(left, text="Review groups").grid(row=0, column=0, sticky="w")
        self.group_listbox = tk.Listbox(left, exportselection=False)
        self.group_listbox.grid(row=1, column=0, sticky="nsew", pady=(8, 0))
        self.group_listbox.bind("<<ListboxSelect>>", self.on_group_selected)

        right = ttk.Frame(content, padding=8)
        right.columnconfigure(0, weight=1)
        right.rowconfigure(3, weight=1)
        content.add(right, weight=4)

        self.group_title_label = ttk.Label(right, text="No review group selected")
        self.group_title_label.grid(row=0, column=0, sticky="w")

        self.contact_sheet_label = ttk.Label(right)
        self.contact_sheet_label.grid(row=1, column=0, sticky="nsew", pady=(8, 0))

        decision_actions = ttk.Frame(right)
        decision_actions.grid(row=2, column=0, sticky="ew", pady=(10, 0))
        ttk.Button(decision_actions, text="Save decision", command=self.save_current_decision).grid(
            row=0, column=0, sticky="w"
        )
        ttk.Button(
            decision_actions,
            text="Save and rerun",
            command=self.save_current_decision_and_rerun,
        ).grid(row=0, column=1, sticky="w", padx=(8, 0))
        ttk.Button(decision_actions, text="Reload group", command=self.reload_current_group).grid(
            row=0, column=2, sticky="w", padx=(8, 0)
        )

        members_container = ttk.Frame(right)
        members_container.grid(row=3, column=0, sticky="nsew", pady=(10, 0))
        members_container.columnconfigure(0, weight=1)
        members_container.rowconfigure(0, weight=1)

        self.members_canvas = tk.Canvas(members_container, highlightthickness=0)
        self.members_canvas.grid(row=0, column=0, sticky="nsew")
        scrollbar = ttk.Scrollbar(members_container, orient="vertical", command=self.members_canvas.yview)
        scrollbar.grid(row=0, column=1, sticky="ns")
        self.members_canvas.configure(yscrollcommand=scrollbar.set)

        self.members_frame = ttk.Frame(self.members_canvas)
        self.members_window_id = self.members_canvas.create_window((0, 0), window=self.members_frame, anchor="nw")
        self.members_frame.bind(
            "<Configure>",
            lambda _event: self.members_canvas.configure(scrollregion=self.members_canvas.bbox("all")),
        )
        self.members_canvas.bind(
            "<Configure>",
            lambda event: self.members_canvas.itemconfigure(self.members_window_id, width=event.width),
        )

        bottom = ttk.Frame(self.root, padding=(12, 0, 12, 12))
        bottom.grid(row=2, column=0, sticky="nsew")
        bottom.columnconfigure(0, weight=1)
        bottom.rowconfigure(2, weight=1)

        ttk.Label(bottom, text="Decision notes").grid(row=0, column=0, sticky="w")
        self.notes_text = tk.Text(bottom, height=4, wrap="word")
        self.notes_text.grid(row=1, column=0, sticky="ew")

        ttk.Label(bottom, text="Pipeline log").grid(row=2, column=0, sticky="w", pady=(10, 0))
        self.log_text = tk.Text(bottom, height=10, wrap="word")
        self.log_text.grid(row=3, column=0, sticky="nsew")

    def _refresh_workspace_paths(self) -> None:
        self.workspace_paths_label.configure(
            text=(
                f"Asset store: {self.asset_store_dir}\n"
                f"Logical store: {self.logical_store_dir}\n"
                f"Pipeline work dir: {self.pipeline_work_dir}"
            )
        )

    def choose_source_dir(self) -> None:
        selected = filedialog.askdirectory(initialdir=self.source_dir_var.get() or str(self.repo_root))
        if selected:
            self.source_dir_var.set(selected)

    def choose_workspace_root(self) -> None:
        selected = filedialog.askdirectory(initialdir=self.workspace_root_var.get() or str(self.repo_root))
        if selected:
            self.workspace_root_var.set(selected)
            self._refresh_workspace_paths()

    def choose_tool_path(self) -> None:
        selected = filedialog.askopenfilename(initialdir=str(self.repo_root / "build"))
        if selected:
            self.tool_path_var.set(selected)

    def append_log(self, message: str) -> None:
        self.log_text.insert("end", message.rstrip() + "\n")
        self.log_text.see("end")

    def build_pipeline_command(self) -> list[str]:
        max_pairs = int(self.max_pairs_var.get())
        command = [
            sys.executable,
            str(self.pipeline_script),
            "--input-dir",
            str(Path(self.source_dir_var.get()).expanduser().resolve()),
            "--work-dir",
            str(self.pipeline_work_dir),
            "--asset-store",
            str(self.asset_store_dir),
            "--logical-store",
            str(self.logical_store_dir),
            "--config",
            self.config_var.get(),
            "--split-mode",
            self.split_mode_var.get(),
            "--similarity-report-max-pairs",
            str(max_pairs),
        ]
        if self.tool_path_var.get().strip():
            command.extend(["--tool", str(Path(self.tool_path_var.get()).expanduser().resolve())])
        return command

    def run_pipeline(self) -> None:
        if self.pipeline_thread and self.pipeline_thread.is_alive():
            messagebox.showinfo("Pipeline running", "The pipeline is already running.")
            return

        try:
            command = self.build_pipeline_command()
        except Exception as error:
            messagebox.showerror("Invalid configuration", str(error))
            return

        self.status_var.set("Running pipeline...")
        self.append_log("$ " + subprocess.list2cmdline(command))
        self.pipeline_thread = threading.Thread(
            target=self._run_pipeline_worker,
            args=(command,),
            daemon=True,
        )
        self.pipeline_thread.start()

    def _run_pipeline_worker(self, command: list[str]) -> None:
        completed = subprocess.run(command, capture_output=True, text=True)
        self.root.after(
            0,
            lambda: self._pipeline_finished(
                completed.returncode,
                completed.stdout,
                completed.stderr,
            ),
        )

    def _pipeline_finished(self, returncode: int, stdout: str, stderr: str) -> None:
        if stdout.strip():
            self.append_log(stdout)
        if stderr.strip():
            self.append_log(stderr)

        if returncode == 0:
            self.status_var.set("Pipeline finished")
            self.refresh_review_groups()
        else:
            self.status_var.set(f"Pipeline failed ({returncode})")
            messagebox.showerror("Pipeline failed", f"Pipeline exited with code {returncode}.")

    def load_review_manifest(self) -> list[ReviewGroupRecord]:
        manifest_path = self.logical_store_dir / "review_candidates" / "review_groups.json"
        if not manifest_path.exists():
            return []

        payload = json.loads(manifest_path.read_text(encoding="utf-8"))
        review_groups = payload.get("review_groups")
        if not isinstance(review_groups, list):
            return []

        records: list[ReviewGroupRecord] = []
        for entry in review_groups:
            if not isinstance(entry, dict):
                continue
            group_id = entry.get("group_id")
            path = entry.get("path")
            contact_sheet = entry.get("contact_sheet")
            decision_json = entry.get("decision_json")
            if not all(isinstance(value, str) for value in (group_id, path, contact_sheet, decision_json)):
                continue
            records.append(
                ReviewGroupRecord(
                    group_id=group_id,
                    group_path=Path(path),
                    contact_sheet=Path(contact_sheet),
                    decision_json=Path(decision_json),
                    logical_id_count=int(entry.get("logical_id_count", 0)),
                    member_occurrence_count=int(entry.get("member_occurrence_count", 0)),
                )
            )
        return records

    def refresh_review_groups(self) -> None:
        self.group_records = self.load_review_manifest()
        self.group_listbox.delete(0, "end")

        for record in self.group_records:
            label = (
                f"{record.group_id} "
                f"({record.logical_id_count} logical IDs, {record.member_occurrence_count} occurrences)"
            )
            self.group_listbox.insert("end", label)

        if not self.group_records:
            self.clear_group_details("No unresolved review groups.")
            return

        self.group_listbox.selection_clear(0, "end")
        self.group_listbox.selection_set(0)
        self.on_group_selected()

    def clear_group_details(self, title: str) -> None:
        self.current_group_record = None
        self.current_group_manifest = None
        self.member_editors = []
        self.member_thumbnail_photos = []
        self.current_contact_sheet_photo = None
        self.group_title_label.configure(text=title)
        self.contact_sheet_label.configure(image="", text="")
        self.notes_text.delete("1.0", "end")
        for child in self.members_frame.winfo_children():
            child.destroy()

    def on_group_selected(self, _event: object | None = None) -> None:
        selection = self.group_listbox.curselection()
        if not selection:
            return
        record = self.group_records[selection[0]]
        self.load_group(record)

    def load_group(self, record: ReviewGroupRecord) -> None:
        group_manifest_path = record.group_path / "group.json"
        decision_path = record.decision_json
        if not group_manifest_path.exists() or not decision_path.exists():
            self.clear_group_details("Selected review group is missing files.")
            return

        group_manifest = json.loads(group_manifest_path.read_text(encoding="utf-8"))
        decision = json.loads(decision_path.read_text(encoding="utf-8"))

        self.current_group_record = record
        self.current_group_manifest = group_manifest
        self.group_title_label.configure(text=record.group_id)

        contact_sheet = self._load_photo(record.contact_sheet, max_size=(980, 320))
        self.current_contact_sheet_photo = contact_sheet
        if contact_sheet is not None:
            self.contact_sheet_label.configure(image=contact_sheet, text="")
        else:
            self.contact_sheet_label.configure(image="", text="No contact sheet available")

        self.notes_text.delete("1.0", "end")
        notes = decision.get("notes", "")
        if isinstance(notes, str):
            self.notes_text.insert("1.0", notes)

        self._build_member_editor(group_manifest, decision)

    def _load_photo(self, path: Path, max_size: tuple[int, int]) -> ImageTk.PhotoImage | None:
        if not path.exists():
            return None
        with Image.open(path) as source:
            source.load()
            preview = source.convert("RGBA")
        preview.thumbnail(max_size)
        return ImageTk.PhotoImage(preview)

    def _build_member_editor(self, group_manifest: dict[str, object], decision: dict[str, object]) -> None:
        for child in self.members_frame.winfo_children():
            child.destroy()

        members = group_manifest.get("members")
        if not isinstance(members, list):
            self.member_editors = []
            return

        assignments, winners = self._derive_assignments(members, decision)
        self.member_thumbnail_photos = []
        self.member_editors = []

        header = ttk.Frame(self.members_frame)
        header.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        header.columnconfigure(1, weight=1)
        ttk.Label(header, text="Thumbnail").grid(row=0, column=0, sticky="w")
        ttk.Label(header, text="Logical texture").grid(row=0, column=1, sticky="w", padx=(12, 0))
        ttk.Label(header, text="Decision group").grid(row=0, column=2, sticky="w", padx=(12, 0))
        ttk.Label(header, text="Representative").grid(row=0, column=3, sticky="w", padx=(12, 0))

        member_count = len(members)
        group_values = [str(index) for index in range(1, member_count + 1)]
        for row_index, member in enumerate(members, start=1):
            if not isinstance(member, dict):
                continue
            logical_id = member.get("logical_id")
            review_image = member.get("review_image")
            if not isinstance(logical_id, str):
                continue

            row = ttk.Frame(self.members_frame, padding=(0, 6))
            row.grid(row=row_index, column=0, sticky="ew")
            row.columnconfigure(1, weight=1)

            thumbnail = self._load_photo(Path(review_image), max_size=(160, 160)) if isinstance(review_image, str) else None
            self.member_thumbnail_photos.append(thumbnail) if thumbnail is not None else None
            thumbnail_label = ttk.Label(row)
            thumbnail_label.grid(row=0, column=0, sticky="nw")
            if thumbnail is not None:
                thumbnail_label.configure(image=thumbnail)
            else:
                thumbnail_label.configure(text="No image")

            details = ttk.Frame(row)
            details.grid(row=0, column=1, sticky="ew", padx=(12, 0))
            details.columnconfigure(0, weight=1)
            ttk.Label(details, text=str(member.get("representative_name", logical_id))).grid(row=0, column=0, sticky="w")
            ttk.Label(details, text=logical_id, foreground="#666666").grid(row=1, column=0, sticky="w", pady=(2, 0))
            ttk.Label(
                details,
                text=f"{member.get('occurrence_count', 0)} occurrences",
                foreground="#666666",
            ).grid(row=2, column=0, sticky="w", pady=(2, 0))

            cluster_var = tk.StringVar(value=str(assignments[logical_id]))
            winner_var = tk.BooleanVar(value=logical_id in winners)
            ttk.Combobox(
                row,
                textvariable=cluster_var,
                values=group_values,
                width=8,
                state="readonly",
            ).grid(row=0, column=2, sticky="nw", padx=(12, 0))
            ttk.Checkbutton(row, variable=winner_var).grid(row=0, column=3, sticky="nw", padx=(24, 0))

            self.member_editors.append(
                {
                    "logical_id": logical_id,
                    "cluster_var": cluster_var,
                    "winner_var": winner_var,
                }
            )

    def _derive_assignments(
        self,
        members: list[object],
        decision: dict[str, object],
    ) -> tuple[dict[str, int], set[str]]:
        logical_ids = [
            member.get("logical_id")
            for member in members
            if isinstance(member, dict) and isinstance(member.get("logical_id"), str)
        ]
        aliases = decision.get("aliases")
        if not isinstance(aliases, dict):
            aliases = {}

        parent: dict[str, str] = {logical_id: logical_id for logical_id in logical_ids}

        def find(value: str) -> str:
            root = parent.setdefault(value, value)
            if root != value:
                parent[value] = find(root)
            return parent[value]

        def union(left: str, right: str) -> None:
            left_root = find(left)
            right_root = find(right)
            if left_root != right_root:
                parent[right_root] = left_root

        winners: set[str] = set()
        for loser_logical_id, winner_logical_id in aliases.items():
            if loser_logical_id in parent and winner_logical_id in parent:
                union(loser_logical_id, winner_logical_id)
                winners.add(winner_logical_id)

        grouped: dict[str, list[str]] = {}
        for logical_id in logical_ids:
            grouped.setdefault(find(logical_id), []).append(logical_id)

        sorted_clusters = sorted((sorted(cluster) for cluster in grouped.values()), key=lambda cluster: cluster[0])
        assignments: dict[str, int] = {}
        normalized_winners: set[str] = set()
        for cluster_index, cluster in enumerate(sorted_clusters, start=1):
            cluster_winner = next((logical_id for logical_id in cluster if logical_id in winners), cluster[0])
            normalized_winners.add(cluster_winner)
            for logical_id in cluster:
                assignments[logical_id] = cluster_index

        return assignments, normalized_winners

    def _collect_current_decision_payload(self) -> dict[str, object]:
        if self.current_group_record is None or self.current_group_manifest is None:
            raise RuntimeError("No review group selected.")

        available_logical_ids = [
            member.get("logical_id")
            for member in self.current_group_manifest.get("members", [])
            if isinstance(member, dict) and isinstance(member.get("logical_id"), str)
        ]

        clusters: dict[str, list[dict[str, object]]] = {}
        for editor in self.member_editors:
            cluster_key = str(editor["cluster_var"].get()).strip()
            if not cluster_key:
                raise ValueError("Every member must have a decision group.")
            clusters.setdefault(cluster_key, []).append(editor)

        aliases: dict[str, str] = {}
        distinct_pairs: set[tuple[str, str]] = set()
        sorted_cluster_members = [clusters[key] for key in sorted(clusters, key=lambda value: (len(value), value))]
        normalized_cluster_ids: list[list[str]] = []
        for cluster_members in sorted_cluster_members:
            checked_winners = [editor for editor in cluster_members if bool(editor["winner_var"].get())]
            if len(checked_winners) > 1:
                duplicate_winners = ", ".join(str(editor["logical_id"]) for editor in checked_winners)
                raise ValueError(f"Only one representative is allowed per decision group. Conflicts: {duplicate_winners}")

            winner_editor = checked_winners[0] if checked_winners else cluster_members[0]
            winner_logical_id = str(winner_editor["logical_id"])
            cluster_logical_ids: list[str] = []
            for editor in cluster_members:
                logical_id = str(editor["logical_id"])
                cluster_logical_ids.append(logical_id)
                if logical_id != winner_logical_id:
                    aliases[logical_id] = winner_logical_id
            normalized_cluster_ids.append(sorted(cluster_logical_ids))

        for left_cluster, right_cluster in combinations(normalized_cluster_ids, 2):
            for left_logical_id in left_cluster:
                for right_logical_id in right_cluster:
                    distinct_pairs.add(canonical_pair(left_logical_id, right_logical_id))

        return {
            "group_id": self.current_group_record.group_id,
            "status": "reviewed",
            "notes": self.notes_text.get("1.0", "end").strip(),
            "aliases": aliases,
            "distinct_pairs": [list(pair) for pair in sorted(distinct_pairs)],
            "available_logical_ids": available_logical_ids,
        }

    def save_current_decision(self) -> None:
        try:
            payload = self._collect_current_decision_payload()
        except Exception as error:
            messagebox.showerror("Could not save decision", str(error))
            return

        decision_path = self.current_group_record.decision_json if self.current_group_record else None
        if decision_path is None:
            return

        decision_path.parent.mkdir(parents=True, exist_ok=True)
        decision_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        self.append_log(f"Saved decision: {decision_path}")
        self.status_var.set("Decision saved")

    def save_current_decision_and_rerun(self) -> None:
        self.save_current_decision()
        if self.status_var.get() != "Decision saved":
            return
        self.run_pipeline()

    def reload_current_group(self) -> None:
        if self.current_group_record is None:
            return
        self.load_group(self.current_group_record)

    def open_logical_store(self) -> None:
        path = self.logical_store_dir
        if not path.exists():
            messagebox.showinfo("Logical store missing", f"Logical store does not exist yet:\n{path}")
            return
        if sys.platform.startswith("win"):
            subprocess.Popen(["explorer", str(path)])
        elif sys.platform == "darwin":
            subprocess.Popen(["open", str(path)])
        else:
            subprocess.Popen(["xdg-open", str(path)])


def main() -> int:
    root = tk.Tk()
    FixturePipelineUi(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
