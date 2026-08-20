"""Python Oracle IPC 诊断已随产品 Python Worker 移除。

历史可运行 revision 见 Git tag ``python-product-last``。产品路径使用 Native
``sublift_worker``；不要再启动 ``python -m sublift.ipc.server``。
"""

from __future__ import annotations

import sys


def main() -> None:
    print(
        "错误：Python IPC Worker 已从产品与工具入口移除。"
        "请使用 Native sublift_worker，或检出 tag python-product-last。",
        file=sys.stderr,
    )
    raise SystemExit(2)


if __name__ == "__main__":
    main()
