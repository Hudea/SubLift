# Product verification

`verify-product.sh` is the Python-free product gate. It builds the Native tree,
runs CTest, Swift tests, Web tests, resource/ORT/capability checks, and at least
one Native CLI extract plus one Native Server extract that exports SRT.

It does not install Python packages, does not run Python scripts, and does not
skip a required product check because `python`, `uv`, or `.venv` are missing.

Isolated Oracle, parity, and benchmark tools use `../verify-offline.sh`.
`../verify-standard.sh` is an explicit transitional mixed Native + Oracle +
cutover gate. It is not the default submit/merge entry.

`../verify-container.sh` is the Python-free LAN container gate (Phase 14).
It needs a Docker daemon: `docker build`, Compose with a host media directory
mounted at `/media`, `paddle.available=true`, sandbox rejection of system
paths, a real Paddle job that only uses container `/media` paths, and exact
golden SRT comparison. Without a daemon it prints SKIPPED and exits 2 (not
green). Missing golden records a candidate under `debug/container/` and fails
until a human copies it to `benchmark/container/paddle_drawtext.v1.srt`.
