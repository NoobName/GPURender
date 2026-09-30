#!/usr/bin/env python3
"""把 GPU 回读出来的原始 RGBA 帧转成 PNG，便于人工核对渲染结果。

这个脚本只在开发/验证阶段用（配合临时的 back buffer 回读代码），
不属于渲染器本身的一部分。只用 Python 标准库 zlib，不依赖 Pillow。

用法：
    python tools/raw_to_png.py frame_raw.rgba 1280 720 frame.png
"""

import struct
import sys
import zlib


def _chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png(path, width, height, rows):
    """rows 是每行 RGB（3 字节/像素）的 bytes 列表。"""
    raw = b"".join(b"\x00" + row for row in rows)  # 每行前置 filter byte 0
    png = b"\x89PNG\r\n\x1a\n"
    png += _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += _chunk(b"IDAT", zlib.compress(raw, 6))
    png += _chunk(b"IEND", b"")
    with open(path, "wb") as fp:
        fp.write(png)


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 1

    src, width, height, dst = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    data = open(src, "rb").read()
    expected = width * height * 4
    if len(data) != expected:
        print("size mismatch: got %d bytes, expected %d" % (len(data), expected))
        return 1

    rows = []
    for y in range(height):
        row = data[y * width * 4:(y + 1) * width * 4]
        rgb = bytearray(width * 3)
        for x in range(width):
            rgb[x * 3 + 0] = row[x * 4 + 0]
            rgb[x * 3 + 1] = row[x * 4 + 1]
            rgb[x * 3 + 2] = row[x * 4 + 2]
        rows.append(bytes(rgb))

    write_png(dst, width, height, rows)
    print("wrote %s (%dx%d)" % (dst, width, height))
    return 0


if __name__ == "__main__":
    sys.exit(main())
