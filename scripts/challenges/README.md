# Archived milestone challenge scripts

These Phase 12 empirical challenge harnesses are **archive-only**.
They are not a product gate and must not be invoked by
`verify-product.sh`, `verify-offline.sh`, or `verify-web-server.sh`.

Unique assertions have been absorbed into `scripts/test_server_e2e.py`:

- malformed / out-of-range SSE cursor
- interrupted reload, corrupt state, LRU cap
- JobConfig omit/clamp and restart persistence
- concurrent atomic save replace/skip

Do not add new `*.py` challenge entry points under `scripts/`.
