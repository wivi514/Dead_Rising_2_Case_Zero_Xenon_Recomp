// Decompress a compressed `.big` entry, using XenonRecomp's own LZX decoder.
//
// WHY THIS EXISTS
// ---------------
// Part 27 found that 1,671 of this package's 12,481 archive entries are compressed
// (`size != size2`), which `docs/big-archive-format.md` had recorded as "equal
// everywhere" from a survey of the seven shader banks. `tools/big_list.py --extract`
// writes the compressed stream and says so; this turns it into the asset.
//
// It links the SAME `lzxDecompress` the recompiler uses to unpack this title's XEX,
// rather than vendoring a decoder, for the same reason `xex_image_dump` does: a second
// implementation of a decompressor is a second thing that can be subtly wrong, and there
// is no oracle for "the bytes are right" except the data itself.
//
// THE STREAM, and how it was established
// --------------------------------------
// Parsed exactly on `cc_03.bct` — 71,580 of 71,580 bytes over five chunks with nothing
// left over, which is what says a guessed layout is right rather than merely plausible:
//
//     BE u32  uncompressed size    (equals the index entry's `size2`)
//     BE u32  window size          (0x8000 = 32 KB everywhere seen)
//     repeated: BE u32 chunk length, then the chunk, which opens 0xFF
//
// The container's header and index are LITTLE-endian and this stream is BIG-endian. Both
// are true at once; a reader that picks one for the whole file gets a plausible size and
// nonsense chunks.
//
// THE ORACLE
// ----------
// We know what correct output looks like: the loose `.bct` files on disc
// (`data/system/greysqr.bct`, `data/misc/meat.bct`) all begin `05 01 01 E2`. So this does
// not ask the user to eyeball the result — it checks the magic and the length against the
// entry's `size2`, and says which interpretation of the chunk framing produced them. A
// decompressor that returns garbage confidently is worse than one that fails.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "xex_patcher.h"

namespace {

uint32_t BE32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t BE16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

std::vector<uint8_t> Read(const char* path)
{
    std::vector<uint8_t> out;
    FILE* f = fopen(path, "rb");
    if (!f)
        return out;
    fseek(f, 0, SEEK_END);
    out.resize(size_t(ftell(f)));
    fseek(f, 0, SEEK_SET);
    if (fread(out.data(), 1, out.size(), f) != out.size())
        out.clear();
    fclose(f);
    return out;
}

bool LooksLikeBct(const std::vector<uint8_t>& d)
{
    return d.size() >= 4 && d[0] == 0x05 && d[1] == 0x01 && d[2] == 0x01 && d[3] == 0xE2;
}

// Part 59 extended the oracle: the frontend .big archives carry compressed TEXT
// entries (screen layouts like options_pc.txt), which decompress perfectly and then
// failed the .bct magic check — the tool refused to write 33,816 correct bytes. The
// oracle for a text entry is that it IS text: >= 95% printable-or-whitespace over the
// whole output. Binary garbage from a wrong framing fails this immediately, so the
// "never emit garbage confidently" property survives.
bool LooksLikeText(const std::vector<uint8_t>& d)
{
    if (d.empty())
        return false;
    size_t printable = 0;
    for (uint8_t c : d)
        if (c == 9 || c == 10 || c == 13 || (c >= 32 && c < 127))
            ++printable;
    return printable * 100 >= d.size() * 95;
}

// Part 60 widened the oracle again: preload4.big NESTS whole .big archives as
// compressed entries (fecmn.big — how the frontend layouts actually load), and a
// decompressed nested archive is neither a .bct nor text. Its own magic is the check.
bool LooksLikeBig(const std::vector<uint8_t>& d)
{
    return d.size() >= 4 && d[0] == 0x06 && d[1] == 0x05 && d[2] == 0x04 && d[3] == 0x03;
}

bool OracleOk(const std::vector<uint8_t>& d)
{
    return LooksLikeBct(d) || LooksLikeText(d) || LooksLikeBig(d);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr,
                "usage: big_decompress <compressed-entry> <out> [expected-size]\n"
                "  the input is what `tools/big_list.py --extract` wrote\n");
        return 2;
    }
    // --force (part 92): write the decompressed bytes even when the .bct oracle
    // fails, SAYING SO. The frontend .tex banks (fecmn.tex etc.) carry the same
    // LZX framing around texture records that are NOT .bct — the oracle is right
    // to refuse by default and right to be overridable by an explicit ask.
    bool force = false;
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--force") == 0)
        {
            force = true;
            for (int j = i; j + 1 < argc; ++j)
                argv[j] = argv[j + 1];
            --argc;
            break;
        }
    std::vector<uint8_t> in = Read(argv[1]);
    if (in.size() < 16)
    {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    const uint32_t uncompressed = BE32(in.data());
    const uint32_t window = BE32(in.data() + 4);
    printf("%s: %zu bytes stored, header says %u uncompressed, %u-byte window\n",
           argv[1], in.size(), uncompressed, window);
    if (argc > 3 && uint32_t(atoi(argv[3])) != uncompressed)
        printf("  WARNING: the archive index said %s, the stream says %u\n",
               argv[3], uncompressed);

    // Walk the chunks once, so the framing is reported before anything is decoded.
    std::vector<std::pair<size_t, uint32_t>> chunks;   // offset of payload, length
    for (size_t o = 8; o + 4 <= in.size();)
    {
        const uint32_t len = BE32(in.data() + o);
        if (!len || len > in.size() - o - 4)
            break;
        chunks.emplace_back(o + 4, len);
        o += 4 + len;
    }
    printf("  %zu chunks\n", chunks.size());

    // TWO INTERPRETATIONS OF THE CHUNK, tried in order and each checked against the
    // oracle. Guessing one and shipping it is how a wrong decode becomes a "finding".
    //
    //  A) every chunk payload is a self-contained LZX stream after a 5-byte
    //     `0xFF u16 uncompressed u16 compressed` header — the XMemCompress framing;
    //  B) the chunk payloads concatenate into ONE LZX stream, which is how
    //     XenonUtils/xex.cpp feeds this same decoder for the XEX.
    std::vector<uint8_t> out;

    // A
    out.assign(uncompressed, 0);
    size_t produced = 0;
    bool okA = !chunks.empty();
    for (auto& [off, len] : chunks)
    {
        if (len < 5 || in[off] != 0xFF) { okA = false; break; }
        const uint32_t rawLen = BE16(in.data() + off + 1);
        const uint32_t cmpLen = BE16(in.data() + off + 3);
        if (cmpLen > len - 5 || produced + rawLen > out.size()) { okA = false; break; }
        if (lzxDecompress(in.data() + off + 5, cmpLen, out.data() + produced, rawLen,
                          window, nullptr, 0) != 0)
        { okA = false; break; }
        produced += rawLen;
    }
    if (okA && produced == uncompressed && OracleOk(out))
        printf("  per-chunk LZX (0xFF u16 u16 header): OK, %zu bytes, .bct magic present\n",
               produced);
    else if (force && okA && produced == uncompressed)
        printf("  per-chunk LZX decoded %zu bytes but the .bct oracle FAILED — writing "
               "anyway (--force)\n", produced);
    else
    {
        printf("  per-chunk LZX: no (%zu of %u bytes%s)\n", produced, uncompressed,
               produced ? (OracleOk(out) ? "" : ", magic wrong") : "");
        okA = false;
    }

    if (!okA)
    {
        // B
        std::vector<uint8_t> cat;
        for (auto& [off, len] : chunks)
            cat.insert(cat.end(), in.begin() + off, in.begin() + off + len);
        out.assign(uncompressed, 0);
        const int rc = lzxDecompress(cat.data(), cat.size(), out.data(), uncompressed,
                                     window, nullptr, 0);
        if (rc == 0 && OracleOk(out))
            printf("  concatenated LZX (the XEX loader's shape): OK, .bct magic present\n");
        else if (force && rc == 0)
            printf("  concatenated LZX decoded but the .bct oracle FAILED — writing "
                   "anyway (--force)\n");
        else
        {
            printf("  concatenated LZX: no (rc=%d%s)\n", rc,
                   OracleOk(out) ? "" : ", magic wrong");
            fprintf(stderr,
                    "NEITHER framing produced a valid .bct. Nothing written — a "
                    "decompressor that emits garbage confidently is worse than one that "
                    "fails.\n");
            return 1;
        }
    }

    FILE* f = fopen(argv[2], "wb");
    if (!f)
    {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    printf("wrote %s (%zu bytes)\n", argv[2], out.size());
    return 0;
}
