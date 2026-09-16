#!/usr/bin/env python3
"""Regenerate quad.png, the colour-reference fixture of the API conformance game.

quad.png is a 4x4 image whose four 2x2 quadrants are pure red (0xff0000, top
left), pure green (0x00ff00, top right), pure blue (0x0000ff, bottom left) and
white (0xffffff, bottom right).  startup.tjs loads it and asserts that
getMainPixel returns those colours; green and white are there so that a
red/blue swap cannot be mistaken for a decode failure.

    python3 make-quad-png.py
"""
import struct
import zlib

W = H = 4
QUADRANTS = {
    (0, 0): (255, 0, 0), (1, 0): (255, 0, 0), (2, 0): (0, 255, 0), (3, 0): (0, 255, 0),
    (0, 1): (255, 0, 0), (1, 1): (255, 0, 0), (2, 1): (0, 255, 0), (3, 1): (0, 255, 0),
    (0, 2): (0, 0, 255), (1, 2): (0, 0, 255), (2, 2): (255, 255, 255), (3, 2): (255, 255, 255),
    (0, 3): (0, 0, 255), (1, 3): (0, 0, 255), (2, 3): (255, 255, 255), (3, 3): (255, 255, 255),
}


def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))


def main():
    raw = b""
    for y in range(H):
        raw += b"\x00" + b"".join(bytes(QUADRANTS[(x, y)]) for x in range(W))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw))
           + chunk(b"IEND", b""))
    with open("quad.png", "wb") as f:
        f.write(png)
    print("quad.png written (%d bytes)" % len(png))


if __name__ == "__main__":
    main()
