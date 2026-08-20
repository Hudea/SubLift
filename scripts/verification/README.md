# Product verification

`verify-product.sh` is the Python-free product gate. It builds the Native tree,
runs CTest, Swift tests, Web tests, resource/ORT/capability checks, and at least
one Native CLI extract plus one Native Server extract that exports SRT.

It does not install Python packages, does not run Python scripts, and does not
skip a required product check because `python`, `uv`, or `.venv` are missing.

Isolated Oracle, parity, and benchmark tools stay on `../verify-standard.sh`.
