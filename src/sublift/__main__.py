"""Package entry: the product command is Native CLI, not this module."""

from __future__ import annotations

import sys

_MSG = (
    "错误：产品入口是 Native CLI（build/cpp/bin/sublift）。\n"
    "Python 包不再提供 extract / IPC Worker。离线工具请使用 sublift-benchmark。"
)


def main() -> None:
    print(_MSG, file=sys.stderr)
    raise SystemExit(2)


if __name__ == "__main__":
    main()
