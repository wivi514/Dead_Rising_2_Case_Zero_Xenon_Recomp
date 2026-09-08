// See png_icon.h for why a PNG decoder lives here at all.
//
// Structure, top to bottom: a bit reader, the two canonical-Huffman tables DEFLATE
// needs, the block loop (stored / fixed / dynamic), then PNG's chunk walk, the zlib
// wrapper check, and the five scanline filters. Every "cannot happen for a valid file"
// branch returns false rather than trusting the input: this reads a file the player put
// on disk, and a corrupt one must cost an icon, not a process.
#include "png_icon.h"

#include "host_paths.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace
{
// ---------------------------------------------------------------------------- inflate
struct BitReader
{
    const uint8_t* p;
    size_t n, pos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool overrun = false;

    // DEFLATE packs bits LSB-first within a byte; Huffman codes are read one bit at a
    // time in that order, which is why the tables below are walked bit by bit rather
    // than indexed by a reversed code.
    uint32_t Bits(int count)
    {
        while (bitcnt < count)
        {
            if (pos >= n) { overrun = true; return 0; }
            bitbuf |= uint32_t(p[pos++]) << bitcnt;
            bitcnt += 8;
        }
        const uint32_t v = bitbuf & ((1u << count) - 1u);
        bitbuf >>= count;
        bitcnt -= count;
        return v;
    }
    void AlignToByte() { bitbuf = 0; bitcnt = 0; }
};

// Canonical Huffman decode table in the zlib "puff" shape: count of codes per length,
// then the symbols sorted by (length, value). Decoding walks lengths 1..15 comparing the
// accumulated code against the running first-code — no lookup table, which is fine for
// an 11 KB file read once.
struct Huffman
{
    uint16_t count[16] = {};
    uint16_t symbol[320] = {};

    bool Build(const uint8_t* lengths, int nsym)
    {
        memset(count, 0, sizeof count);
        for (int s = 0; s < nsym; ++s) ++count[lengths[s]];
        if (count[0] == nsym) return false; // no codes at all
        // an over-subscribed set of lengths is a corrupt stream
        int left = 1;
        for (int len = 1; len < 16; ++len)
        {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;
        }
        uint16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = uint16_t(offs[len] + count[len]);
        for (int s = 0; s < nsym; ++s)
            if (lengths[s]) symbol[offs[lengths[s]]++] = uint16_t(s);
        return true;
    }

    int Decode(BitReader& br) const
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len)
        {
            code |= int(br.Bits(1));
            const int cnt = count[len];
            if (code - cnt < first) return symbol[index + (code - first)];
            index += cnt;
            first += cnt;
            first <<= 1;
            code <<= 1;
            if (br.overrun) return -1;
        }
        return -1;
    }
};

const uint16_t kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
const uint8_t kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
const uint16_t kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                 8193, 12289, 16385, 24577 };
const uint8_t kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

bool InflateBlockData(BitReader& br, const Huffman& lit, const Huffman& dist,
                      std::vector<uint8_t>& out, size_t outCap)
{
    for (;;)
    {
        const int sym = lit.Decode(br);
        if (sym < 0) return false;
        if (sym < 256)
        {
            if (out.size() >= outCap) return false;
            out.push_back(uint8_t(sym));
        }
        else if (sym == 256)
            return true;
        else
        {
            const int li = sym - 257;
            if (li >= 29) return false;
            const size_t len = kLenBase[li] + br.Bits(kLenExtra[li]);
            const int di = dist.Decode(br);
            if (di < 0 || di >= 30) return false;
            const size_t d = kDistBase[di] + br.Bits(kDistExtra[di]);
            if (br.overrun || d > out.size() || out.size() + len > outCap) return false;
            const size_t from = out.size() - d;
            for (size_t i = 0; i < len; ++i) out.push_back(out[from + i]); // may overlap
        }
    }
}

bool Inflate(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t outCap)
{
    BitReader br{ in, n };
    out.reserve(outCap);
    for (;;)
    {
        const uint32_t final = br.Bits(1);
        const uint32_t type = br.Bits(2);
        if (br.overrun) return false;
        if (type == 0)
        {
            br.AlignToByte();
            if (br.pos + 4 > n) return false;
            const uint16_t len = uint16_t(in[br.pos] | (in[br.pos + 1] << 8));
            const uint16_t nlen = uint16_t(in[br.pos + 2] | (in[br.pos + 3] << 8));
            br.pos += 4;
            if (uint16_t(~len) != nlen || br.pos + len > n || out.size() + len > outCap) return false;
            out.insert(out.end(), in + br.pos, in + br.pos + len);
            br.pos += len;
        }
        else if (type == 1)
        {
            static Huffman fixedLit, fixedDist;
            static bool built = [] {
                uint8_t l[288];
                int s = 0;
                for (; s < 144; ++s) l[s] = 8;
                for (; s < 256; ++s) l[s] = 9;
                for (; s < 280; ++s) l[s] = 7;
                for (; s < 288; ++s) l[s] = 8;
                fixedLit.Build(l, 288);
                uint8_t d[30];
                for (int i = 0; i < 30; ++i) d[i] = 5;
                fixedDist.Build(d, 30);
                return true;
            }();
            (void)built;
            if (!InflateBlockData(br, fixedLit, fixedDist, out, outCap)) return false;
        }
        else if (type == 2)
        {
            const int nlen = int(br.Bits(5)) + 257;
            const int ndist = int(br.Bits(5)) + 1;
            const int ncode = int(br.Bits(4)) + 4;
            if (nlen > 286 || ndist > 30) return false;
            static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                               11, 4, 12, 3, 13, 2, 14, 1, 15 };
            uint8_t lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = uint8_t(br.Bits(3));
            Huffman lencode;
            if (!lencode.Build(lengths, 19)) return false;
            int idx = 0;
            while (idx < nlen + ndist)
            {
                int sym = lencode.Decode(br);
                if (sym < 0) return false;
                if (sym < 16)
                    lengths[idx++] = uint8_t(sym);
                else
                {
                    uint8_t len = 0;
                    int rep;
                    if (sym == 16)
                    {
                        if (idx == 0) return false;
                        len = lengths[idx - 1];
                        rep = 3 + int(br.Bits(2));
                    }
                    else if (sym == 17)
                        rep = 3 + int(br.Bits(3));
                    else
                        rep = 11 + int(br.Bits(7));
                    if (idx + rep > nlen + ndist) return false;
                    while (rep--) lengths[idx++] = len;
                }
            }
            if (lengths[256] == 0) return false; // no end-of-block code
            Huffman lit, dist;
            if (!lit.Build(lengths, nlen)) return false;
            // A single-distance-code block is legal (all zero lengths but one); Build's
            // "no codes" refusal is only wrong when the block also uses no distances,
            // which a valid encoder never emits for a length/distance pair.
            if (!dist.Build(lengths + nlen, ndist) && ndist != 1) return false;
            if (!InflateBlockData(br, lit, dist, out, outCap)) return false;
        }
        else
            return false;
        if (final) return true;
    }
}

// --------------------------------------------------------------------------------- PNG
uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

uint8_t Paeth(int a, int b, int c)
{
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return uint8_t(a);
    return pb <= pc ? uint8_t(b) : uint8_t(c);
}

bool Refuse(const std::filesystem::path& f, const char* why)
{
    fprintf(stderr, "[icon] %s: %s — keeping the default window icon\n", f.string().c_str(), why);
    return false;
}
} // namespace

namespace PngIcon
{
bool Load(const std::filesystem::path& file, Image& out)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) return false; // absent is not an error worth a line: the game may not be unpacked yet
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    if (d.size() < 8 + 25 || memcmp(d.data(), sig, 8) != 0) return Refuse(file, "not a PNG");

    uint32_t w = 0, h = 0;
    int depth = 0, ctype = 0, interlace = 0;
    std::vector<uint8_t> idat, palette;
    bool haveHdr = false;
    for (size_t p = 8; p + 12 <= d.size();)
    {
        const uint32_t len = BE32(&d[p]);
        const uint8_t* type = &d[p + 4];
        if (p + 12 + size_t(len) > d.size()) return Refuse(file, "truncated chunk");
        const uint8_t* body = &d[p + 8];
        if (memcmp(type, "IHDR", 4) == 0 && len == 13)
        {
            w = BE32(body); h = BE32(body + 4);
            depth = body[8]; ctype = body[9]; interlace = body[12];
            haveHdr = true;
        }
        else if (memcmp(type, "PLTE", 4) == 0)
            palette.assign(body, body + len);
        else if (memcmp(type, "IDAT", 4) == 0)
            idat.insert(idat.end(), body, body + len);
        else if (memcmp(type, "IEND", 4) == 0)
            break;
        p += 12 + len; // length + type + data + crc (the crc is not checked: a wrong pixel is not a hazard)
    }
    if (!haveHdr) return Refuse(file, "no IHDR");
    if (depth != 8) return Refuse(file, "only 8-bit channels are handled");
    if (interlace) return Refuse(file, "interlaced PNGs are not handled");
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return Refuse(file, "implausible dimensions");
    int channels;
    switch (ctype)
    {
        case 2: channels = 3; break;               // RGB
        case 3: channels = 1; break;               // palette
        case 6: channels = 4; break;               // RGBA
        default: return Refuse(file, "colour type is not RGB, RGBA or palette");
    }
    if (ctype == 3 && palette.size() < 3) return Refuse(file, "palette image without a PLTE");

    // zlib wrapper: 2-byte header (CM=8), the deflate stream, a 4-byte adler32 we skip.
    if (idat.size() < 6 || (idat[0] & 0x0F) != 8 || (idat[1] & 0x20)) return Refuse(file, "IDAT is not a zlib stream");
    const size_t stride = size_t(w) * channels;
    const size_t rawSize = (stride + 1) * h;
    std::vector<uint8_t> raw;
    if (!Inflate(idat.data() + 2, idat.size() - 6, raw, rawSize) || raw.size() != rawSize)
        return Refuse(file, "the compressed image data did not inflate to the expected size");

    // Unfilter in place, one scanline at a time: each row starts with its filter byte.
    std::vector<uint8_t> pix(stride * h);
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint8_t filter = raw[y * (stride + 1)];
        const uint8_t* src = &raw[y * (stride + 1) + 1];
        uint8_t* dst = &pix[y * stride];
        const uint8_t* up = y ? &pix[(y - 1) * stride] : nullptr;
        for (size_t x = 0; x < stride; ++x)
        {
            const int a = x >= size_t(channels) ? dst[x - channels] : 0;
            const int b = up ? up[x] : 0;
            const int c = (up && x >= size_t(channels)) ? up[x - channels] : 0;
            int v = src[x];
            switch (filter)
            {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += Paeth(a, b, c); break;
                default: return Refuse(file, "unknown scanline filter");
            }
            dst[x] = uint8_t(v);
        }
    }

    out.width = w;
    out.height = h;
    out.rgba.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i)
    {
        uint8_t* o = &out.rgba[i * 4];
        if (ctype == 6)
            memcpy(o, &pix[i * 4], 4);
        else if (ctype == 2)
        {
            memcpy(o, &pix[i * 3], 3);
            o[3] = 255;
        }
        else
        {
            const size_t pi = size_t(pix[i]) * 3;
            if (pi + 2 >= palette.size()) return Refuse(file, "palette index out of range");
            memcpy(o, &palette[pi], 3);
            o[3] = 255;
        }
    }
    return true;
}

const Image& GameTile()
{
    // Only a SUCCESS is cached. The progress window is created before the first-run
    // extract and the game window after it, so a miss remembered from the first call
    // would deny the icon to every window of the very session that produced the file.
    static Image tile;
    static bool loaded = false;
    if (!loaded)
    {
        Image img;
        const std::filesystem::path f = HostPaths::Game() / "X_IMAGEID_GAME.PNG";
        if (Load(f, img))
        {
            fprintf(stderr, "[icon] window icon: %s (%ux%u)\n", f.string().c_str(), img.width, img.height);
            tile = std::move(img);
            loaded = true;
        }
    }
    return tile;
}
} // namespace PngIcon
