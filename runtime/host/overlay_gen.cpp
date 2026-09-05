// See overlay_gen.h for why this exists. This file is a LINE-FOR-LINE port of the
// two Python generators — tools/gen_pc_options.py and tools/gen_kbm_icons.py — and
// its contract is BYTE IDENTITY with them: same inputs, same output files, same
// bytes. Where a choice looks arbitrary (an iteration order, a tie-break in the
// Huffman package-merge, a %.5f) it is the Python's choice, kept so the identity
// gate stays exact. Read the Python first for the WHY of every transform: each
// carries the part-60/part-92 ladder that established it (the guest decoder's
// crash on degenerate LZX streams, the Visible="false" idiom, the layout.bin size
// pin, the used-extent rule). This file only re-states the load-bearing ones.
//
// The one thing the Python does that this file does not: DRAW. The 25 key-cap
// chips are PIL-rendered in the dev tree and ship as finished DXT5 texel blobs
// (tools/release/kbm_chips/*.dxt — our art, no Capcom byte); this composes them
// into the player's own banks.
//
// Error discipline: the Python is assert-dense on purpose (every container model
// mismatch is a hard stop, never a skip). The port keeps that with an internal
// exception caught at the two public entry points — a gate failure names itself in
// `err` and nothing half-written is left behind (outputs are written only after
// every gate of their layer passed).
#include "overlay_gen.h"

#include "host_paths.h"

#include <xex_patcher.h> // XenonUtils: the SAME lzxDecompress that unpacks the XEX

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

struct GenError
{
    std::string msg;
};

[[noreturn]] void Refuse(const std::string& msg)
{
    throw GenError{msg};
}

// ---------------------------------------------------------------------------
// Bytes and files
// ---------------------------------------------------------------------------

Bytes ReadFileBytes(const fs::path& p)
{
    FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f)
        Refuse("cannot read " + p.string());
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    Bytes out(size_t(sz < 0 ? 0 : sz));
    if (!out.empty() && std::fread(out.data(), 1, out.size(), f) != out.size())
    {
        std::fclose(f);
        Refuse("short read on " + p.string());
    }
    std::fclose(f);
    return out;
}

void WriteFileBytes(const fs::path& p, const Bytes& b)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    FILE* f = std::fopen(p.string().c_str(), "wb");
    if (!f)
        Refuse("cannot write " + p.string());
    if (!b.empty() && std::fwrite(b.data(), 1, b.size(), f) != b.size())
    {
        std::fclose(f);
        Refuse("short write on " + p.string());
    }
    std::fclose(f);
}

uint32_t LE32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}
void PutLE32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
uint32_t BE32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t BE16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
void AppendLE32(Bytes& b, uint32_t v)
{
    b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24));
}
void AppendBE32(Bytes& b, uint32_t v)
{
    b.push_back(uint8_t(v >> 24)); b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v));
}
void AppendBE16(Bytes& b, uint16_t v)
{
    b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v));
}

// ---------------------------------------------------------------------------
// LZX: decode (XenonUtils' decoder, per-chunk XMemCompress framing)
// ---------------------------------------------------------------------------

// A compressed `.big` entry: [BE u32 uncompressed][BE u32 window] then chunks of
// {BE u32 frameLen; 0xFF; BE u16 rawLen; BE u16 cmpLen; LZX bits (+ slack)}. Each
// chunk is an independent stream — the framing tools/big_decompress calls
// "interpretation A", proven on 71,580/71,580 bytes of cc_03.bct.
Bytes LzxDecodeEntry(const Bytes& stored, const char* what)
{
    if (stored.size() < 16)
        Refuse(std::string(what) + ": stored entry too small to be compressed");
    const uint32_t uncompressed = BE32(stored.data());
    const uint32_t window = BE32(stored.data() + 4);
    Bytes out(uncompressed, 0);
    size_t produced = 0;
    for (size_t o = 8; o + 4 <= stored.size();)
    {
        const uint32_t len = BE32(stored.data() + o);
        if (!len || len > stored.size() - o - 4)
            break;
        const uint8_t* frame = stored.data() + o + 4;
        if (len < 5 || frame[0] != 0xFF)
            Refuse(std::string(what) + ": chunk without the 0xFF XMemCompress header");
        const uint32_t rawLen = BE16(frame + 1);
        const uint32_t cmpLen = BE16(frame + 3);
        if (cmpLen > len - 5 || produced + rawLen > out.size())
            Refuse(std::string(what) + ": chunk lengths inconsistent with the stream header");
        if (lzxDecompress(frame + 5, cmpLen, out.data() + produced, rawLen,
                          window, nullptr, 0) != 0)
            Refuse(std::string(what) + ": lzxDecompress refused a chunk");
        produced += rawLen;
        o += 4 + len;
    }
    if (produced != uncompressed)
        Refuse(std::string(what) + ": decoded " + std::to_string(produced) + " of " +
               std::to_string(uncompressed) + " bytes");
    return out;
}

// ---------------------------------------------------------------------------
// LZX: encode. gen_pc_options.py lzx_encode_stream, ported symbol for symbol.
// ---------------------------------------------------------------------------

// Position-code tables for the 32 KB window, from the tables in the IMAGE ITSELF
// (extra_bits at 0x820BC6F4, position_base at 0x820C8E38).
constexpr int kLzxExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7,
                              8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

const std::vector<int>& LzxBase()
{
    static const std::vector<int> base = [] {
        std::vector<int> b;
        int v = 0;
        for (int e : kLzxExtra)
        {
            b.push_back(v);
            v += 1 << e;
        }
        return b;
    }();
    return base;
}

// Length-limited canonical Huffman lengths by package-merge. Tie-breaks follow the
// Python exactly: candidates ordered by (frequency, symbol tuple) — two candidates
// can never compare equal because the symbol tuples are all distinct — and the
// merged pairs concatenate their tuples in sorted order.
struct HuffPkg
{
    uint64_t freq;
    std::vector<int> syms;
    bool operator<(const HuffPkg& o) const
    {
        if (freq != o.freq)
            return freq < o.freq;
        return syms < o.syms;
    }
};

std::vector<int> HuffmanLengths(const std::vector<uint64_t>& freqs, int maxlen)
{
    std::vector<int> used;
    for (size_t s = 0; s < freqs.size(); ++s)
        if (freqs[s])
            used.push_back(int(s));
    std::vector<int> out(freqs.size(), 0);
    if (used.empty())
        return out;
    if (used.size() == 1)
    {
        // A legal, decoder-accepted degenerate tree, only reached by the LENGTH
        // tree and only when a block has exactly one distinct match length.
        out[used[0]] = 1;
        return out;
    }
    std::vector<HuffPkg> level;
    for (int s : used)
        level.push_back({freqs[s], {s}});
    for (int pass = 0; pass < maxlen - 1; ++pass)
    {
        std::vector<HuffPkg> prev = level;
        std::sort(prev.begin(), prev.end());
        std::vector<HuffPkg> next;
        for (size_t i = 0; i + 1 < prev.size(); i += 2)
        {
            HuffPkg m{prev[i].freq + prev[i + 1].freq, prev[i].syms};
            m.syms.insert(m.syms.end(), prev[i + 1].syms.begin(), prev[i + 1].syms.end());
            next.push_back(std::move(m));
        }
        for (int s : used)
            next.push_back({freqs[s], {s}});
        std::sort(next.begin(), next.end());
        level = std::move(next);
    }
    const size_t take = 2 * used.size() - 2;
    // level is already sorted (the loop appended it sorted).
    for (size_t i = 0; i < take && i < level.size(); ++i)
        for (int s : level[i].syms)
            out[s] += 1;
    // Kraft equality, checked in integers: sum of 2^(maxlen - l) must be 2^maxlen.
    uint64_t kraft = 0;
    for (int l : out)
        if (l)
            kraft += 1ull << (maxlen - l);
    if (kraft != (1ull << maxlen))
        Refuse("package-merge broke Kraft");
    return out;
}

// Canonical code assignment, identical to libmspack's table builder: sort by
// (length, symbol), codes count upward with left-justification per length.
struct HuffCode
{
    uint32_t code;
    int len;
};

std::map<int, HuffCode> CanonicalCodes(const std::vector<int>& lengths)
{
    std::vector<std::pair<int, int>> syms; // (length, symbol)
    for (size_t s = 0; s < lengths.size(); ++s)
        if (lengths[s])
            syms.push_back({lengths[s], int(s)});
    std::sort(syms.begin(), syms.end());
    std::map<int, HuffCode> codes;
    uint32_t code = 0;
    int prevLen = 0;
    for (auto [l, s] : syms)
    {
        code <<= (l - prevLen);
        codes[s] = {code, l};
        code += 1;
        prevLen = l;
    }
    return codes;
}

// The bit accumulator: MSB-first bits, packed at the end into 16-bit LITTLE-endian
// words — bit-exact against libmspack's reader.
struct BitSink
{
    std::vector<uint8_t> bits;
    void Put(uint32_t val, int n)
    {
        for (int b = n - 1; b >= 0; --b)
            bits.push_back(uint8_t((val >> b) & 1));
    }
    Bytes Pack() const
    {
        std::vector<uint8_t> padded = bits;
        while (padded.size() % 16)
            padded.push_back(0);
        Bytes payload;
        for (size_t k = 0; k < padded.size(); k += 16)
        {
            uint32_t v = 0;
            for (size_t b = k; b < k + 16; ++b)
                v = (v << 1) | padded[b];
            payload.push_back(uint8_t(v & 0xFF));
            payload.push_back(uint8_t(v >> 8));
        }
        return payload;
    }
};

// A real, small LZX compressor: greedy LZ77 over the full 32 KB window with
// per-chunk canonical Huffman trees, emitting VERBATIM blocks — the same shape the
// shipped encoder uses for the frontend text entries. WHY IT HAS TO BE REAL is the
// part-60 ladder in the Python's docstring: the guest decoder crashes with heap
// corruption on every degenerate stream (stored entries, literal-only fixed
// trees), and the cure is streams statistically like the shipped ones.
Bytes LzxEncodeStream(const Bytes& data)
{
    if (data.size() > 0x8000)
        Refuse("lzx_encode_stream: " + std::to_string(data.size()) +
               " bytes needs multiple chunks, and multi-chunk streams from this "
               "encoder are REFUSED by the guest decoder");
    Bytes out;
    AppendBE32(out, uint32_t(data.size()));
    AppendBE32(out, 0x8000);

    const Bytes& chunk = data; // single chunk by the assert above
    const int n = int(chunk.size());

    // ---- pass 1: greedy LZ77 with an R0/R1/R2-aware cost preference
    struct Op
    {
        bool lit;
        int a; // literal byte, or offset
        int b; // match length
    };
    std::vector<Op> ops;
    std::unordered_map<uint32_t, std::vector<int>> last; // 3-gram -> positions
    auto key3 = [&](int p) {
        return (uint32_t(chunk[p]) << 16) | (uint32_t(chunk[p + 1]) << 8) | chunk[p + 2];
    };
    int i = 0;
    while (i < n)
    {
        int bestLen = 0, bestOff = 0;
        if (i + 3 <= n)
        {
            auto it = last.find(key3(i));
            if (it != last.end())
            {
                const std::vector<int>& cand = it->second;
                const size_t lo = cand.size() > 64 ? cand.size() - 64 : 0;
                for (size_t c = cand.size(); c-- > lo;) // most recent first
                {
                    const int j = cand[c];
                    const int off = i - j;
                    if (off > 0x8000 - 2)
                        break;
                    int length = 3;
                    const int limit = std::min(257, n - i);
                    while (length < limit && chunk[j + length] == chunk[i + length])
                        ++length;
                    if (!bestLen || length > bestLen)
                    {
                        bestLen = length;
                        bestOff = off;
                        if (length >= 128)
                            break;
                    }
                }
            }
        }
        if (bestLen >= 3)
        {
            ops.push_back({false, bestOff, bestLen});
            for (int p = i; p < std::min(i + bestLen, n - 2); ++p)
                last[key3(p)].push_back(p);
            i += bestLen;
        }
        else
        {
            ops.push_back({true, chunk[i], 0});
            if (i + 3 <= n)
                last[key3(i)].push_back(i);
            i += 1;
        }
    }

    // ---- pass 2: resolve the R0-R2 offset history now, so the emitted symbols
    // are exactly the counted ones
    struct ROp
    {
        bool lit;
        int a; // literal byte, or formatted offset
        int b; // match length
    };
    std::vector<ROp> rops;
    {
        int R[3] = {1, 1, 1};
        for (const Op& op : ops)
        {
            if (op.lit)
            {
                rops.push_back({true, op.a, 0});
                continue;
            }
            const int off = op.a;
            int fmt;
            if (off == R[0])
                fmt = 0;
            else if (off == R[1])
            {
                fmt = 1;
                R[1] = R[0];
                R[0] = off;
            }
            else if (off == R[2])
            {
                fmt = 2;
                R[2] = R[0];
                R[0] = off;
            }
            else
            {
                fmt = off + 2;
                R[2] = R[1];
                R[1] = R[0];
                R[0] = off;
            }
            rops.push_back({false, fmt, op.b});
        }
    }

    const std::vector<int>& base = LzxBase();
    auto slotOf = [&](int fmt) {
        int slot = 0;
        for (int s = 0; s < 30; ++s)
            if (base[s] <= fmt)
                slot = s;
        return slot;
    };

    std::vector<uint64_t> mainFreq(496, 0); // 256 literals + 30 slots * 8
    std::vector<uint64_t> lenFreq(249, 0);
    for (const ROp& op : rops)
    {
        if (op.lit)
        {
            mainFreq[op.a] += 1;
            continue;
        }
        const int slot = slotOf(op.a);
        const int header = std::min(op.b - 2, 7);
        mainFreq[256 + slot * 8 + header] += 1;
        if (header == 7)
            lenFreq[op.b - 2 - 7] += 1;
    }

    std::vector<int> mainLen = HuffmanLengths(mainFreq, 16);
    std::vector<int> lengthLen = HuffmanLengths(lenFreq, 16);
    if (std::none_of(lengthLen.begin(), lengthLen.end(), [](int l) { return l != 0; }))
    {
        // No long matches this chunk: the tree would be EMPTY, and no shipped
        // stream has an empty LENGTH tree, so write a harmless two-code one.
        lengthLen[0] = lengthLen[1] = 1;
    }
    std::map<int, HuffCode> mainCodes = CanonicalCodes(mainLen);
    std::map<int, HuffCode> lengthCodes = CanonicalCodes(lengthLen);

    BitSink sink;

    // ---- tree-length writer: the delta/17/18 run format, with a real pretree per
    // READ_LENGTHS call, exactly as the decoder consumes it. prev is all-zero for
    // every call this encoder makes (each chunk is an independent stream).
    auto writeLengths = [&](const std::vector<int>& lens) {
        struct P
        {
            int sym;
            bool hasExtra;
            int extraVal, extraBits;
        };
        std::vector<P> ops2;
        size_t x = 0;
        while (x < lens.size())
        {
            if (lens[x] == 0)
            {
                size_t run = 0;
                while (x + run < lens.size() && lens[x + run] == 0)
                    ++run;
                while (run >= 20)
                {
                    const size_t take = std::min<size_t>(run, 51);
                    ops2.push_back({18, true, int(take - 20), 5});
                    run -= take;
                    x += take;
                }
                while (run >= 4)
                {
                    const size_t take = std::min<size_t>(run, 19);
                    ops2.push_back({17, true, int(take - 4), 4});
                    run -= take;
                    x += take;
                }
                for (size_t r = 0; r < run; ++r)
                {
                    ops2.push_back({0, false, 0, 0}); // (prev 0 - new 0) mod 17
                    ++x;
                }
            }
            else
            {
                ops2.push_back({((0 - lens[x]) % 17 + 17) % 17, false, 0, 0});
                ++x;
            }
        }
        std::vector<uint64_t> pf(20, 0);
        for (const P& p : ops2)
            pf[p.sym] += 1;
        std::vector<int> plens = HuffmanLengths(pf, 15);
        int usedCount = 0;
        for (int l : plens)
            if (l)
                ++usedCount;
        if (usedCount == 1)
        {
            // A one-code pretree: give it a legal complete partner.
            int lone = 0;
            for (int s = 0; s < 20; ++s)
                if (plens[s] == 1)
                {
                    lone = s;
                    break;
                }
            plens[lone] = 1;
            plens[(lone + 1) % 20] = 1;
        }
        std::map<int, HuffCode> pcodes = CanonicalCodes(plens);
        for (int s = 0; s < 20; ++s)
            sink.Put(uint32_t(plens[s]), 4);
        for (const P& p : ops2)
        {
            const HuffCode& c = pcodes.at(p.sym);
            sink.Put(c.code, c.len);
            if (p.hasExtra)
                sink.Put(uint32_t(p.extraVal), p.extraBits);
        }
    };

    sink.Put(0, 1); // no intel E8 translation
    sink.Put(1, 3); // LZX_BLOCKTYPE_VERBATIM
    sink.Put(uint32_t(n), 24);
    writeLengths(std::vector<int>(mainLen.begin(), mainLen.begin() + 256));
    writeLengths(std::vector<int>(mainLen.begin() + 256, mainLen.end()));
    writeLengths(lengthLen);

    // ---- content
    for (const ROp& op : rops)
    {
        if (op.lit)
        {
            const HuffCode& c = mainCodes.at(op.a);
            sink.Put(c.code, c.len);
            continue;
        }
        const int fmt = op.a;
        const int slot = slotOf(fmt);
        const int header = std::min(op.b - 2, 7);
        const HuffCode& c = mainCodes.at(256 + slot * 8 + header);
        sink.Put(c.code, c.len);
        if (header == 7)
        {
            const HuffCode& lc = lengthCodes.at(op.b - 2 - 7);
            sink.Put(lc.code, lc.len);
        }
        const int eb = kLzxExtra[slot];
        if (eb)
            sink.Put(uint32_t(fmt - base[slot]), eb);
    }

    const Bytes payload = sink.Pack();
    if (payload.size() >= size_t(n))
        Refuse("lzx_encode_stream did not compress (" + std::to_string(payload.size()) +
               " >= " + std::to_string(n) + ") — this encoder is for layout TEXT; "
               "do not point it at compressed data");
    // FIVE ZERO TRAILER BYTES inside the chunk length, after the payload — every
    // shipped chunk has exactly this, and it is decoder READAHEAD SLACK: the
    // guest's bit reader pulls 16-bit words past cmpLen at stream end.
    Bytes frame;
    frame.push_back(0xFF);
    AppendBE16(frame, uint16_t(n));
    AppendBE16(frame, uint16_t(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.insert(frame.end(), 5, 0);
    AppendBE32(out, uint32_t(frame.size()));
    out.insert(out.end(), frame.begin(), frame.end());
    return out;
}

// The round-trip gate the Python runs through tools/big_decompress: every emitted
// stream must decode, through the decoder we did NOT write, to the exact plaintext.
void VerifyEncodedStream(const Bytes& stored, const Bytes& want, const char* what)
{
    const Bytes got = LzxDecodeEntry(stored, what);
    if (got != want)
        Refuse(std::string(what) + ": encoded stream decoded to DIFFERENT bytes");
}

// ---------------------------------------------------------------------------
// .big archives (gen_pc_options.py read_big / write_big)
// ---------------------------------------------------------------------------

struct BigEntry
{
    std::string name;
    uint32_t nameOff, hash, size, size2, dataOff, flags, resv;
    Bytes stored;
};

struct BigArchive
{
    Bytes raw;
    uint32_t dataStart, namesOff;
    std::vector<BigEntry> entries;
};

BigArchive ReadBig(const fs::path& path)
{
    BigArchive a;
    a.raw = ReadFileBytes(path);
    if (a.raw.size() < 24 || LE32(a.raw.data()) != 0x03040506)
        Refuse(path.string() + ": bad .big magic");
    a.dataStart = LE32(a.raw.data() + 4);
    const uint32_t count = LE32(a.raw.data() + 12);
    const uint32_t hdr = LE32(a.raw.data() + 16);
    a.namesOff = LE32(a.raw.data() + 20);
    if (hdr != 0x18 || a.namesOff != 0x18 + count * 28)
        Refuse(path.string() + ": unexpected .big header layout");
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint8_t* e = a.raw.data() + 0x18 + i * 28;
        BigEntry be;
        be.nameOff = LE32(e);
        be.hash = LE32(e + 4);
        be.size = LE32(e + 8);
        be.size2 = LE32(e + 12);
        be.dataOff = LE32(e + 16);
        be.flags = LE32(e + 20);
        be.resv = LE32(e + 24);
        size_t end = be.nameOff;
        while (end < a.raw.size() && a.raw[end])
            ++end;
        be.name.assign(reinterpret_cast<const char*>(a.raw.data() + be.nameOff),
                       end - be.nameOff);
        be.stored.assign(a.raw.begin() + be.dataOff, a.raw.begin() + be.dataOff + be.size);
        a.entries.push_back(std::move(be));
    }
    return a;
}

// Rebuild: header + index + the ORIGINAL name table bytes + payloads packed in
// original file order. `align` preserves the source archive's own placement
// granularity (fecmn.big packs at 4 bytes; preload4.big at 0x800).
void WriteBig(const fs::path& path, BigArchive& a, uint32_t align)
{
    const Bytes nameTable(a.raw.begin() + a.namesOff, a.raw.begin() + a.dataStart);
    std::vector<size_t> order(a.entries.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        return a.entries[x].dataOff < a.entries[y].dataOff;
    });
    uint32_t pos = a.dataStart;
    Bytes payload;
    for (size_t i : order)
    {
        BigEntry& e = a.entries[i];
        const uint32_t pad = (align - (pos % align)) % align;
        payload.insert(payload.end(), pad, 0);
        pos += pad;
        e.dataOff = pos;
        payload.insert(payload.end(), e.stored.begin(), e.stored.end());
        pos += uint32_t(e.stored.size());
        e.size = uint32_t(e.stored.size());
    }
    const uint32_t total = pos;
    Bytes out;
    AppendLE32(out, 0x03040506);
    AppendLE32(out, a.dataStart);
    AppendLE32(out, total);
    AppendLE32(out, uint32_t(a.entries.size()));
    AppendLE32(out, 0x18);
    AppendLE32(out, a.namesOff);
    for (const BigEntry& e : a.entries)
    {
        AppendLE32(out, e.nameOff);
        AppendLE32(out, e.hash);
        AppendLE32(out, e.size);
        AppendLE32(out, e.size2);
        AppendLE32(out, e.dataOff);
        AppendLE32(out, e.flags);
        AppendLE32(out, e.resv);
    }
    out.insert(out.end(), nameTable.begin(), nameTable.end());
    out.insert(out.end(), payload.begin(), payload.end());
    if (out.size() != total)
        Refuse(path.string() + ": rebuilt archive size mismatch");
    WriteFileBytes(path, out);
}

// ---------------------------------------------------------------------------
// The options_pc.txt layout rewrite (gen_pc_options.py rewrite_options_pc)
// ---------------------------------------------------------------------------

struct RowSpec
{
    const char* name;
    std::vector<std::pair<const char*, const char*>> values; // (string id, comment)
};

// Row order, and the baked value lists. Verbs use the 360 token order
// (ACT:<direction>:<name>) that every working screen on this build uses.
const std::vector<RowSpec>& Rows()
{
    static const std::vector<RowSpec> rows = {
        {"Resolution",
         {{"100000", "CZ 1280x720"}, {"100001", "CZ 2560x1440"},
          {"100002", "CZ 3840x2160"}, {"100003", "CZ 5120x2880"}}},
        {"DisplayMode",
         {{"10724", "IDS_OPTIONS_PC_WINDOWED"}, {"100004", "CZ Borderless"},
          {"10723", "IDS_OPTIONS_PC_FULLSCREEN"}}},
        {"VSync", {{"10701", "IDS_OPTIONS_OFF"}, {"10700", "IDS_OPTIONS_ON"}}},
        {"Shadow",
         {{"10754", "IDS_OPTIONS_PC_LOW"}, {"10755", "IDS_OPTIONS_PC_MEDIUM"},
          {"10756", "IDS_OPTIONS_PC_HIGH"}}},
    };
    return rows;
}

constexpr double kYFirst = 0.28473, kYStep = 0.04861; // the shipped layout's row rhythm

bool StartsWith(const std::string& s, const char* prefix)
{
    return s.rfind(prefix, 0) == 0;
}

std::string Strip(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i)
        if (i == text.size() || text[i] == '\n')
        {
            lines.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    return lines;
}

std::string JoinLines(const std::vector<std::string>& lines)
{
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (i)
            out += '\n';
        out += lines[i];
    }
    return out;
}

struct BlockSpan
{
    size_t header, open, close;
};

// Find the first block whose header matches at or after fromLine. Brace-counting,
// exact — the Python's find_block.
template <typename Pred>
bool FindBlock(const std::vector<std::string>& lines, Pred pred, size_t fromLine,
               BlockSpan* out)
{
    for (size_t i = fromLine; i < lines.size(); ++i)
    {
        if (!pred(lines[i]))
            continue;
        if (i + 1 >= lines.size() || Strip(lines[i + 1]) != "{")
            Refuse("options_pc.txt: no brace after line " + std::to_string(i) + ": " + lines[i]);
        int depth = 0;
        for (size_t j = i + 1; j < lines.size(); ++j)
        {
            const std::string s = Strip(lines[j]);
            if (s == "{")
                ++depth;
            else if (s == "}")
            {
                --depth;
                if (depth == 0)
                {
                    *out = {i, i + 1, j};
                    return true;
                }
            }
        }
        Refuse("options_pc.txt: unclosed block at line " + std::to_string(i));
    }
    return false;
}

// The Python's re.sub(r'onSelect="ACT:[^"]*<Dir>[^"]*"', replacement, line): the
// match runs from onSelect="ACT: to the next quote (the class can't cross one) and
// fires only if <Dir> appears inside it. Replaces every occurrence in the line.
std::string RewriteActVerb(const std::string& line, const char* dir, const std::string& name)
{
    const std::string needle = "onSelect=\"ACT:";
    std::string out;
    size_t pos = 0;
    while (true)
    {
        const size_t hit = line.find(needle, pos);
        if (hit == std::string::npos)
        {
            out += line.substr(pos);
            return out;
        }
        const size_t contentStart = hit + needle.size();
        const size_t quote = line.find('"', contentStart);
        if (quote == std::string::npos)
        {
            out += line.substr(pos);
            return out;
        }
        const std::string content = line.substr(contentStart, quote - contentStart);
        if (content.find(dir) != std::string::npos)
        {
            out += line.substr(pos, hit - pos);
            out += "onSelect=\"ACT:" + std::string(dir) + ":" + name + "\"";
        }
        else
        {
            out += line.substr(pos, quote + 1 - pos);
        }
        pos = quote + 1;
    }
}

// Rewrite one kept spin group: focus chain, verbs, Y position, and the CTSelect
// cFEText -> cFETextList with our baked values.
std::vector<std::string> TransformGroup(const std::vector<std::string>& glines,
                                        const std::string& name, size_t rowIndex)
{
    const std::vector<RowSpec>& rows = Rows();
    const std::string up = rows[(rowIndex + rows.size() - 1) % rows.size()].name;
    const std::string down = rows[(rowIndex + 1) % rows.size()].name;
    const double y = kYFirst + kYStep * double(rowIndex);
    const auto& values = rows[rowIndex].values;

    // Header properties sit before the first nested cFE block.
    size_t firstChild = size_t(-1);
    for (size_t k = 2; k < glines.size(); ++k)
        if (StartsWith(glines[k], "cFE"))
        {
            firstChild = k;
            break;
        }
    if (firstChild == size_t(-1))
        Refuse("options_pc.txt: spin group " + name + " has no child block");

    std::vector<std::string> outl;
    for (size_t k = 0; k < firstChild; ++k)
    {
        std::string l = glines[k];
        if (StartsWith(l, "onUp="))
        {
            l = "onUp=\"FOC:" + up + "\"";
            if (rowIndex == 0)
            {
                // Initial focus: OptionsPC's class enter is compiled out, so the
                // layout declares it ('Focus' is in the parser's own attribute
                // table at 0x820B9398).
                outl.push_back("Focus=\"true\"");
            }
        }
        else if (StartsWith(l, "onDown="))
            l = "onDown=\"FOC:" + down + "\"";
        else if (StartsWith(l, "onLeft="))
            l = "onLeft=\"ACT:Prev:" + name + "\"";
        else if (StartsWith(l, "onRight="))
            l = "onRight=\"ACT:Next:" + name + "\"";
        else if (StartsWith(l, "Y="))
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "Y=%.5f", y);
            l = buf;
        }
        outl.push_back(l);
    }
    std::vector<std::string> body(glines.begin() + firstChild, glines.end());

    // The two arrow buttons carry the same verbs on their onSelect.
    for (std::string& l : body)
        l = RewriteActVerb(l, "Prev", name);
    for (std::string& l : body)
        l = RewriteActVerb(l, "Next", name);

    // Replace the CTSelect block (cFEText or cFETextList — the shipped PC layout
    // uses a bare cFEText that a PC-only runtime populated) with the WORKING 360
    // idiom: a cFETextList with the values baked in.
    BlockSpan sel;
    if (!FindBlock(body,
                   [](const std::string& l) {
                       const std::string s = Strip(l);
                       return s == "cFEText CTSelect" || s == "cFETextList CTSelect";
                   },
                   0, &sel))
        Refuse("options_pc.txt: " + name + ": no CTSelect block");
    std::vector<std::string> newBlock = {"cFETextList CTSelect", "{", "Font=\"arialblk46\"",
                                         "Size=" + std::to_string(values.size())};
    for (const auto& [sid, comment] : values)
        newBlock.push_back("Text=\"" + std::string(sid) + " " + comment + "\"");
    for (const char* l : {"Justify=\"center\"", "DropShadowX=0.00100", "DropShadowY=0.00100",
                          "Loop=\"true\"", "Init=0", "Y=0.04215", "W=1.00000", "H=1.00000",
                          "R=0.11765", "G=0.39216", "B=0.39216", "ScaleX=0.55000",
                          "ScaleY=0.65000", "}"})
        newBlock.push_back(l);
    std::vector<std::string> merged(body.begin(), body.begin() + sel.header);
    merged.insert(merged.end(), newBlock.begin(), newBlock.end());
    merged.insert(merged.end(), body.begin() + sel.close + 1, body.end());

    outl.insert(outl.end(), merged.begin(), merged.end());
    return outl;
}

std::string RewriteOptionsPc(const std::string& text)
{
    std::vector<std::string> lines = SplitLines(text);

    // 1. Collect every cFESpinGroup block span, by name (insertion-ordered, so
    //    "first group whose header is line i" behaves like the Python dict).
    std::vector<std::pair<std::string, BlockSpan>> groups;
    size_t pos = 0;
    while (true)
    {
        BlockSpan span;
        if (!FindBlock(lines,
                       [](const std::string& l) { return StartsWith(l, "cFESpinGroup "); },
                       pos, &span))
            break;
        // header is "cFESpinGroup <Name>" — the Python's split()[1]: the second
        // whitespace-delimited token, nothing more.
        const std::string& header = lines[span.header];
        const size_t sp = header.find_first_of(" \t");
        const size_t nb = header.find_first_not_of(" \t", sp);
        size_t ne = header.find_first_of(" \t\r", nb);
        if (ne == std::string::npos)
            ne = header.size();
        groups.push_back({header.substr(nb, ne - nb), span});
        pos = span.close + 1;
    }

    std::set<std::string> keep;
    std::map<std::string, size_t> keepIndex;
    for (size_t r = 0; r < Rows().size(); ++r)
    {
        keep.insert(Rows()[r].name);
        keepIndex[Rows()[r].name] = r;
    }
    for (const std::string& k : keep)
    {
        bool found = false;
        for (const auto& [gname, span] : groups)
            if (gname == k)
                found = true;
        if (!found)
            Refuse("options_pc.txt: expected spin group not found: " + k);
    }

    // 2. Rebuild: walk the file; HIDE unimplemented groups (Visible="false", the
    //    shipped layouts' own idiom — deleting them null-derefs in the class's
    //    widget lookups, a stub null-calls in the deferred focus walk); transform
    //    kept ones. Everything not named stays byte-identical.
    std::vector<std::string> out;
    size_t i = 0;
    while (i < lines.size())
    {
        const std::pair<std::string, BlockSpan>* hid = nullptr;
        const std::pair<std::string, BlockSpan>* kept = nullptr;
        for (const auto& g : groups)
        {
            if (g.second.header != i)
                continue;
            if (keep.count(g.first))
                kept = &g;
            else
                hid = &g;
            break;
        }
        if (hid)
        {
            // FULL body, hidden — minus its cFEAnim blocks (kept only to stay
            // inside the encoder's single-chunk limit; an invisible row's
            // animations have nothing to show).
            std::vector<std::string> body(lines.begin() + hid->second.header,
                                          lines.begin() + hid->second.close + 1);
            body.insert(body.begin() + 2, "Visible=\"false\"");
            std::vector<std::string> pruned;
            size_t j = 0;
            while (j < body.size())
            {
                if (StartsWith(body[j], "cFEAnim "))
                {
                    size_t k = j + 1;
                    if (k >= body.size() || Strip(body[k]) != "{")
                        Refuse("options_pc.txt: cFEAnim without a brace");
                    int depth = 0;
                    while (k < body.size())
                    {
                        const std::string s = Strip(body[k]);
                        if (s == "{")
                            ++depth;
                        else if (s == "}")
                        {
                            --depth;
                            if (depth == 0)
                                break;
                        }
                        ++k;
                    }
                    j = k + 1;
                    continue;
                }
                pruned.push_back(body[j]);
                ++j;
            }
            out.insert(out.end(), pruned.begin(), pruned.end());
            i = hid->second.close + 1;
            continue;
        }
        if (kept)
        {
            std::vector<std::string> glines(lines.begin() + kept->second.header,
                                            lines.begin() + kept->second.close + 1);
            std::vector<std::string> t =
                TransformGroup(glines, kept->first, keepIndex[kept->first]);
            out.insert(out.end(), t.begin(), t.end());
            i = kept->second.close + 1;
            continue;
        }
        out.push_back(lines[i]);
        ++i;
    }

    // Un-hide the screen's art (the yellow-paper backdrop): the layout ships
    // mainmenu_art Visible="false" and the PC class's compiled-out enter was what
    // re-showed it.
    for (size_t k = 0; k < out.size(); ++k)
    {
        if (!StartsWith(out[k], "cFEWidget mainmenu_art"))
            continue;
        for (size_t j = k + 1; j < std::min(k + 5, out.size()); ++j)
            if (out[j] == "Visible=\"false\"")
            {
                out.erase(out.begin() + j);
                break;
            }
        break;
    }
    return JoinLines(out);
}

// ---------------------------------------------------------------------------
// .bcs string banks (gen_pc_options.py patch_bcs)
// ---------------------------------------------------------------------------

// The strings the shipped banks lack, added to every language identically. Ids
// 100000+: the shipped bank's own maximum is 99999 (measured — 60003 was taken).
const std::vector<std::pair<uint32_t, const char*>>& NewStrings()
{
    static const std::vector<std::pair<uint32_t, const char*>> ns = {
        {100000, "1280 x 720"}, {100001, "2560 x 1440"}, {100002, "3840 x 2160"},
        {100003, "5120 x 2880"}, {100004, "Borderless"},
    };
    return ns;
}

std::map<uint32_t, Bytes> ParseBcs(const Bytes& d, const char* what,
                                   std::vector<uint32_t>* idOrder = nullptr)
{
    if (d.size() < 4)
        Refuse(std::string(what) + ": truncated .bcs");
    const uint32_t n = LE32(d.data());
    if (d.size() < 4 + 8ull * n)
        Refuse(std::string(what) + ": .bcs index truncated");
    std::map<uint32_t, Bytes> table;
    for (uint32_t k = 0; k < n; ++k)
    {
        const uint32_t id = LE32(d.data() + 4 + 4 * k);
        const uint32_t off = LE32(d.data() + 4 + 4 * n + 4 * k);
        size_t end = off;
        while (end < d.size() && d[end])
            ++end;
        table[id] = Bytes(d.begin() + off, d.begin() + end);
        if (idOrder)
            idOrder->push_back(id);
    }
    return table;
}

Bytes BuildBcs(const std::vector<uint32_t>& ids, const std::map<uint32_t, Bytes>& table)
{
    const uint32_t n = uint32_t(ids.size());
    const uint32_t header = 4 + 4 * n * 2;
    Bytes blob;
    std::vector<uint32_t> offs;
    for (uint32_t id : ids)
    {
        offs.push_back(header + uint32_t(blob.size()));
        const Bytes& s = table.at(id);
        blob.insert(blob.end(), s.begin(), s.end());
        blob.push_back(0);
    }
    Bytes out;
    AppendLE32(out, n);
    for (uint32_t id : ids)
        AppendLE32(out, id);
    for (uint32_t o : offs)
        AppendLE32(out, o);
    out.insert(out.end(), blob.begin(), blob.end());
    return out;
}

// Add the NEW_STRINGS ids and rebuild the bank sorted-by-id, then verify through
// the same reader shape the runtime implies: every id yields its bytes back.
void PatchBcs(const fs::path& src, const fs::path& dst)
{
    const Bytes d = ReadFileBytes(src);
    std::map<uint32_t, Bytes> table = ParseBcs(d, src.string().c_str());
    for (const auto& [id, s] : NewStrings())
    {
        if (table.count(id))
            Refuse(src.string() + ": id " + std::to_string(id) +
                   " already exists — pick other ids");
        table[id] = Bytes(reinterpret_cast<const uint8_t*>(s),
                          reinterpret_cast<const uint8_t*>(s) + std::strlen(s));
    }
    std::vector<uint32_t> ids;
    for (const auto& [id, s] : table)
        ids.push_back(id); // std::map iterates sorted
    const Bytes out = BuildBcs(ids, table);
    const std::map<uint32_t, Bytes> got = ParseBcs(out, dst.string().c_str());
    if (got != table)
        Refuse(dst.string() + ": verification failed");
    WriteFileBytes(dst, out);
}

// ---------------------------------------------------------------------------
// fecmn.tex — the glyph bank (gen_kbm_icons.py)
// ---------------------------------------------------------------------------

// The title's own H33 name hash over the lowercase entry name.
uint32_t H33(const std::string& s)
{
    uint32_t v = 0;
    for (char ch : s)
        v = (v * 33) ^ uint32_t(uint8_t(std::tolower(uint8_t(ch))));
    return v;
}

struct TexEntry
{
    std::string name;
    uint32_t rec[7]; // {nameOffAbs, hash, size, 0x4030, payloadOffAbs, 4, 2}
};

std::vector<TexEntry> ParseTexBank(const Bytes& data)
{
    if (data.size() < 0x18)
        Refuse("fecmn.tex: truncated bank");
    const uint32_t count = LE32(data.data() + 0xC);
    std::vector<TexEntry> entries;
    for (uint32_t i = 0; i < count; ++i)
    {
        TexEntry e;
        for (int k = 0; k < 7; ++k)
            e.rec[k] = LE32(data.data() + 0x18 + 28 * i + 4 * k);
        size_t end = e.rec[0];
        while (end < data.size() && data[end])
            ++end;
        e.name.assign(reinterpret_cast<const char*>(data.data() + e.rec[0]), end - e.rec[0]);
        entries.push_back(std::move(e));
    }
    return entries;
}

// Rebuild the bank: entry table + name blob byte-identical, payloads re-laid-out
// in the original order with patched ones substituted.
Bytes RebuildTexBank(const Bytes& data, const std::vector<TexEntry>& entries,
                     const std::map<std::string, Bytes>& patches)
{
    std::vector<size_t> order(entries.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        return entries[x].rec[4] < entries[y].rec[4];
    });
    const uint32_t firstPay = entries[order[0]].rec[4];
    Bytes out(data.begin(), data.begin() + firstPay);
    for (size_t i : order)
    {
        const TexEntry& e = entries[i];
        Bytes pay;
        auto it = patches.find(e.name);
        if (it != patches.end())
            pay = it->second;
        else
            pay.assign(data.begin() + e.rec[4], data.begin() + e.rec[4] + e.rec[2]);
        while (out.size() & 3) // payloads are 4-byte aligned in the bank
            out.push_back(0);
        const uint32_t newOff = uint32_t(out.size());
        out.insert(out.end(), pay.begin(), pay.end());
        uint8_t* rec = out.data() + 0x18 + 28 * i;
        PutLE32(rec + 0, e.rec[0]);
        PutLE32(rec + 4, e.rec[1]);
        PutLE32(rec + 8, uint32_t(pay.size()));
        PutLE32(rec + 12, e.rec[3]);
        PutLE32(rec + 16, newOff);
        PutLE32(rec + 20, e.rec[5]);
        PutLE32(rec + 24, e.rec[6]);
    }
    PutLE32(out.data() + 0x8, uint32_t(out.size()));
    return out;
}

// ---------------------------------------------------------------------------
// The layers
// ---------------------------------------------------------------------------

struct Paths
{
    fs::path game;      // assets/game
    fs::path patched;   // assets/game_patched
    fs::path kbm;       // assets/game_kbm
    fs::path chips;     // the shipped key-cap texel blobs, or empty if absent
};

fs::path FindChipsDir()
{
    std::error_code ec;
    const fs::path shipped = HostPaths::ExeDir() / "kbm_chips";
    if (fs::is_directory(shipped, ec))
        return shipped;
    const fs::path dev = HostPaths::Root() / "tools" / "release" / "kbm_chips";
    if (fs::is_directory(dev, ec))
        return dev;
    return {};
}

Paths MakePaths()
{
    Paths p;
    p.game = HostPaths::Game();
    p.patched = p.game.parent_path() / (p.game.filename().string() + "_patched");
    p.kbm = p.game.parent_path() / (p.game.filename().string() + "_kbm");
    p.chips = FindChipsDir();
    return p;
}

// The language banks are enumerated, not hard-coded: sorted str_*.bcs in the
// shipped frontend directory, exactly the Python's os.listdir walk.
std::vector<std::string> StrBankNames(const fs::path& frontend)
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(frontend, ec))
    {
        const std::string f = e.path().filename().string();
        if (StartsWith(f, "str_") && f.size() > 4 && f.rfind(".bcs") == f.size() - 4)
            names.push_back(f);
    }
    std::sort(names.begin(), names.end());
    return names;
}

// --- layer 1: assets/game_patched (gen_pc_options.py main) -----------------

void GeneratePatchedLayer(const Paths& p,
                          const std::function<void(const char*, float)>& progress)
{
    const fs::path frontend = p.game / "data" / "frontend";
    const fs::path outFrontend = p.patched / "data" / "frontend";

    if (progress)
        progress("PREPARING MENUS - OPTIONS SCREEN", 0.05f);

    // fecmn.big: decompress options_pc.txt, rewrite it, re-encode, repack.
    BigArchive fecmn = ReadBig(frontend / "fecmn.big");
    BigEntry* entry = nullptr;
    for (BigEntry& e : fecmn.entries)
        if (e.name == "options_pc.txt")
            entry = &e;
    if (!entry)
        Refuse("fecmn.big: no options_pc.txt entry");
    const Bytes original = LzxDecodeEntry(entry->stored, "options_pc.txt");
    const std::string rewritten =
        RewriteOptionsPc(std::string(original.begin(), original.end()));
    const Bytes data(rewritten.begin(), rewritten.end());
    entry->stored = LzxEncodeStream(data);
    VerifyEncodedStream(entry->stored, data, "options_pc.txt");
    entry->size2 = uint32_t(data.size());

    // Keep the originals for the repack gate before WriteBig mutates offsets.
    const BigArchive orig = ReadBig(frontend / "fecmn.big");
    WriteBig(outFrontend / "fecmn.big", fecmn, 4);

    // Verify the repack: parse it back; every entry byte-identical to what we
    // intended, including the ones we did not touch.
    BigArchive check = ReadBig(outFrontend / "fecmn.big");
    if (check.entries.size() != orig.entries.size())
        Refuse("fecmn.big repack changed the entry count");
    for (const BigEntry& oe : orig.entries)
    {
        const Bytes& want = (oe.name == "options_pc.txt") ? entry->stored : oe.stored;
        bool found = false;
        for (const BigEntry& ce : check.entries)
            if (ce.name == oe.name)
            {
                if (ce.stored != want)
                    Refuse("fecmn.big repack verification failed on " + oe.name);
                found = true;
            }
        if (!found)
            Refuse("fecmn.big repack lost " + oe.name);
    }

    if (progress)
        progress("PREPARING MENUS - BOOT PRELOAD", 0.25f);

    // preload4.big: the boot preload NESTS compressed copies of fecmn.big and
    // fecmn.tex, and the frontend reads THOSE, not the loose files. Re-encoding
    // the nested streams is a closed road (the guest decoder refused every
    // attempt); the copies are EVICTED instead — their index HASH is flipped so
    // every runtime lookup misses and the engine falls back to the loose files
    // the overlay serves patched. Byte-identical except the two hash words.
    {
        const fs::path src = p.game / "data" / "preload4.big";
        Bytes raw = ReadFileBytes(src);
        const BigArchive pre = ReadBig(src);
        int flipped = 0;
        for (size_t i = 0; i < pre.entries.size(); ++i)
        {
            const std::string& nm = pre.entries[i].name;
            if (nm == "fecmn.big" || nm == "fecmn.tex")
            {
                uint8_t* hp = raw.data() + 0x18 + i * 28 + 4;
                PutLE32(hp, LE32(hp) ^ 0xDEADBEEF);
                ++flipped;
            }
        }
        if (flipped != 2)
            Refuse("preload4.big: found " + std::to_string(flipped) +
                   " of 2 nested frontend banks to evict");
        // The gate the Python asserts: only the hash words may differ.
        const Bytes shipped = ReadFileBytes(src);
        if (shipped.size() != raw.size())
            Refuse("preload4 poison changed the file size");
        size_t diffs = 0;
        for (size_t i = 0; i < raw.size(); ++i)
            if (raw[i] != shipped[i])
                ++diffs;
        if (diffs > 8)
            Refuse("preload4 poison changed more than the hashes: " +
                   std::to_string(diffs) + " bytes");
        WriteFileBytes(p.patched / "data" / "preload4.big", raw);
    }

    // Every language bank, with the added value strings.
    const std::vector<std::string> banks = StrBankNames(frontend);
    size_t done = 0;
    for (const std::string& f : banks)
    {
        if (progress)
        {
            char l[64];
            std::snprintf(l, sizeof l, "PREPARING MENUS - STRINGS %zu OF %zu", done + 1,
                          banks.size());
            progress(l, 0.30f + 0.30f * float(done) / float(banks.size() ? banks.size() : 1));
        }
        PatchBcs(frontend / f, outFrontend / f);
        ++done;
    }

    // layout.bin PINS EVERY FILE'S SIZE — 152-byte records, BE size at +132 — and
    // the loader reads by that size, not by asking the filesystem. Every file
    // this overlay replaces gets its record updated.
    {
        Bytes layout = ReadFileBytes(p.game / "layout.bin");
        std::vector<std::string> overridden = {"data/preload4.big",
                                               "data/frontend/fecmn.big"};
        for (const std::string& f : banks)
            overridden.push_back("data/frontend/" + f);
        size_t patchedRecords = 0;
        for (size_t i = 0; i + 152 <= layout.size(); i += 152)
        {
            size_t end = i;
            while (end < i + 128 && layout[end])
                ++end;
            const std::string name(reinterpret_cast<const char*>(layout.data() + i),
                                   end - i);
            if (std::find(overridden.begin(), overridden.end(), name) == overridden.end())
                continue;
            std::error_code ec;
            const uintmax_t sz = fs::file_size(p.patched / name, ec);
            if (ec)
                Refuse("layout.bin: overridden file missing: " + name);
            uint8_t* sp = layout.data() + i + 132;
            sp[0] = uint8_t(sz >> 24); sp[1] = uint8_t(sz >> 16);
            sp[2] = uint8_t(sz >> 8); sp[3] = uint8_t(sz);
            ++patchedRecords;
        }
        if (patchedRecords != overridden.size())
            Refuse("layout.bin: only " + std::to_string(patchedRecords) + " of " +
                   std::to_string(overridden.size()) + " records found");
        WriteFileBytes(p.patched / "layout.bin", layout);
    }
}

// --- layer 2: assets/game_kbm (gen_kbm_icons.py main) ----------------------

void GenerateKbmLayer(const Paths& p,
                      const std::function<void(const char*, float)>& progress)
{
    const fs::path srcTex = p.game / "data" / "frontend" / "fecmn.tex";
    const Bytes data = ReadFileBytes(srcTex);
    const std::vector<TexEntry> entries = ParseTexBank(data);

    // GATE 1: identity repack — the container model must reproduce the shipped
    // bank byte for byte before anything is patched into it.
    if (RebuildTexBank(data, entries, {}) != data)
        Refuse("fecmn.tex GATE 1 FAILED: zero-patch rebuild is not byte-identical — "
               "the container model is wrong, refusing to write anything");

    std::map<std::string, Bytes> patches;
    // The DEVICE-FOLLOW sidecar: for every patched glyph, the 16-byte decoded-
    // record header (a unique in-memory fingerprint) plus BOTH texel sets, so the
    // runtime can swap the art in place when the active input device changes.
    struct SwapEntry
    {
        std::string base;
        Bytes fingerprint, padTex, kbTex;
    };
    std::vector<SwapEntry> swapEntries;
    size_t done = 0, patchable = 0;
    for (const TexEntry& e : entries)
    {
        const std::string base = e.name.substr(0, e.name.rfind('.'));
        std::error_code ec;
        if (fs::exists(p.chips / (base + ".dxt"), ec))
            ++patchable;
    }
    for (const TexEntry& e : entries)
    {
        const std::string base = e.name.substr(0, e.name.rfind('.'));
        const fs::path chipPath = p.chips / (base + ".dxt");
        std::error_code ec;
        if (!fs::exists(chipPath, ec))
            continue;
        if (progress)
        {
            char l[64];
            std::snprintf(l, sizeof l, "PREPARING KEY PROMPTS - %zu OF %zu", done + 1,
                          patchable);
            progress(l, 0.65f + 0.30f * float(done) / float(patchable ? patchable : 1));
        }
        // GATE 2: the stored hash must be the H33 name hash — content swaps leave
        // it untouched, so a mismatch means this is not the bank we know.
        if (e.rec[1] != H33(e.name))
            Refuse("fecmn.tex GATE 2 FAILED: " + e.name + " hash mismatch");
        const Bytes stored(data.begin() + e.rec[4], data.begin() + e.rec[4] + e.rec[2]);
        const uint32_t window = BE32(stored.data() + 4);
        if (window != 0x8000)
            Refuse("fecmn.tex: " + e.name + " window is not 0x8000");
        const Bytes old = LzxDecodeEntry(stored, e.name.c_str());
        if (old.size() < 48 || old[0] != 0x05 || old[1] != 0x01 || old[2] != 0x01 ||
            old[3] != 0xE6)
            Refuse("fecmn.tex: " + e.name + " is not the 05 01 01 E6 texture record "
                   "this generator understands");
        const Bytes hdr(old.begin(), old.begin() + 48);
        const Bytes texels(old.begin() + 48, old.end());
        const Bytes chip = ReadFileBytes(chipPath);
        if (chip.size() != texels.size())
            Refuse("fecmn.tex: " + e.name + " shipped chip is " +
                   std::to_string(chip.size()) + " bytes, the bank's texels are " +
                   std::to_string(texels.size()) + " — a different SKU's bank?");
        Bytes raw = hdr;
        raw.insert(raw.end(), chip.begin(), chip.end());
        const Bytes pay = LzxEncodeStream(raw);
        // GATE 3: round-trip through the real decompressor.
        VerifyEncodedStream(pay, raw, e.name.c_str());
        patches[e.name] = pay;
        swapEntries.push_back({base, Bytes(hdr.begin(), hdr.begin() + 16), texels, chip});
        ++done;
    }
    if (patches.empty())
        Refuse("fecmn.tex: no chip blob matched any bank entry — kbm_chips missing "
               "or wrong");

    // THE TITLE-SCREEN STRINGS: three same-length byte edits, each of which must
    // occur EXACTLY once, then the id-4049 LS -> MASH rewrite via a full table
    // rebuild (the .bcs size is not pinned). Reads the game_patched bank this
    // same run generated — the KB layer stacks on the part-60 layer.
    {
        Bytes sbank = ReadFileBytes(p.patched / "data" / "frontend" / "str_en.bcs");
        struct Edit
        {
            const char* oldB;
            size_t oldLen;
            const char* newB;
        };
        const Edit edits[] = {
            {"PRESS\0START\0", 12, "PRESS\0ENTER\0"},
            {"PRESS START\0", 12, "PRESS ENTER\0"},
            {"LEFT STICK ", 11, "A / D KEYS "},
        };
        for (const Edit& ed : edits)
        {
            size_t count = 0, at = 0;
            for (size_t s = 0; s + ed.oldLen <= sbank.size(); ++s)
                if (std::memcmp(sbank.data() + s, ed.oldB, ed.oldLen) == 0)
                {
                    ++count;
                    at = s;
                }
            if (count != 1)
                Refuse("str_en.bcs holds " + std::to_string(count) + " of a title "
                       "string expected exactly once — refusing the KB edit");
            std::memcpy(sbank.data() + at, ed.newB, ed.oldLen);
        }
        std::vector<uint32_t> idOrder;
        std::map<uint32_t, Bytes> table = ParseBcs(sbank, "str_en.bcs", &idOrder);
        const Bytes ls = {'L', 'S', ' '};
        if (table.count(4049) == 0 || table[4049] != ls)
            Refuse("str_en.bcs: string id 4049 is not 'LS ' — the bank layout "
                   "moved; refusing to rewrite");
        table[4049] = Bytes{'M', 'A', 'S', 'H'};
        const Bytes rebuilt = BuildBcs(idOrder, table); // keep the shipped id order
        if (ParseBcs(rebuilt, "str_en.bcs (rebuilt)") != table)
            Refuse("rebuilt str_en.bcs does not read back");
        WriteFileBytes(p.kbm / "data" / "frontend" / "str_en.bcs", rebuilt);
    }

    // glyph_swap.bin: the device-follow sidecar, PAD texels from the player's own
    // bank + our KB texels. No Capcom byte ships — both sets are composed here.
    {
        Bytes swp;
        swp.push_back('K'); swp.push_back('B'); swp.push_back('S'); swp.push_back('W');
        AppendLE32(swp, uint32_t(swapEntries.size()));
        for (const SwapEntry& se : swapEntries)
        {
            if (se.padTex.size() != se.kbTex.size())
                Refuse("glyph_swap: texel set size mismatch on " + se.base);
            AppendLE32(swp, uint32_t(se.base.size()));
            AppendLE32(swp, uint32_t(se.padTex.size()));
            swp.insert(swp.end(), se.base.begin(), se.base.end());
            swp.insert(swp.end(), se.fingerprint.begin(), se.fingerprint.end());
            swp.insert(swp.end(), se.padTex.begin(), se.padTex.end());
            swp.insert(swp.end(), se.kbTex.begin(), se.kbTex.end());
        }
        WriteFileBytes(p.kbm / "glyph_swap.bin", swp);
    }

    // The bank itself. THE SIZE PIN: layout.bin fixes fecmn.tex at the shipped
    // size and the loader reads by that size, so the patched bank must fit UNDER
    // it and is padded to EXACTLY it — one layout record serves both overlay
    // states.
    Bytes out = RebuildTexBank(data, entries, patches);
    if (out.size() > data.size())
        Refuse("fecmn.tex GATE FAILED: patched bank " + std::to_string(out.size()) +
               " bytes exceeds the shipped " + std::to_string(data.size()) +
               " that layout.bin pins — refusing to write");
    out.insert(out.end(), data.size() - out.size(), 0);
    PutLE32(out.data() + 0x8, uint32_t(out.size()));
    WriteFileBytes(p.kbm / "data" / "frontend" / "fecmn.tex", out);
}

// ---------------------------------------------------------------------------
// Wanted / stamp
// ---------------------------------------------------------------------------

// Bump when any transform above changes byte-for-byte output — INCLUDING a chip
// art change (re-export tools/release/kbm_chips with gen_kbm_icons.py
// --export-chips in the same commit): a shipped update must not keep serving a
// player's stale banks (the gotcha-13 shape, on disk).
constexpr int kGeneratorVersion = 1;

fs::path StampPath(const Paths& p)
{
    return p.patched / ".cz_overlay_version";
}

bool OutputsCurrent(const Paths& p)
{
    std::error_code ec;
    // The stamp carries the generator version; missing or older means regenerate.
    Bytes stamp;
    if (fs::exists(StampPath(p), ec))
    {
        FILE* f = std::fopen(StampPath(p).string().c_str(), "rb");
        if (f)
        {
            char buf[16] = {};
            (void)!std::fread(buf, 1, sizeof buf - 1, f);
            std::fclose(f);
            if (std::atoi(buf) != kGeneratorVersion)
                return false;
        }
    }
    else
        return false;

    const fs::path frontend = p.game / "data" / "frontend";
    std::vector<fs::path> wanted = {
        p.patched / "data" / "frontend" / "fecmn.big",
        p.patched / "data" / "preload4.big",
        p.patched / "layout.bin",
    };
    for (const std::string& f : StrBankNames(frontend))
        wanted.push_back(p.patched / "data" / "frontend" / f);
    if (!p.chips.empty())
    {
        wanted.push_back(p.kbm / "data" / "frontend" / "fecmn.tex");
        wanted.push_back(p.kbm / "data" / "frontend" / "str_en.bcs");
        wanted.push_back(p.kbm / "glyph_swap.bin");
    }
    for (const fs::path& w : wanted)
        if (!fs::exists(w, ec))
            return false;
    return true;
}

bool EnvSet(const char* name)
{
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

} // namespace

namespace OverlayGen
{
bool WantedAtBoot()
{
    if (EnvSet("CZ_NO_OVERLAY_GEN"))
        return false;
    // CZ_NO_PATCHED_ASSETS asked for the shipped data byte-for-byte; do not spend
    // a first-run generating files that run will then ignore.
    if (std::getenv("CZ_NO_PATCHED_ASSETS"))
        return false;
    const Paths p = MakePaths();
    std::error_code ec;
    if (!fs::exists(p.game / "data" / "frontend" / "fecmn.big", ec))
        return false; // game not unpacked yet; the first-run gate will say so
    return !OutputsCurrent(p);
}

bool Generate(const std::function<void(const char*, float)>& progress, std::string& err)
{
    try
    {
        const Paths p = MakePaths();
        GeneratePatchedLayer(p, progress);
        if (!p.chips.empty())
            GenerateKbmLayer(p, progress);
        else
            std::fprintf(stderr,
                         "[overlay] kbm_chips not found beside the executable or in "
                         "tools/release — keyboard prompt icons NOT generated (pad "
                         "art will show; the options screen and strings still work)\n");
        // The stamp is written LAST: a failed run leaves no stamp, so the next
        // boot tries again rather than trusting a half-written layer.
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d", kGeneratorVersion);
        WriteFileBytes(StampPath(p), Bytes(buf, buf + std::strlen(buf)));
        return true;
    }
    catch (const GenError& e)
    {
        err = e.msg;
        return false;
    }
}
}
