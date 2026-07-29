"""Single source of truth for Phase 6 parity dump scripts.

Consumed by check_cutover_gate.run_correctness_checks only.
"""

from __future__ import annotations

from pathlib import Path

PARITY_DIR = Path(__file__).resolve().parent

# Full cutover correctness suite (order is stable for reports).
PARITY_SCRIPTS: list[tuple[str, Path]] = [
    ("config", PARITY_DIR / "dump_config.py"),
    ("signature", PARITY_DIR / "dump_signature.py"),
    ("changepoint", PARITY_DIR / "dump_changepoint.py"),
    ("timeline", PARITY_DIR / "dump_timeline.py"),
    ("dedupe", PARITY_DIR / "dump_dedupe.py"),
    ("line_select", PARITY_DIR / "dump_line_select.py"),
    ("pipeline", PARITY_DIR / "dump_pipeline.py"),
    ("extractor", PARITY_DIR / "dump_extractor.py"),
    ("vision", PARITY_DIR / "dump_vision.py"),
    ("paddle", PARITY_DIR / "dump_paddle.py"),
    ("paddle_stages", PARITY_DIR / "dump_paddle_stages.py"),
    ("ipc_session", PARITY_DIR / "dump_ipc_session.py"),
]

# Pure goldens (no worker binary). Optional pre-build fail-fast.
PURE_SCRIPTS: list[tuple[str, Path]] = [
    item for item in PARITY_SCRIPTS if item[0] != "ipc_session"
]

IPC_SCRIPTS: list[tuple[str, Path]] = [
    item for item in PARITY_SCRIPTS if item[0] == "ipc_session"
]
