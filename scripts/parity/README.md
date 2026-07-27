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

## Adding a signature golden (6.1)

1. Freeze `oracle_commit` and input asset SHA256.
2. Add `scripts/parity/dump_signature.py` calling Python `compute_signature`.
3. Write `signature.jsonl` (+ envelope metadata).
4. Compare L0 exact for dhash/timestamps; L1 epsilon for `fg_ratio`.
5. Add `cpp/tests/parity/signature_parity_test.cpp`.
6. Bump `golden_schema_version` only if field rules change.
