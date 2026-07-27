# Parity dump scripts

Frozen-oracle helpers for Phase 6. Contract: [`docs/cpp/parity-contract.md`](../../docs/cpp/parity-contract.md).

## Default Config golden (feat-06004)

```bash
# regenerate
uv run python scripts/parity/dump_config.py

# verify committed golden vs Python DEFAULT_CONFIG
uv run python scripts/parity/dump_config.py --check
```

Golden path: `benchmark/parity/goldens/config/default_config.v1.json`.

C++ loads the same file in Catch2 `[parity][config]` (offline; no Python at test time).

## Signature golden (feat-06101)

Full sub-phase plan: [`docs/cpp/phase6.1-pure-pipeline.md`](../../docs/cpp/phase6.1-pure-pipeline.md).

```bash
# regenerate fixtures (proves bgr_quirk RGB2GRAY vs BGR2GRAY trap)
uv run python scripts/parity/gen_signature_fixtures.py

# write golden (refuses dirty worktree unless --allow-dirty)
uv run python scripts/parity/dump_signature.py
# or: uv run python scripts/parity/dump_signature.py --allow-dirty

# verify core (config + per-fixture semantics/asset/sha/geometry/ts/fg/dhash)
uv run python scripts/parity/dump_signature.py --check
```

C++: `ctest` `[parity][signature]` loads the same fixtures, verifies `input_asset_sha256`, then L0/L1 compares candidate signatures.

OpenCV: soft-optional at CMake configure (`brew install opencv@4`); hard-require with `-DSUBLIFT_REQUIRE_OPENCV=ON`.

## Changepoint golden (feat-06102)

```bash
uv run python scripts/parity/gen_changepoint_fixtures.py
uv run python scripts/parity/dump_changepoint.py          # or --allow-dirty
uv run python scripts/parity/dump_changepoint.py --check
```

Golden: `benchmark/parity/goldens/events/changepoint.v1.json` (scenarios + L0 events).

Downstream: `dump_timeline.py` → segments, `dump_dedupe.py` → entries, `dump_line_select.py` → line-select.
