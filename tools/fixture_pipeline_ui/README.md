# Fixture Pipeline UI

`tools/fixture_pipeline_ui/main.py` is a local desktop wrapper around `tools/run_fixture_pipeline.py`.

It does three things:

- picks the source atlas folder
- picks a workspace root where `fixture_asset_store/`, `fixture_logical_store/`, and `fixture_pipeline/` are created
- reviews unresolved similarity groups by marking logical textures as the same or different

For reviewed groups, the UI writes:

- `aliases`
  - loser logical IDs mapped to the chosen representative
- `distinct_pairs`
  - logical-ID pairs that should stay separate and stop resurfacing as review candidates

Run it with:

```bash
python tools/fixture_pipeline_ui/main.py
```

The UI expects:

- Python with Tkinter available
- Pillow installed
- `libatlas_tool` built, unless you browse to an explicit tool path
