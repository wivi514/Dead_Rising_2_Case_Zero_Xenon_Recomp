#!/usr/bin/env python3
"""Decrypt assets/game/default.xex (retail AES key) and expose a rigorous
guest-address <-> decrypted-stream-offset mapping across ALL of the XEX's
block-table ranges (Fable2 has 4 blocks, not 2 -- see the note below).

!! DOES NOT WORK ON DEAD RISING 2: CASE ZERO. Kept because other tools import its
!! address-mapping helpers, and because it is the right tool for a retail-key,
!! basic-compression XEX -- which is what both template ports had. Case Zero's XEX is
!! devkit-key encrypted (all-zero key, not the retail key below) and uses
!! `compression = 2` (LZX), and BOTH failures are silent here: the retail key yields
!! noise, and this script parses a normal-compression FileFormatInfo as a basic block
!! table, producing a confident and entirely fictional block list
!! (`data=0x5E752CD6 zero=0x0CA60E7D` -- that is a SHA-1 fragment, not a size).
!! Use `tools/xex_image_dump` instead; see docs/bootstrap-2026-08-04.md §2.
!!
!! Provenance: copied verbatim from ~/GithubRepo/Asuras_Wrath_Xenon_Recomp/tools,
!! which took it from Fable2XenonRecomp.

Usage:
    python3 decrypt_xex.py <path/to/default.xex> [out.bin]

Writes the full decrypted stream to out.bin (default: decrypted.bin next to
this script) and prints the block-table ranges. Import guest_to_dec()/
dec_to_guest() from this module for address lookups in other scripts.

Recipe (see project memory reference-fable2-bnk-format's sibling note,
pitfall-xex-encrypted, for the full writeup): retail XEX2 files use a fixed
retail key (not a per-console CPU key), so offline decryption works without
needing the console's key vault.

IMPORTANT: an earlier ad-hoc version of this script (used informally across
several sessions) only handled 2 of the XEX's 4 block-table ranges, silently
mis-mapping any guest address at or above roughly 0x832D0000 (queried using
a fabricated 2-range formula instead of parsing the real block table). This
version parses the REAL block table from the XEX header and is correct for
the full address range. If you're about to declare "not found" for an
address search, make sure you're using THIS script's mapping, not a
hand-rolled shortcut.
"""
import struct
import sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
IMAGE_BASE = 0x82000000
PE_DATA_OFFSET = 0x4000


def decrypt(xex_path):
    raw = open(xex_path, "rb").read()

    header_count = struct.unpack_from(">I", raw, 8)[0]
    opt_table_off = 24
    ffi_off = None
    for i in range(header_count):
        key, val = struct.unpack_from(">II", raw, opt_table_off + i * 8)
        if key == 0x3FF:  # XEX_FILE_FORMAT_INFO
            ffi_off = val
    if ffi_off is None:
        raise ValueError("XEX_FILE_FORMAT_INFO header entry not found")

    sec_info_off = struct.unpack_from(">I", raw, 16)[0]
    title_key_enc = raw[sec_info_off + 0x150: sec_info_off + 0x160]
    cipher = Cipher(algorithms.AES(RETAIL_KEY), modes.ECB())
    d = cipher.decryptor()
    title_key = d.update(title_key_enc) + d.finalize()

    cipher2 = Cipher(algorithms.AES(title_key), modes.CBC(b"\x00" * 16))
    d2 = cipher2.decryptor()
    ciphertext = raw[PE_DATA_OFFSET:]
    ciphertext = ciphertext[:len(ciphertext) - (len(ciphertext) % 16)]
    decrypted = d2.update(ciphertext) + d2.finalize()

    size, enc_type, comp_type = struct.unpack_from(">IHH", raw, ffi_off)
    pos = ffi_off + 8
    end = ffi_off + size
    blocks = []
    while pos < end:
        data_size, zero_size = struct.unpack_from(">II", raw, pos)
        pos += 8
        if data_size == 0:
            break
        blocks.append((data_size, zero_size))

    ranges = []
    image_off = 0
    dec_off = 0
    for data_size, zero_size in blocks:
        ranges.append((image_off, image_off + data_size, dec_off))
        image_off += data_size + zero_size
        dec_off += data_size

    return decrypted, ranges


def make_converters(ranges):
    def guest_to_dec(guest_addr):
        image_off = guest_addr - IMAGE_BASE
        for start, dend, dstart in ranges:
            if start <= image_off < dend:
                return dstart + (image_off - start)
        return None  # falls in a zero-padding gap or outside all blocks

    def dec_to_guest(dec_off):
        for start, dend, dstart in ranges:
            dsize = dend - start
            if dstart <= dec_off < dstart + dsize:
                return IMAGE_BASE + start + (dec_off - dstart)
        return None

    return guest_to_dec, dec_to_guest


if __name__ == "__main__":
    xex_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else "decrypted.bin"
    decrypted, ranges = decrypt(xex_path)
    with open(out_path, "wb") as f:
        f.write(decrypted)
    print(f"wrote {len(decrypted)} bytes to {out_path}")
    print("block ranges (image_start, image_data_end, dec_start):")
    for r in ranges:
        print(f"  image=0x{r[0]:X}-0x{r[1]:X} -> dec_start=0x{r[2]:X}")

    guest_to_dec, dec_to_guest = make_converters(ranges)
    test_addr = 0x832B3EB8  # BinkOpen, known-good verification target
    doff = guest_to_dec(test_addr)
    if doff is not None:
        print(f"verify: guest 0x{test_addr:08X} -> dec 0x{doff:X} "
              f"bytes={decrypted[doff:doff+4].hex()} (expect 7d8802a6)")
