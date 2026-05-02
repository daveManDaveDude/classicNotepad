#!/usr/bin/env python3
import argparse
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate a macOS .icns file from a PNG source.")
    parser.add_argument("--source", required=True, help="Source PNG path.")
    parser.add_argument("--output", required=True, help="Output .icns path.")
    args = parser.parse_args()

    try:
        from PIL import Image
    except ImportError:
        print("Pillow is required to generate the macOS app icon. Install it with: python3 -m pip install Pillow", file=sys.stderr)
        return 1

    source = Path(args.source)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)

    image = Image.open(source).convert("RGBA")
    image.save(
        output,
        sizes=[
            (16, 16),
            (32, 32),
            (128, 128),
            (256, 256),
            (512, 512),
            (1024, 1024),
        ],
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
