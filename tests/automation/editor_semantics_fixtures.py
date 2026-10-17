"""Pinned source fixtures shared by native and mapped editor parity tests."""

from pathlib import Path


MIXED_SOURCE_TEXT = "alpha\r\nbeta\n漢字\t😀\rfinal\r\n"
MAPPED_PREFIX_BYTES = 48 * 1024 * 1024


def utf16_units(text: str) -> int:
    return len(text.encode("utf-16-le", errors="surrogatepass")) // 2


def write_mapped_comparison_fixture(path: Path, suffix: str = MIXED_SOURCE_TEXT) -> int:
    """Write an activation-sized ASCII row followed by the exact parity suffix.

    Return the suffix's absolute UTF-16 start. The prefix is ASCII, so its byte
    length and UTF-16 unit length are equal.
    """
    block = b"x" * (1024 * 1024)
    with Path(path).open("wb") as output:
        remaining = MAPPED_PREFIX_BYTES
        while remaining:
            count = min(remaining, len(block))
            output.write(block[:count])
            remaining -= count
        output.write(suffix.encode("utf-8"))
    return MAPPED_PREFIX_BYTES
