# IPC protocol fixtures

Expected message-order fixtures for Python/C++ worker parity and Swift client tests.
Format is defined in `docs/cpp/worker-ipc-contract.md` §11.

## Example: path mode success (mock OCR, schematic)

File: `path_mode_mock_success.messages.jsonl` (one JSON object per line, logical order).

```text
{"dir":"c2w","type":"hello","client":"sublift-mac","protocol_version":1}
{"dir":"w2c","type":"bye","protocol_version":1,"runtime":"python","engines":["vision","mock","paddle"],"capabilities":["path_mode","push_entry","cancel"]}
{"dir":"c2w","type":"start_job","video_id":"v1","engine":"mock","video_path":"/path/to.mp4","fps":5.0}
{"dir":"w2c","type":"progress","video_id":"v1","pct":0.1,"stage":"processing"}
{"dir":"w2c","type":"push_entry","video_id":"v1","entry":{"start_ms":0,"end_ms":1000,"text":"hello"}}
{"dir":"w2c","type":"progress","video_id":"v1","pct":1.0,"stage":"finalizing"}
{"dir":"w2c","type":"entries","video_id":"v1","entries":[{"start_ms":0,"end_ms":1000,"text":"hello"}]}
```

Notes:

- Success is determined by receiving `entries` (see PipelineClient `requestStreaming`).
- `done(ok=true)` may follow and can be ignored by the client.
- `done(ok=false)` / `error` are failures.
- Real recordings should use frozen oracle assets and redact absolute paths if published.
