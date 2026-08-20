"""Frozen Oracle and internal offline-tool implementation.

The product CLI is Native ``build/cpp/bin/sublift``. This package is not a
product runtime. Algorithm copies here are frozen: they reproduce historical
golden/parity assets and must not track new Native features.

Public offline entry: ``python -m sublift_offline`` / ``sublift-benchmark``.
"""

__version__ = "0.1.0"
