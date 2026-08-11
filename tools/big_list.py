#!/usr/bin/env python3
"""List or search the contents of the engine's `.big` archives.

WHY THIS EXISTS
---------------
`docs/big-archive-format.md` cracked the container in phase 1, and for a year the only
consumers of that knowledge were the runtime's loader and a hex editor. Part 27 wanted a
question answered — is `data/misc/textures/cc_03.bct` inside an archive? — and reached for
`grep -rl cc_0 *.big`, which returned zero. **So did grepping for `meat`, `zombie` and
`chuck`, names that are certainly in there** — `grep` on these binaries is a filter that
cannot match (gotcha 25). The negative result meant nothing, and the only way to make it
mean something is to read the table. It does exist: `cc_03.bct` is 71,580 bytes inside
`data/streamedassets.big`, and `meat` matches 12 entries where the grep found none.

146 archives ship in this package holding 12,481 entries, so "is asset X anywhere" is a
question worth asking in one command rather than one archive at a time.

WHAT IT VERIFIES BEFORE IT BELIEVES A PARSE
--------------------------------------------
The format doc records a specific way this goes wrong: an earlier parse assumed a 40-byte
index stride, which makes `0x18 + count*40 == data_start` come out right for ONE archive
and wrong for the other six. So the stride is taken from `names_offset`, which is in the
header, and two self-consistency checks run per file before any entry is printed:

    0x18 + entry_count*28 == names_offset
    total_size            == the actual file size

An archive failing either is REPORTED, not skipped silently — a reader that quietly drops
what it cannot parse turns "the asset is not here" into "the asset is not anywhere", which
is the same mistake the grep made. That report earned itself immediately: the first version
also enforced the doc's fixed-width name table and **95 of 146 archives failed**, which is
how the doc's claim came to be corrected rather than the archives being written off.

USAGE
    big_list.py <archive.big> [...]              list entries
    big_list.py --find cc_ assets/game/...       search names (case-insensitive substring)
    big_list.py --find sun.bct --all             search every .big under assets/game
    big_list.py --extract <name> --out DIR ...   write matching payloads out
"""
import argparse
import glob
import os
import struct
import sys

HDR = struct.Struct('<6I')          # magic, data_start, total_size, count, hdr_size, names
ENT = struct.Struct('<7I')          # name_off, hash, size, size2, data_off, flags, resv
MAGIC = bytes([0x06, 0x05, 0x04, 0x03])


class BadArchive(Exception):
    pass


def read_toc(path):
    """-> (entries, info). Raises BadArchive with a reason rather than returning junk."""
    size = os.path.getsize(path)
    with open(path, 'rb') as f:
        head = f.read(HDR.size)
        if len(head) < HDR.size:
            raise BadArchive('shorter than a header')
        if head[:4] != MAGIC:
            raise BadArchive('magic is %s, not 06 05 04 03' % head[:4].hex(' '))
        magic, data_start, total, count, hdr_size, names_off = HDR.unpack(head)
        if not count:
            raise BadArchive('entry_count is 0')
        # The three checks from docs/big-archive-format.md, run BEFORE trusting anything.
        if 0x18 + count * 28 != names_off:
            raise BadArchive('0x18 + %d*28 = %#x but names_offset is %#x'
                             % (count, 0x18 + count * 28, names_off))
        if total != size:
            raise BadArchive('header says %d bytes, file is %d' % (total, size))
        span = data_start - names_off
        if span < 0:
            raise BadArchive('name table has negative length')
        # NAMES ARE NUL-TERMINATED AT `name_offset`, NOT FIXED WIDTH.
        #
        # docs/big-archive-format.md says fixed-width, NUL-padded, width computed as
        # `(data_start - names_offset) / entry_count`. That is true of the SHADER banks,
        # which is where the format was cracked, and false of most of the rest: on the
        # first run of this reader **95 of 146 archives failed that divisibility check**
        # (`charvocals.big`, 9,843 bytes over 985 entries). Their names are simply
        # variable-length.
        #
        # Reading to the NUL from each entry's own `name_offset` handles both, and needs
        # no width at all — the fixed-width archives are the case where every name happens
        # to be padded to the same stride. The width is still computed when it divides,
        # purely to report it.
        width = span // count if count and span % count == 0 else 0
        f.seek(0x18)
        raw = f.read(count * 28)
        f.seek(names_off)
        table = f.read(span)
        out = []
        for i in range(count):
            no, h, sz, sz2, off, flags, _ = ENT.unpack_from(raw, i * 28)
            rel = no - names_off
            name = ''
            if 0 <= rel < len(table):
                end = table.find(b'\0', rel)
                name = table[rel:end if end >= 0 else len(table)].decode('latin-1')
            out.append({'name': name, 'hash': h, 'size': sz, 'size2': sz2,
                        'offset': off, 'flags': flags})
        return out, {'count': count, 'width': width, 'data_start': data_start,
                     'total': total}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('archives', nargs='*')
    ap.add_argument('--all', action='store_true',
                    help='every .big under assets/game')
    ap.add_argument('--find', help='case-insensitive substring of an entry name')
    ap.add_argument('--extract', help='write out entries whose name matches this')
    ap.add_argument('--out', default='.')
    ap.add_argument('--limit', type=int, default=40)
    args = ap.parse_args()

    paths = list(args.archives)
    if args.all:
        paths += sorted(glob.glob('assets/game/**/*.big', recursive=True))
    if not paths:
        ap.error('give an archive, or --all')

    total_entries = 0
    bad = []
    hits = 0
    for p in paths:
        try:
            entries, info = read_toc(p)
        except BadArchive as e:
            bad.append((p, str(e)))
            continue
        total_entries += len(entries)
        if args.find or args.extract:
            needle = (args.find or args.extract).lower()
            for e in entries:
                if needle not in e['name'].lower():
                    continue
                hits += 1
                print('%-52s %-16s %9d bytes  @%#x  hash %08X%s'
                      % (os.path.relpath(p), e['name'], e['size'], e['offset'],
                         e['hash'],
                         '  COMPRESSED -> %d' % e['size2']
                         if e['size'] != e['size2'] else ''))
                if args.extract:
                    with open(p, 'rb') as f:
                        f.seek(e['offset'])
                        blob = f.read(e['size'])
                    # SAY SO when what lands on disk is not the asset. 1,671 of this
                    # package's 12,481 entries are compressed, and writing one out under
                    # its own name without a word would hand the next reader a file that
                    # looks like a .bct, is named like a .bct, and is not one.
                    if e['size'] != e['size2']:
                        print('    NOTE: this entry is COMPRESSED (%d -> %d). What is '
                              'written out is the compressed stream, not the asset: a BE '
                              'u32 uncompressed size, a BE u32 32 KB window, then '
                              '[BE u32 length][LZX block] chunks each opening 0xFF.'
                              % (e['size'], e['size2']))
                    os.makedirs(args.out, exist_ok=True)
                    dst = os.path.join(args.out, e['name'] or ('%08X.bin' % e['hash']))
                    open(dst, 'wb').write(blob)
                    print('    -> %s (%d bytes)' % (dst, len(blob)))
        else:
            print('%s: %d entries, name width %d, payload at %#x'
                  % (os.path.relpath(p), info['count'],
                     info['width'] or -1, info['data_start']))
            for e in entries[:args.limit]:
                print('    %-16s %9d bytes  @%#x  hash %08X%s'
                      % (e['name'], e['size'], e['offset'], e['hash'],
                         '  COMPRESSED -> %d' % e['size2']
                         if e['size'] != e['size2'] else ''))
            if len(entries) > args.limit:
                print('    ... and %d more' % (len(entries) - args.limit))

    print('\n%d archives, %d entries parsed' % (len(paths) - len(bad), total_entries))
    if args.find or args.extract:
        print('%d entries matched %r' % (hits, args.find or args.extract))
    # Named, never silent: an archive this cannot read is exactly where the answer would
    # hide, and "0 matches" from a run that failed to open half its inputs is the failure
    # this tool was written to replace.
    if bad:
        print('\n%d archives DID NOT PARSE — the search above cannot speak for these:'
              % len(bad))
        for p, why in bad[:15]:
            print('   %-52s %s' % (os.path.relpath(p), why))
        if len(bad) > 15:
            print('   ... and %d more' % (len(bad) - 15))
    return 0


if __name__ == '__main__':
    sys.exit(main())
