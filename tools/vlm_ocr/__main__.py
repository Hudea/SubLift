"""Module entrypoint for `python3 -m tools.vlm_ocr`."""

import sys

from .cli import main


if __name__ == "__main__":
    sys.exit(main())
