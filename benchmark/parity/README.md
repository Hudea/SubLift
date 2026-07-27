# Benchmark / parity goldens

Checked-in frozen oracle artifacts for C++ parity (Phase 6).

| Path | Kind | Schema |
|---|---|---|
| `goldens/config/default_config.v1.json` | default `Config` | v1 |

Regenerate config golden:

```bash
uv run python scripts/parity/dump_config.py
uv run python scripts/parity/dump_config.py --check
```

Policy: goldens must embed `oracle_commit` + `config_fingerprint`. Large video fixtures may use git-lfs later; config golden is small and stays in git.
