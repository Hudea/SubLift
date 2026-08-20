# Native resource manifest

`manifest.v1.json` is the product contract for Paddle PP-OCRv6-small models,
the recognition dictionary, and ONNX Runtime shared libraries.

- Models and ORT artifacts are pinned by SHA-256.
- Fetch writes a temporary file, verifies the digest, then atomically replaces
  the destination. A failed download or digest mismatch must not leave a file
  that `ResourceLocator` treats as available.
- ONNX Runtime may come from the official GitHub archive or, on macOS, from
  Homebrew `onnxruntime` 1.28.0. Python wheels, `.venv`, and `site-packages`
  are not product sources.
- Changing a pin requires re-running the applicable Paddle quality / parity /
  performance gates.

Install or inspect with Native CLI:

```bash
./build/cpp/bin/sublift resources status
./build/cpp/bin/sublift resources install
```
