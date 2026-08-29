// See stfs_extract.h for why this exists and what it deliberately does not do.
//
// The container walk mirrors tools/extract_stfs.py, which mirrors Xenia-Canary's
// xcontent_container_device.cc (BSD, read as a format reference). The arithmetic
// worth a comment is the hash-table interleave: data lives in 0x1000-byte blocks,
// every 170 data blocks are preceded by one hash-table block, every 170 of THOSE by a
// second-level table, and so on — so a data block's byte offset is its index plus the
// number of tables sitting in front of it, per level. It is easy to be off by one
// table here and the failure mode is silent garbage, which is why the port keeps the
// reference's exact expressions rather than "simplifying" them.
#include "stfs_extract.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace
{
constexpr uint32_t kBlockSize = 0x1000;
constexpr uint32_t kBlocksPerHashLevel[3] = {170, 28900, 4913000};
constexpr uint32_t kEndOfChain = 0xFFFFFF;
constexpr uint32_t kEntriesPerDirBlock = kBlockSize / 0x40;

// Container header offsets (the struct layout in the Xenia reference; identical
// constants in tools/extract_stfs.py).
constexpr uint32_t kOffHeaderSize = 0x340;       // be u32
constexpr uint32_t kOffMetadata = 0x344;
constexpr uint32_t kOffContentType = kOffMetadata + 0x00;
constexpr uint32_t kOffExecutionInfo = kOffMetadata + 0x10;
constexpr uint32_t kOffVolumeDescriptor = kOffMetadata + 0x35;
constexpr uint32_t kOffVolumeType = kOffMetadata + 0x65; // be u32: 0 STFS, 1 SVOD
constexpr uint32_t kOffDisplayName = 0x411;              // utf-16-be, 0x80 bytes

uint32_t BeU32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t BeU16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint16_t LeU16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
// STFS stores block numbers as 3-byte little-endian.
uint32_t U24Le(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16); }

// Byte offset of data block `blockIndex`, skipping interleaved hash tables. The +1 in
// the division is because a table precedes the range it covers, so block 0 already
// sits behind one level-0 table.
uint64_t BlockIndexToOffset(uint64_t baseOffset, uint32_t blockIndex)
{
    uint64_t block = blockIndex;
    for (uint32_t levelBase : kBlocksPerHashLevel)
    {
        block += (uint64_t(blockIndex) + levelBase) / levelBase;
        if (blockIndex < levelBase)
            break;
    }
    return baseOffset + (block << 12);
}

uint64_t BlockIndexToHashBlockNumber(uint32_t blockIndex)
{
    if (blockIndex < kBlocksPerHashLevel[0])
        return 0;
    uint64_t block = uint64_t(blockIndex / kBlocksPerHashLevel[0]) * (kBlocksPerHashLevel[0] + 1);
    block += blockIndex / kBlocksPerHashLevel[1] + 1;
    if (blockIndex < kBlocksPerHashLevel[1])
        return block;
    return block + 1;
}

// A directory entry name is player-supplied input. Refuse anything that could step
// outside outDir: empty components, ".", "..", separators inside a single name.
bool SafeName(const std::string& name)
{
    if (name.empty() || name == "." || name == "..")
        return false;
    return name.find('/') == std::string::npos && name.find('\\') == std::string::npos;
}

struct Entry
{
    std::string path;
    uint32_t length;
    uint32_t startBlock;
    uint32_t blockCount;
};

class Reader
{
public:
    bool Open(const fs::path& p, std::string& err)
    {
        f_.open(p, std::ios::binary);
        if (!f_)
        {
            err = "cannot open " + p.string();
            return false;
        }
        std::error_code ec;
        size_ = fs::file_size(p, ec);
        if (ec)
        {
            err = "cannot stat " + p.string();
            return false;
        }
        return true;
    }

    // Every read goes through here so a malformed package that points a block past
    // the end of the file is an error that says so, not a short read used as data.
    bool ReadAt(uint64_t off, void* dst, size_t n, std::string& err)
    {
        if (off + n > size_)
        {
            char buf[128];
            snprintf(buf, sizeof buf,
                     "read of %zu bytes at 0x%" PRIX64 " is past the package end (0x%" PRIX64 ")",
                     n, off, size_);
            err = buf;
            return false;
        }
        f_.seekg(std::streamoff(off));
        f_.read(reinterpret_cast<char*>(dst), std::streamsize(n));
        if (!f_)
        {
            err = "short read from package";
            return false;
        }
        return true;
    }

    uint64_t size() const { return size_; }

private:
    std::ifstream f_;
    uint64_t size_ = 0;
};
} // namespace

namespace StfsExtract
{
bool Extract(const fs::path& package, const fs::path& outDir, std::string& err,
             const std::function<void(uint64_t, uint64_t)>& progress)
{
    Reader r;
    if (!r.Open(package, err))
        return false;

    // The header, and the identity print. 0x1000 covers everything up to the display
    // name comfortably and every real package is megabytes.
    uint8_t hdr[0x1000];
    if (!r.ReadAt(0, hdr, sizeof hdr, err))
        return false;

    const bool live = memcmp(hdr, "LIVE", 4) == 0;
    const bool con = memcmp(hdr, "CON ", 4) == 0;
    const bool pirs = memcmp(hdr, "PIRS", 4) == 0;
    if (!live && !con && !pirs)
    {
        err = "not an XContent package (no LIVE/CON/PIRS magic)";
        return false;
    }

    const uint32_t headerSize = BeU32(hdr + kOffHeaderSize);
    const uint32_t contentType = BeU32(hdr + kOffContentType);
    const uint32_t titleId = BeU32(hdr + kOffExecutionInfo + 12);
    const uint32_t volumeType = BeU32(hdr + kOffVolumeType);

    // Display name: UTF-16BE, printed as its ASCII subset — every character this
    // title uses, and a lossy print beats a codec for an identity line.
    std::string displayName;
    for (uint32_t i = 0; i < 0x80; i += 2)
    {
        const uint16_t c = BeU16(hdr + kOffDisplayName + i);
        if (c == 0)
            break;
        displayName += (c >= 0x20 && c < 0x7F) ? char(c) : '?';
    }

    fprintf(stderr,
            "[extract] package : %s (%s)\n"
            "[extract] name    : %s\n"
            "[extract] title id: %08X   content type: %08X\n",
            package.filename().string().c_str(), live ? "LIVE" : con ? "CON" : "PIRS",
            displayName.c_str(), titleId, contentType);

    if (volumeType == 1)
    {
        // SVOD is a different on-disk shape (sibling .data/ fragment files) that no
        // XBLA package uses. Refuse by name rather than ship an untested walk.
        err = "this is an SVOD package, which this build does not unpack — use\n"
              "    python3 tools/extract_stfs.py \"" + package.string() + "\" -o <out>";
        return false;
    }
    if (volumeType != 0)
    {
        err = "unknown volume type " + std::to_string(volumeType);
        return false;
    }

    // The STFS volume descriptor.
    const uint8_t* vd = hdr + kOffVolumeDescriptor;
    if (vd[0] != 0x24)
    {
        err = "bad STFS volume descriptor length";
        return false;
    }
    if (!(vd[2] & 0x01))
    {
        // Read-write packages put the active hash table at a +1 offset per range;
        // guessing wrong reads hash tables as data. Refuse rather than produce
        // garbage — same rule as the reference.
        err = "package is not read-only format; unsupported";
        return false;
    }
    const uint32_t tableBlockCount = LeU16(vd + 3);
    uint32_t tableBlockIndex = U24Le(vd + 5);

    // Data starts at the first 0x1000 boundary at/after the header.
    const uint64_t baseOffset = (uint64_t(headerSize) + kBlockSize - 1) / kBlockSize * kBlockSize;

    // The low 24 bits of a block's hash entry `info` word are the NEXT block number —
    // a file is a linked list, not an extent.
    auto nextBlock = [&](uint32_t blockIndex, uint32_t* out, std::string& e) {
        const uint64_t tableOff = baseOffset + (BlockIndexToHashBlockNumber(blockIndex) << 12);
        const uint64_t entryOff = tableOff + (blockIndex % kBlocksPerHashLevel[0]) * 0x18;
        uint8_t info[4];
        if (!r.ReadAt(entryOff + 0x14, info, 4, e))
            return false;
        *out = BeU32(info) & 0xFFFFFF;
        return true;
    };

    // The directory: 0x40-byte entries, a parent-ordinal tree flattened into paths.
    std::vector<Entry> files;
    std::map<uint32_t, std::string> dirNames; // entry ordinal -> "path/"
    uint32_t ordinal = 0;
    uint64_t totalBytes = 0;
    for (uint32_t t = 0; t < tableBlockCount; t++)
    {
        uint8_t block[kBlockSize];
        if (!r.ReadAt(BlockIndexToOffset(baseOffset, tableBlockIndex), block, sizeof block, err))
            return false;
        for (uint32_t j = 0; j < kEntriesPerDirBlock; j++)
        {
            const uint8_t* e = block + j * 0x40;
            if (e[0] == 0)
                break;
            const uint8_t eflags = e[40];
            const std::string name(reinterpret_cast<const char*>(e), eflags & 0x3F);
            if (!SafeName(name))
            {
                err = "refusing directory entry with unsafe name: \"" + name + "\"";
                return false;
            }
            const uint16_t parent = BeU16(e + 50);
            std::string base;
            if (auto it = dirNames.find(parent); it != dirNames.end())
                base = it->second;
            if (eflags & 0x80)
                dirNames[ordinal] = base + name + "/";
            else
            {
                files.push_back({base + name, BeU32(e + 52), U24Le(e + 47), U24Le(e + 44)});
                totalBytes += files.back().length;
            }
            ordinal++;
        }
        if (!nextBlock(tableBlockIndex, &tableBlockIndex, err))
            return false;
        if (tableBlockIndex == kEndOfChain)
            break;
    }

    // default.xex last — see the header comment on interruption safety.
    for (size_t i = 0; i < files.size(); i++)
        if (files[i].path == "default.xex")
        {
            std::swap(files[i], files.back());
            break;
        }

    fprintf(stderr, "[extract] %zu files, %" PRIu64 " MB -> %s\n", files.size(),
            totalBytes >> 20, outDir.string().c_str());

    uint64_t written = 0;
    int lastPercent = -1;
    std::vector<uint8_t> buf;
    for (const auto& fe : files)
    {
        buf.clear();
        buf.reserve(fe.length);
        uint32_t remaining = fe.length;
        uint32_t block = fe.startBlock;
        for (uint32_t k = 0; k < fe.blockCount && remaining && block != kEndOfChain; k++)
        {
            const uint32_t n = remaining < kBlockSize ? remaining : kBlockSize;
            const size_t at = buf.size();
            buf.resize(at + n);
            if (!r.ReadAt(BlockIndexToOffset(baseOffset, block), buf.data() + at, n, err))
            {
                err = fe.path + ": " + err;
                return false;
            }
            remaining -= n;
            if (!nextBlock(block, &block, err))
            {
                err = fe.path + ": " + err;
                return false;
            }
        }
        if (remaining)
        {
            err = fe.path + ": block chain ended " + std::to_string(remaining) + " bytes short";
            return false;
        }

        const fs::path dest = outDir / fe.path;
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(buf.size())))
        {
            err = "cannot write " + dest.string();
            return false;
        }

        written += fe.length;
        if (progress)
            progress(written, totalBytes);
        const int percent = totalBytes ? int(written * 100 / totalBytes) : 100;
        if (percent / 10 != lastPercent / 10)
        {
            fprintf(stderr, "[extract] %3d%%  (%" PRIu64 " MB)\n", percent, written >> 20);
            lastPercent = percent;
        }
    }

    fprintf(stderr, "[extract] done: %zu files, %" PRIu64 " bytes\n", files.size(), written);
    return true;
}
} // namespace StfsExtract
