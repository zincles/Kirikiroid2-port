#!/usr/bin/env python3
"""Build the encrypted-XP3 test fixture under tests/encrypted-game/.

The fixture reproduces how an obfuscated XP3 game is started by this engine:

    tests/encrypted-game/
        game.xp3            encrypted archive (cipher: tests/make-xp3.py
                            "--encrypt", files flagged TVP_XP3_FILE_PROTECTED)
        xp3filter.tjs       the patch side: registers the decode filter
        patch.tjs           the patch side: engine startup patch
        no-patch/game.xp3   byte-identical copy of game.xp3, but in a directory
                            without the patch files -> negative control

    ./build/krkr2 tests/encrypted-game/game.xp3          # works
    ./build/krkr2 tests/encrypted-game/no-patch/game.xp3 # fails (no filter)

The archive holds the smoke game's startup.tjs and tone.wav / tone.ogg /
tone.mp3, so a run that reaches "smoke: done" has executed script and decoded
audio *through the cipher*.  The patch files live next to the archive, not
inside it: when an archive is given on the command line the engine's
TVPProjectDir is the archive path, and TVPGetAppPath() (which is what looks for
xp3filter.tjs / patch.tjs) is the archive's directory.

The script re-reads the archive it just wrote with an independent parser
(mirroring src/core/base/XP3Archive.cpp) and checks that every decoded payload
is bit-identical to the input file -- i.e. the cipher is reversible and the
container is well-formed -- and that the stored bytes really are ciphertext.
It also prints, per file, the checksum of the *decoded* bytes the filter logs
("xp3filter: <name> decoded <n> bytes, checksum=<c>"), so an engine run can be
compared against the sources byte for byte.

Usage:
    tests/make-encrypted-game.py [--out <dir>] [--quiet]
"""

import importlib.util
import os
import re
import shutil
import struct
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

SIGNATURE = b"XP3\r\n \n\x1a\x8bg\x01"
SEGM_ENCODE_ZLIB = 1
XP3_FILE_PROTECTED = 1 << 31


def load_maker():
    spec = importlib.util.spec_from_file_location(
        "make_xp3", os.path.join(HERE, "make-xp3.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def iter_chunks(buf, start=0, end=None):
    """Yield (tag, payload) for a chunk stream: u32 tag + u64 size + payload."""
    end = len(buf) if end is None else end
    pos = start
    while pos + 12 <= end:
        tag = bytes(buf[pos:pos + 4])
        size, = struct.unpack_from("<Q", buf, pos + 4)
        yield tag, bytes(buf[pos + 12:pos + 12 + size])
        pos += 12 + size


def read_xp3(path):
    """Independent XP3 reader -> {name: {'flags','data','stored'}}."""
    blob = open(path, "rb").read()
    assert blob[:11] == SIGNATURE, "bad XP3 signature"
    index_offset, = struct.unpack_from("<Q", blob, 11)
    flag = blob[index_offset]
    method = flag & 0x07
    if method == 1:                                   # zlib index
        csize, usize = struct.unpack_from("<QQ", blob, index_offset + 1)
        index = zlib.decompress(blob[index_offset + 17:index_offset + 17 + csize])
        assert len(index) == usize, "index size mismatch"
    elif method == 0:                                 # raw index
        usize, = struct.unpack_from("<Q", blob, index_offset + 1)
        index = blob[index_offset + 9:index_offset + 9 + usize]
    else:
        raise AssertionError(f"unknown index encode method {method}")

    out = {}
    for tag, payload in iter_chunks(index):
        if tag != b"File":
            continue
        sub = dict(iter_chunks(payload))
        flags, orgsize, arcsize = struct.unpack_from("<IQQ", sub[b"info"], 0)
        namelen, = struct.unpack_from("<H", sub[b"info"], 20)
        name = sub[b"info"][22:22 + namelen * 2].decode("utf-16-le")
        data = bytearray()
        for i in range(len(sub[b"segm"]) // 28):
            sflags, soff, sorg, sarc = struct.unpack_from("<IQQQ", sub[b"segm"], i * 28)
            raw = blob[soff:soff + sarc]
            if (sflags & 0x07) == SEGM_ENCODE_ZLIB:
                raw = zlib.decompress(raw)
                assert len(raw) == sorg, "segment size mismatch"
            data += raw
        assert len(data) == orgsize, f"{name}: size mismatch"
        out[name] = {"flags": flags, "stored": bytes(data)}
    return out


def checksum(data: bytes) -> int:
    """The decoded-file checksum the filter script accumulates (s2<<16 | s1)."""
    s1 = s2 = 0
    for b in data:
        s1 = (s1 + b) & 0xFFFF
        s2 = (s2 + s1) & 0xFFFF
    return s2 * 65536 + s1


def check_alphabet(maker):
    """The TJS filter carries the printable-ASCII alphabet as a literal (it
    needs indexOf() for character codes, TJS2 has no charCodeAt).  Verify that
    literal is the same string Python hashes with, so the two sides of the
    cipher cannot drift apart."""
    path = os.path.join(HERE, "encrypted-game", "xp3filter.tjs")
    text = open(path, encoding="utf-8").read()
    m = re.search(r'^var xp3Alpha = "(.*)";$', text, re.M)
    if not m:
        return f"{path}: no xp3Alpha literal found"
    literal = re.sub(r"\\(.)", r"\1", m.group(1))
    if literal != maker.PRINTABLE_ASCII:
        return (f"{path}: xp3Alpha {literal!r} != make-xp3.py "
                f"PRINTABLE_ASCII {maker.PRINTABLE_ASCII!r}")
    return None


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    flags = {a for a in argv[1:] if a.startswith("--")}
    out_dir = os.path.join(HERE, "encrypted-game")
    if "--out" in flags:
        out_dir = argv[argv.index("--out") + 1]
    quiet = "--quiet" in flags
    if args:
        print("usage: make-encrypted-game.py [--out <dir>] [--quiet]",
              file=sys.stderr)
        return 2

    maker = load_maker()
    src = os.path.join(HERE, "smoke-game")
    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, "game.xp3")
    no_patch_dir = os.path.join(out_dir, "no-patch")
    os.makedirs(no_patch_dir, exist_ok=True)

    # Explicit list, so the fixture stays stable while tests/smoke-game grows
    # other assets (and so its contents are documented here).  startup.tjs
    # exercises the script engine, the renderer and the audio decoders.
    wanted = ["startup.tjs", "tone.wav", "tone.ogg", "tone.mp3"]
    for name in wanted:
        if not os.path.isfile(os.path.join(src, name)):
            print(f"make-encrypted-game: {src}/{name} is missing", file=sys.stderr)
            return 1
    staging = tempfile.mkdtemp(prefix="krkr2-encrypted-game-")
    try:
        for name in wanted:
            shutil.copyfile(os.path.join(src, name), os.path.join(staging, name))
        info = maker.build(staging, dst, encrypt=True, protected=True, quiet=True)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    if info is None:
        return 1

    # Independent round-trip check against the sources.
    archive = read_xp3(dst)
    sources = {name: open(os.path.join(src, name), "rb").read() for name in wanted}
    assert sorted(archive) == sorted(sources), (
        f"archive contents {sorted(archive)} != sources {sorted(sources)}")
    problems = []
    bad = check_alphabet(maker)
    if bad:
        problems.append(bad)
    for name, entry in sorted(archive.items()):
        plain = sources[name]
        if entry["flags"] & XP3_FILE_PROTECTED == 0:
            problems.append(f"{name}: TVP_XP3_FILE_PROTECTED not set")
        if entry["stored"] == plain:
            problems.append(f"{name}: stored bytes are plaintext")
        decoded = maker.apply_cipher(name, entry["stored"])
        if decoded != plain:
            problems.append(f"{name}: decoded payload differs from source")
    if problems:
        print("make-encrypted-game: FAILED", file=sys.stderr)
        for p in problems:
            print("    " + p, file=sys.stderr)
        return 1

    shutil.copyfile(dst, os.path.join(no_patch_dir, "game.xp3"))

    if not quiet:
        print(f"make-encrypted-game: {dst} ({os.path.getsize(dst)} bytes), "
              f"cipher {maker.CIPHER_NAME}, payloads stored+encrypted, "
              f"files flagged protected")
        for name in sorted(archive):
            print(f"    {name}: {len(sources[name])} bytes, "
                  f"seed=0x{maker.name_seed(name):02x}, "
                  f"stored {'plaintext' if archive[name]['stored'] == sources[name] else 'ciphertext'} "
                  f"-> decrypts to the source file")
        print("    decoded-file checksums (what the filter logs when it reads a "
              "file in full):")
        for name in sorted(sources):
            print(f"        {name}: {len(sources[name])} bytes, "
                  f"checksum={checksum(sources[name])}")
        print(f"make-encrypted-game: {os.path.join(no_patch_dir, 'game.xp3')} "
              f"(identical copy, no patch files next to it)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
