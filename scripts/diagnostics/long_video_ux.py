"""长视频 UX 诊断不再走 Python IPC。

产品与诊断请使用 Native ``sublift extract`` / ``sublift_worker``。
历史 Python IPC 路径见 Git tag ``python-product-last``。
"""

from __future__ import annotations

import sys


def main() -> None:
    print(
        "错误：Python IPC Bridge 已移除。请使用 Native CLI："
        "./build/cpp/bin/sublift extract <video> -o out.srt",
        file=sys.stderr,
    )
    raise SystemExit(2)


if __name__ == "__main__":
    main()
