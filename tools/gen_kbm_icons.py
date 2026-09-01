#!/usr/bin/env python3
"""Generate the keyboard/mouse prompt icons — our own key-cap art in the title's
own glyph textures (part 92, native-kbm-plan phase D).

WHY THIS EXISTS. Prompt strings carry inline markup ([@x_button_ig]) that
resolves to a frontend BITMAP by name, and all 25 pad-glyph bitmaps live in ONE
bank: data/frontend/fecmn.tex (recon in docs/native-kbm-phaseA.md A.4). With the
native keyboard live (cpu/native_kbm.cpp) the prompts still show pad buttons;
this tool rebuilds fecmn.tex with the pad glyphs replaced by ORIGINAL key-cap
chip art — a dark rounded chip with a white legend, the DR2 PC style, drawn from
scratch: no Capcom art is copied, moved or imitated beyond "keyboard keys look
like keyboard keys".

THE CONTAINER, all measured on the shipped bank and gated below:
  * fecmn.tex: '06050403' header; u32 LE entry count at 0xC; 28-byte entries at
    0x18 {nameOffAbs, hash, size, 0x4030, payloadOffAbs, 4, 2}; name blob; then
    payloads. The hash is the title's own H33 name hash over the lowercase
    entry name ("x_button_ig.bct") — content swaps leave it untouched.
  * Each payload: {BE u32 decompSize, BE u32 window} then chunks of
    {BE u32 len, data}; chunk data = FF, BE u16 rawLen, BE u16 cmpLen, LZX.
  * Decompressed: 48-byte .bct-variant header (magic 05 01 01 E6, used-extent
    at +4 as two BE u16) + the texels: DXT5, 16-byte blocks in the Xenos tiled
    order (XGAddress2DTiledOffset with the width-in-blocks clamped up to 32),
    the whole payload 16-bit byte-swapped, and the tiled address taken MODULO
    the surface's block count (the y&8 bank bit wraps on surfaces half a macro
    row tall). Canvases: 16384 bytes = 128x128, 8192 = 64x64 — the WIDGET
    SAMPLES ONLY THE USED-EXTENT REGION (+4 of the header), so the art must
    fit inside (uw, uh) at the top-left.

THE LZX WE WRITE is gen_pc_options.py's real verbatim-block encoder
(lzx_encode_stream) — part 60's ladder established that the guest decoder
CRASHES on every degenerate stream (stored bytes, uncompressed blocks,
literal-only trees) even though libmspack decodes them all, and the cure is
streams statistically like the shipped ones. Every entry here is under the
encoder's 32 KB single-chunk limit, and every patched entry still round-trips
through tools/big_decompress --force (byte identity, gate 3).

THE SIZE PIN: layout.bin fixes data/frontend/fecmn.tex at 501,900 bytes and the
loader reads by that size, so the patched bank must fit UNDER it and is padded
to EXACTLY it — one layout record then serves both overlay states. How the
game reaches the loose file at all: gen_pc_options.py evicts the nested
preload4.big copy of fecmn.tex (index-hash flip), the same proven road its
fecmn.big took in part 60.

GATES (all run every time, all fatal):
  1. identity: the bank re-packed with ZERO patches must be byte-identical to
     the shipped file.
  2. hash: every patched entry's stored hash must equal H33(lowercase name).
  3. round-trip: big_decompress --force on every patched entry must return
     exactly the bytes we encoded (header + tiled swapped DXT5).

The output goes to assets/game_kbm/data/frontend/fecmn.tex — a SEPARATE overlay
from assets/game_patched, served by kernel/vfs.cpp only while the native KB/M
path is enabled (CZ_NO_KB_PROMPTS=1 keeps the pad art with the keyboard live;
CZ_NO_NATIVE_KBM=1 disables both the input path and the icons).

LEGEND CURATION (anchored on the shipped padmap's own bindings and the
generated key map — see docs/native-kbm-phaseA.md):
  menus (small):  a=ENTER  b=ESC  x=X  y=C  LB=1 RB=2 LT=3 RT=4
  gameplay (_ig): a=SPACE (jump=BUTTON_1)   b=E (use/pickup=BUTTON_2)
                  x=LMB (attack=BUTTON_3)   LB/RB=1/3 (item cycles=L1/R1)
                  LT=RMB (aim)              RT=LMB (fire)
  butstart=ENTER  butback=TAB  dpads=arrow keys  R3=MMB (heavy attack)
  analog_move_center=WASD cluster
  UNPATCHED (no keyboard equivalent bound): L3, y_button_ig.

Usage:
    python3 tools/gen_kbm_icons.py             # gates + write the patched bank
    python3 tools/gen_kbm_icons.py --preview D # also dump chip PNGs into D
"""

import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "assets/game/data/frontend/fecmn.tex"
OUT = REPO / "assets/game_kbm/data/frontend/fecmn.tex"
BIGDEC = REPO / "tools/big_decompress"
FONT = "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf"

# glyph base name -> legend spec: ("key", text) | ("mouse", button) | ("wasd",)
LEGENDS = {
    "a_button": ("key", "↵"),   # ENTER as the return symbol — the 32-px slot cannot fit the word
    "a_button_ig": ("key", "SPACE"),
    "b_button": ("key", "ESC"),
    "b_button_ig": ("key", "E"),
    "x_button": ("key", "X"),
    "x_button_ig": ("mouse", "L"),
    "y_button": ("key", "C"),
    "butstart": ("key", "ENTER"),
    "butback": ("key", "TAB"),
    "dpad_up": ("key", "↑"),
    "dpad_down": ("key", "↓"),
    "dpad_left": ("key", "←"),
    "dpad_right": ("key", "→"),
    "LBbutton": ("key", "1"),
    "LBbutton_ig": ("key", "1"),
    "RBbutton": ("key", "2"),
    "RBbutton_ig": ("key", "3"),
    "LTbutton": ("key", "3"),
    "LTbutton_ig": ("mouse", "R"),
    "RTbutton": ("key", "4"),
    "RTbutton_ig": ("mouse", "L"),
    "R3": ("mouse", "M"),
    "analog_move_center": ("wasd",),
}


def h33(s):
    v = 0
    for ch in s.lower():
        v = ((v * 33) & 0xFFFFFFFF) ^ ord(ch)
    return v


# ---- Xenos tiling / swap ----------------------------------------------------

def tiled2d(x, y, wu, l2b):
    macro = ((x >> 5) + (y >> 5) * (wu >> 5)) << (l2b + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << l2b
    off = (macro + ((micro & ~15) << 1) + (micro & 15) +
           ((y & 8) << (3 + l2b)) + ((y & 1) << 4))
    return ((((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) +
             ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> l2b)


def swap16(b):
    out = bytearray(len(b))
    out[0::2] = b[1::2]
    out[1::2] = b[0::2]
    return bytes(out)


# ---- DXT5 encode ------------------------------------------------------------

def pack565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def dxt5_encode_block(px):
    """px: 16 (r,g,b,a). Simple two-endpoint quantizer — plenty for flat chips."""
    a_vals = [p[3] for p in px]
    a0, a1 = max(a_vals), min(a_vals)
    if a0 == a1:
        # avoid the a0<=a1 special palette; nudge so the 8-interp form is used
        a0 = min(255, a0 + 1) if a0 < 255 else a0
        a1 = max(0, a1 - 1) if a0 == a1 else a1
    apal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    abits = 0
    for i, a in enumerate(a_vals):
        best = min(range(8), key=lambda k: abs(apal[k] - a))
        abits |= best << (3 * i)
    ablock = bytes([a0, a1]) + abits.to_bytes(6, "little")

    lum = [(p[0] * 3 + p[1] * 6 + p[2]) for p in px]
    hi = px[lum.index(max(lum))][:3]
    lo = px[lum.index(min(lum))][:3]
    c0, c1 = pack565(*hi), pack565(*lo)
    if c0 == c1:
        pal = [hi, lo, hi, lo]
        if c0 == 0:
            c0 = 1
        else:
            c1 = c0 - 1
        order = (c0, c1)
    else:
        if c0 < c1:
            c0, c1 = c1, c0
            hi, lo = lo, hi
        order = (c0, c1)
    pal = [hi, lo,
           tuple((2 * hi[i] + lo[i]) // 3 for i in range(3)),
           tuple((hi[i] + 2 * lo[i]) // 3 for i in range(3))]
    cbits = 0
    for i, p in enumerate(px):
        best = min(range(4), key=lambda k: sum((pal[k][j] - p[j]) ** 2 for j in range(3)))
        cbits |= best << (2 * i)
    cblock = struct.pack("<HH", order[0], order[1]) + cbits.to_bytes(4, "little")
    return ablock + cblock


def encode_dxt5_tiled(img, storage_blocks):
    """DXT5 blocks in the bank's tiled order: XGAddress2DTiledOffset with the
    pitch clamped up to 32 blocks, TAKEN MODULO THE STORAGE'S BLOCK COUNT — the
    64x64 glyphs live in a 512-slot (8192-byte) storage where the address's
    y&8 bank bit lands the 256 image blocks across both halves; verified
    against the shipped a_button (the mod-512 decode is the one that renders
    its green A cleanly, and the mapping is bijective). The storage size is
    the ORIGINAL entry's, preserved exactly."""
    w, h = img.size
    bw, bh = w // 4, h // 4
    out = bytearray(storage_blocks * 16)
    pix = img.load()
    wu = max(bw, 32)
    seen = set()
    for by in range(bh):
        for bx in range(bw):
            block = [pix[bx * 4 + i % 4, by * 4 + i // 4] for i in range(16)]
            ti = tiled2d(bx, by, wu, 4) % storage_blocks
            assert ti not in seen, "tiled mapping collision"
            seen.add(ti)
            out[ti * 16:ti * 16 + 16] = dxt5_encode_block(block)
    return swap16(bytes(out))


# ---- the LZX encoder: gen_pc_options.py's, imported --------------------------
import importlib.util as _ilu

_spec = _ilu.spec_from_file_location(
    "gen_pc_options", Path(__file__).resolve().parent / "gen_pc_options.py")
_gpo = _ilu.module_from_spec(_spec)
_spec.loader.exec_module(_gpo)


def make_entry_payload(raw, window):
    assert window == 0x8000, "the encoder emits 0x8000-window streams"
    return _gpo.lzx_encode_stream(raw)


# ---- the chip art ------------------------------------------------------------

def rounded(dr, box, rad, fill, outline=None, width=1):
    dr.rounded_rectangle(box, radius=rad, fill=fill, outline=outline, width=width)


def draw_key_chip(size, text):
    """A dark rounded key cap with a white legend — the DR2 PC style, ours."""
    w, h = size
    img = Image.new("RGBA", size, (0, 0, 0, 0))
    dr = ImageDraw.Draw(img)
    m = max(2, h // 16)
    rounded(dr, (m, m, w - m, h - m), h // 5, (28, 28, 32, 235),
            outline=(200, 200, 205, 255), width=max(2, h // 24))
    # inner highlight edge, like a key cap's top face
    m2 = m + max(2, h // 14)
    rounded(dr, (m2, m2, w - m2, h - m2 - h // 10), h // 6, (52, 52, 58, 255))
    fs = h
    while fs > 8:
        font = ImageFont.truetype(FONT, fs)
        tw = dr.textlength(text, font=font)
        if tw <= (w - 2 * m2) * 0.86 and fs <= (h - 2 * m2) * 1.1:
            break
        fs -= 2
    bbox = font.getbbox(text)
    tx = (w - dr.textlength(text, font=font)) / 2
    ty = (h - (bbox[3] - bbox[1])) / 2 - bbox[1]
    dr.text((tx, ty - h * 0.04), text, font=font, fill=(240, 240, 240, 255))
    return img


def draw_mouse_chip(size, button):
    """A mouse silhouette with the named button lit: L / R / M."""
    w, h = size
    img = Image.new("RGBA", size, (0, 0, 0, 0))
    dr = ImageDraw.Draw(img)
    mw, mh = int(w * 0.56), int(h * 0.86)
    x0, y0 = (w - mw) // 2, (h - mh) // 2
    body = (x0, y0, x0 + mw, y0 + mh)
    dr.rounded_rectangle(body, radius=mw // 2, fill=(28, 28, 32, 235),
                         outline=(200, 200, 205, 255), width=max(2, h // 24))
    midy = y0 + mh * 0.42
    dr.line((x0, midy, x0 + mw, midy), fill=(200, 200, 205, 255), width=2)
    dr.line((x0 + mw / 2, y0, x0 + mw / 2, midy), fill=(200, 200, 205, 255), width=2)
    hot = (250, 250, 250, 255)
    if button == "L":
        dr.pieslice((x0, y0, x0 + mw, y0 + int(mh * 0.84)), 180, 270, fill=hot)
    elif button == "R":
        dr.pieslice((x0, y0, x0 + mw, y0 + int(mh * 0.84)), 270, 360, fill=hot)
    else:
        ww, wh = mw // 5, int(mh * 0.30)
        dr.rounded_rectangle((x0 + (mw - ww) // 2, y0 + mh * 0.08,
                              x0 + (mw + ww) // 2, y0 + mh * 0.08 + wh),
                             radius=ww // 2, fill=hot)
    dr.line((x0, midy, x0 + mw, midy), fill=(200, 200, 205, 255), width=2)
    dr.line((x0 + mw / 2, y0, x0 + mw / 2, midy), fill=(200, 200, 205, 255), width=2)
    return img


def draw_wasd_chip(size):
    w, h = size
    img = Image.new("RGBA", size, (0, 0, 0, 0))
    k = min(w, h) // 2 - 2
    for ch, (cx, cy) in {"W": (w // 2 - k // 2, 0), "A": (w // 2 - k - k // 2, h - k - 1),
                         "S": (w // 2 - k // 2, h - k - 1), "D": (w // 2 + k // 2, h - k - 1)}.items():
        img.alpha_composite(draw_key_chip((k, k), ch), (int(cx), int(cy)))
    return img


# ---- the bank ---------------------------------------------------------------

def parse_bank(data):
    count = struct.unpack_from("<I", data, 0xC)[0]
    entries = []
    for i in range(count):
        v = list(struct.unpack_from("<7I", data, 0x18 + 28 * i))
        e = data.index(b"\0", v[0])
        entries.append({"name": data[v[0]:e].decode(), "rec": v})
    return count, entries


def rebuild(data, entries, patches):
    """Rebuild the bank: entry table + name blob byte-identical, payloads
    re-laid-out in the original order with patched ones substituted."""
    count = len(entries)
    order = sorted(range(count), key=lambda i: entries[i]["rec"][4])
    first_pay = entries[order[0]]["rec"][4]
    out = bytearray(data[:first_pay])
    for i in order:
        e = entries[i]
        pay = patches.get(e["name"])
        if pay is None:
            pay = data[e["rec"][4]:e["rec"][4] + e["rec"][2]]
        while len(out) & 3:          # payloads are 4-byte aligned in the bank
            out.append(0)
        newoff = len(out)
        out += pay
        struct.pack_into("<7I", out, 0x18 + 28 * i,
                         e["rec"][0], e["rec"][1], len(pay), e["rec"][3],
                         newoff, e["rec"][5], e["rec"][6])
    struct.pack_into("<I", out, 0x8, len(out))
    return bytes(out)


def decompress_entry(payload):
    with tempfile.NamedTemporaryFile(suffix=".cmp", delete=False) as f:
        f.write(payload)
        cmp_path = f.name
    dec_path = cmp_path + ".dec"
    r = subprocess.run([str(BIGDEC), "--force", cmp_path, dec_path],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"big_decompress failed: {r.stdout}{r.stderr}")
    out = Path(dec_path).read_bytes()
    Path(cmp_path).unlink()
    Path(dec_path).unlink()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", metavar="DIR", help="also write chip PNGs here")
    args = ap.parse_args()

    data = SRC.read_bytes()
    count, entries = parse_bank(data)

    # GATE 1: identity repack.
    ident = rebuild(data, entries, {})
    if ident != data:
        print("GATE 1 FAILED: zero-patch rebuild is not byte-identical — the "
              "container model is wrong, refusing to write anything",
              file=sys.stderr)
        sys.exit(1)

    patches = {}
    previews = []
    for e in entries:
        base = e["name"].rsplit(".", 1)[0]
        spec = LEGENDS.get(base)
        if spec is None:
            continue
        # GATE 2: the stored hash must be the H33 name hash.
        if e["rec"][1] != h33(e["name"]):
            print(f"GATE 2 FAILED: {e['name']} hash {e['rec'][1]:08X} != "
                  f"H33 {h33(e['name']):08X}", file=sys.stderr)
            sys.exit(1)
        old = decompress_entry(data[e["rec"][4]:e["rec"][4] + e["rec"][2]])
        hdr, texels = old[:48], old[48:]
        if hdr[:4] != b"\x05\x01\x01\xE6":
            print(f"GATE FAILED: {e['name']} is not the 05 01 01 E6 texture "
                  f"record this tool understands", file=sys.stderr)
            sys.exit(1)
        w, h = (128, 128) if len(texels) == 16384 else (64, 64)
        uw, uh = struct.unpack(">HH", hdr[4:8])
        # THE WIDGET SAMPLES ONLY THE USED-EXTENT REGION of the canvas — the
        # first build drew chips at extent x2 and the operator's F9 showed
        # exactly the top-left quarter of one. Art fits INSIDE (uw, uh).
        canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        aw = min(w, max(8, int(uw * 0.96)))
        ah = min(h, max(8, int(uh * 0.96)))
        if spec[0] == "key":
            text = spec[1]
            chip = draw_key_chip((aw, ah), text)
        elif spec[0] == "mouse":
            chip = draw_mouse_chip((aw, ah), spec[1])
        else:
            chip = draw_wasd_chip((aw, ah))
        canvas.alpha_composite(chip, (1, 1))
        if args.preview:
            previews.append((base, canvas.copy()))
        raw = hdr + encode_dxt5_tiled(canvas, len(texels) // 16)
        window = struct.unpack(">I", data[e["rec"][4] + 4:e["rec"][4] + 8])[0]
        pay = make_entry_payload(raw, window)
        # GATE 3: round-trip through the real decompressor.
        back = decompress_entry(pay)
        if back != raw:
            print(f"GATE 3 FAILED: {e['name']} round-trip mismatch "
                  f"({len(back)} vs {len(raw)} bytes)", file=sys.stderr)
            sys.exit(1)
        patches[e["name"]] = pay

    if args.preview:
        pdir = Path(args.preview)
        pdir.mkdir(parents=True, exist_ok=True)
        for base, img in previews:
            img.save(pdir / f"{base}.png")

    # THE TITLE-SCREEN STRING: with the keyboard live the title should say
    # PRESS ENTER, the way DR2 PC's shell does. Two spellings ship — a
    # PRESS\0START id pair (the title screen) and one "PRESS START" — and both
    # replacements are SAME-LENGTH in-place edits of the game_patched bank (the
    # str banks are layout-pinned like everything else), served from this
    # overlay only while the keyboard is the input path.
    sbank = (REPO / "assets/game_patched/data/frontend/str_en.bcs").read_bytes()
    for old, new in ((b"PRESS\x00START\x00", b"PRESS\x00ENTER\x00"),
                     (b"PRESS START\x00", b"PRESS ENTER\x00")):
        n = sbank.count(old)
        if n != 1:
            print(f"GATE FAILED: str_en.bcs holds {n} of {old!r}, expected 1",
                  file=sys.stderr)
            sys.exit(1)
        sbank = sbank.replace(old, new)
    sout = OUT.parent / "str_en.bcs"
    OUT.parent.mkdir(parents=True, exist_ok=True)
    sout.write_bytes(sbank)
    print(f"wrote {sout} (2 same-length PRESS ENTER edits)")

    out = rebuild(data, entries, patches)
    # THE SIZE PIN (see the module comment): the loader reads this file at
    # layout.bin's recorded size — the shipped 501,900 — so the patched bank
    # must fit under it and is padded to exactly it.
    if len(out) > len(data):
        print(f"GATE FAILED: patched bank {len(out)} bytes exceeds the shipped "
              f"{len(data)} that layout.bin pins — refusing to write",
              file=sys.stderr)
        sys.exit(1)
    padded = bytearray(out)
    padded += b"\0" * (len(data) - len(out))
    struct.pack_into("<I", padded, 0x8, len(padded))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(bytes(padded))
    print(f"gates passed; {len(patches)} of {count} entries patched; "
          f"wrote {OUT} ({len(out)} logical, padded to the shipped "
          f"{len(data)})")


if __name__ == "__main__":
    main()
