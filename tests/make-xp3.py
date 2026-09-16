#!/usr/bin/env python3
"""Pack a directory tree into an XP3 archive.

The layout is taken from this engine's reader (src/core/base/XP3Archive.cpp,
tTVPXP3Archive::Init / FindChunk), so anything this tool produces is exactly
what the player expects:

    offset 0   11 bytes  signature "XP3\\r\\n \\n\\x1a\\x8bg\\x01"
    offset 11  8 bytes   index offset (u64 LE)

    index block (at that offset)
        u8  index_flag          0 = raw, 1 = zlib (bit 0x80 = another index follows)
        if zlib: u64 compressed_size, u64 raw_size, zlib data
        else:    u64 index_size, index data

    index data: a stream of nested chunks, each
        u32 tag ('File'/'info'/'segm'/'adlr') + u64 payload_size + payload

    'File'  payload contains the other three chunks
    'info'  u32 flags, u64 original_size, u64 archived_size,
            u16 name_length, name (UTF-16LE, '/' -separated)
    'segm'  28 bytes per segment: u32 flags (0 = stored, 1 = zlib),
            u64 data offset (absolute), u64 original_size, u64 archived_size
    'adlr'  u32 file hash (this port only reads it back; 0 is accepted)

Usage:
    tests/make-xp3.py <input-directory> <output.xp3>
                      [--compress]      zlib-compress the file payloads
                      [--raw-index]     store the index uncompressed
                      [--encrypt]       apply the "krkr2-smoke-xor/1" cipher
                      [--protected]     set TVP_XP3_FILE_PROTECTED on each file
                      [--quiet]

Notes:
    * Stored (uncompressed) payloads are the default: the reader handles both,
      and stored data makes an archive easy to inspect.
    * Names are written with '/' separators and lower-cased, matching the
      engine's case-insensitive storage-name normalisation.
    * --encrypt only touches the *payloads*.  The index stays a plain (zlib)
      index: the engine must be able to list the archive before any decryption
      filter exists, which is also how real encrypted XP3 archives are built
      (the index encoding methods are the two documented ones, 0 and 1; an
      obfuscated index cannot be read by this engine at all).
    * --compress --encrypt stores  zlib(cipher(plain)),  matching the engine's
      read path:  stored -> zlib decompress -> extraction filter.

The "krkr2-smoke-xor/1" cipher (registered by tests/encrypted-game/xp3filter.tjs
in TJS; this is the normative description of it):

    seed(name)          # 8-bit per-file key seed
        h = 0
        for ch in name.lower():              # name as stored in the index
            c = PRINTABLE_ASCII.find(ch)     # -1 if not in 0x20..0x7e
            if c >= 0: c += 0x20             # -> character code
            h = (h * 31 + c) & 0xFFFFFF      # 24-bit, stays inside int32
        return (h ^ (h >> 8) ^ (h >> 16)) & 0xFF

    keystream_byte(pos, seed)                # pos = absolute offset in the file
        v = (pos * 0x9D + seed * 0x2B + 0x5A) & 0xFF
        r = pos & 7
        v = (((v << r) | (v >> (8 - r))) & 0xFF)   # rotate left by r bits
        return v ^ ((pos >> 3) & 0xFF)

    cipher(data, name) = data[i] ^ keystream_byte(i, seed(name))   # involution

The alphabet dance in seed() exists because TJS2 strings have no charCodeAt();
the filter script gets a character code with indexOf() on the same literal
alphabet, which keeps both sides bit-identical.  File names are therefore
expected to be printable ASCII (all names in the fixture are).

It is a stream cipher over the absolute file offset, so it is position based
rather than state based: the engine hands the filter one buffer at a time with
the buffer's absolute offset, which makes this the property the filter needs.
The cipher is deliberately trivial -- it exists to prove the *patch mechanism*
(decrypt-on-read through a scripted filter), not to model a real game's
cryptography.
"""

import os
import struct
import sys
import zlib

SIGNATURE = b"XP3\r\n \n\x1a\x8bg\x01"
INDEX_ENCODE_RAW = 0
INDEX_ENCODE_ZLIB = 1
SEGM_ENCODE_RAW = 0
SEGM_ENCODE_ZLIB = 1
XP3_FILE_PROTECTED = 1 << 31

CIPHER_NAME = "krkr2-smoke-xor/1"

# Printable ASCII, 0x20..0x7e; the filter script carries the same string as a
# literal and gets character codes with indexOf() (TJS2 has no charCodeAt).
PRINTABLE_ASCII = "".join(chr(c) for c in range(0x20, 0x7F))


# --------------------------------------------------------------------------
# payload cipher (see module docstring)
# --------------------------------------------------------------------------
def name_seed(name: str) -> int:
    """8-bit per-file key seed derived from the in-archive file name."""
    h = 0
    for ch in name.lower():
        c = PRINTABLE_ASCII.find(ch)
        if c >= 0:
            c += 0x20
        h = (h * 31 + c) & 0xFFFFFF
    return (h ^ (h >> 8) ^ (h >> 16)) & 0xFF


def keystream_byte(pos: int, seed: int) -> int:
    v = (pos * 0x9D + seed * 0x2B + 0x5A) & 0xFF
    r = pos & 7
    v = ((v << r) | (v >> (8 - r))) & 0xFF
    return v ^ ((pos >> 3) & 0xFF)


def apply_cipher(name: str, data: bytes) -> bytes:
    """XOR `data` with the keystream for file `name` (its own inverse)."""
    seed = name_seed(name)
    return bytes(b ^ keystream_byte(i, seed) for i, b in enumerate(data))


# --------------------------------------------------------------------------
# archive writer
# --------------------------------------------------------------------------
def chunk(tag: bytes, payload: bytes) -> bytes:
    assert len(tag) == 4
    return tag + struct.pack("<Q", len(payload)) + payload


def build(src, dst, compress_files=False, raw_index=False, encrypt=False,
          protected=False, quiet=False):
    if not os.path.isdir(src):
        print(f"make-xp3: {src} is not a directory", file=sys.stderr)
        return None

    # Collect files, sorted for reproducibility, names normalised the way the
    # engine's storage layer expects ("dir/file.ext", lower case).
    files = []
    for root, dirs, names in os.walk(src):
        dirs.sort()
        for n in sorted(names):
            full = os.path.join(root, n)
            rel = os.path.relpath(full, src).replace(os.sep, "/").lower()
            with open(full, "rb") as f:
                files.append((rel, f.read()))

    # Lay out payloads: header (11 + 8) then each file's data in index order.
    # Offsets advance by the *written* payload size, which differs from the
    # original size when the payloads are compressed.
    payloads = []
    for name, data in files:
        stored = apply_cipher(name, data) if encrypt else data
        if compress_files:
            stored = zlib.compress(stored)
        payloads.append((name, data, stored))
    header_len = len(SIGNATURE) + 8
    offsets = []
    pos = header_len
    for _name, _data, payload in payloads:
        offsets.append(pos)
        pos += len(payload)
    index_offset = pos

    blob = bytearray()
    infos = []
    for (name, data, payload), off in zip(payloads, offsets):
        blob += payload
        name16 = name.encode("utf-16-le")
        # info: flags(u32), orgsize(u64), arcsize(u64), namelen(u16), name
        flags = XP3_FILE_PROTECTED if protected else 0
        info = struct.pack("<IQQH", flags, len(data), len(payload), len(name))
        info += name16
        segm_entry = struct.pack(
            "<IQQQ",
            SEGM_ENCODE_ZLIB if compress_files else SEGM_ENCODE_RAW,
            off,
            len(data),
            len(payload),
        )
        infos.append((chunk(b"info", info), chunk(b"segm", segm_entry), off,
                      len(payload)))

    index = bytearray()
    for info_c, segm_c, _off, _size in infos:
        file_payload = info_c + segm_c + chunk(b"adlr", struct.pack("<I", 0))
        index += chunk(b"File", file_payload)

    if raw_index:
        index_block = struct.pack("<BQ", INDEX_ENCODE_RAW, len(index)) + bytes(index)
    else:
        compressed = zlib.compress(bytes(index), 9)
        index_block = struct.pack("<BQQ", INDEX_ENCODE_ZLIB, len(compressed), len(index)) + compressed

    with open(dst, "wb") as f:
        f.write(SIGNATURE)
        f.write(struct.pack("<Q", index_offset))
        f.write(bytes(blob))
        f.write(index_block)

    if not quiet:
        total = index_offset + len(index_block)
        print(f"make-xp3: {dst}: {len(files)} file(s), {total} bytes, "
              f"index {'raw' if raw_index else 'zlib'}, payloads "
              f"{'zlib' if compress_files else 'stored'}"
              f"{', cipher ' + CIPHER_NAME if encrypt else ''}"
              f"{', protected' if protected else ''}")
        for (name, data), off in zip(files, offsets):
            seed = f", seed=0x{name_seed(name):02x}" if encrypt else ""
            print(f"    {name} ({len(data)} bytes @ {off}{seed})")
    return {"files": [n for n, _ in files], "index_offset": index_offset,
            "size": index_offset + len(index_block)}


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    flags = {a for a in argv[1:] if a.startswith("--")}
    if len(args) != 2:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        print("usage: make-xp3.py <input-directory> <output.xp3> "
              "[--compress] [--raw-index] [--encrypt] [--protected] [--quiet]",
              file=sys.stderr)
        return 2
    src, dst = args
    if build(src, dst,
             compress_files="--compress" in flags,
             raw_index="--raw-index" in flags,
             encrypt="--encrypt" in flags,
             protected="--protected" in flags,
             quiet="--quiet" in flags) is None:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
