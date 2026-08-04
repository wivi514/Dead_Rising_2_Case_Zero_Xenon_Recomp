// Dump a XEX's loaded image (and its section map) to a flat file for offline analysis.
//
// WHY THIS EXISTS
// ---------------
// Both template ports in this workspace analyse the game image with
// `tools/decrypt_xex.py`, a pure-Python AES-CBC decrypt plus a walk of the XEX's
// *basic* compression block table. That works because Fable 2's and Asura's Wrath's
// XEXs both use `compressionType = XEX_COMPRESSION_BASIC` (1), where the "compression"
// is only a run of (data_size, zero_size) pairs — no actual codec.
//
// Case Zero's XEX is `encryption = 1, compression = 2` (XEX_COMPRESSION_NORMAL): the
// image is AES-CBC encrypted *and* LZX-compressed, in 0x8000-byte blocks each carrying
// a 20-byte SHA-1 of the next block, with 2-byte-length chunks inside. decrypt_xex.py
// parses that FileFormatInfo as a basic block table and produces nonsense (its first
// "block" comes out as data=0x5E752CD6). There is no LZX decoder in the Python stdlib
// and none in this repo, so rather than port one, this reuses the decoder the
// recompiler itself will use — XenonUtils' `Image::ParseImage`, which wraps libmspack's
// lzxd. If the dump and the recompiler ever disagree about a byte, they are the same
// code path, which is the property we actually want from an analysis image.
//
// Output:
//   <out>            the loaded image, indexed by (guest_addr - image_base)
//   <out>.sections   text map: name, guest base, size, flags — because the flat image
//                    has holes (uninitialised sections are zero-filled here, exactly as
//                    the runtime will see them) and a scan that ignores the map will
//                    happily "find" patterns in .bss.
//
// Build (needs XenonRecomp checked out and built at ~/GithubRepo/XenonRecomp):
//   see tools/build_xex_image_dump.sh
//
// Usage:
//   ./xex_image_dump assets/game/default.xex assets/game/default_image.bin

#include <cstdio>
#include <cstdlib>
#include <vector>

#include <image.h>
#include <memory_mapped_file.h>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <default.xex> <out.bin>\n", argv[0]);
        return 1;
    }

    MemoryMappedFile file(argv[1]);
    if (!file.isOpen())
    {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }

    Image image = Image::ParseImage(file.data(), file.size());

    printf("image base : 0x%llX\n", (unsigned long long)image.base);
    printf("image size : 0x%X (%u bytes)\n", image.size, image.size);
    printf("entry point: 0x%llX\n", (unsigned long long)image.entry_point);
    printf("sections   : %zu\n", image.sections.size());

    FILE* out = fopen(argv[2], "wb");
    if (!out)
    {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    fwrite(image.data.get(), 1, image.size, out);
    fclose(out);

    std::string mapPath = std::string(argv[2]) + ".sections";
    FILE* map = fopen(mapPath.c_str(), "w");
    if (!map)
    {
        fprintf(stderr, "cannot write %s\n", mapPath.c_str());
        return 1;
    }
    fprintf(map, "# %s -- section map of %s\n", mapPath.c_str(), argv[1]);
    fprintf(map, "# image_base=0x%llX image_size=0x%X entry=0x%llX\n",
            (unsigned long long)image.base, image.size,
            (unsigned long long)image.entry_point);
    fprintf(map, "# flags: 1=code 2=data 4=bss (SectionFlags)\n");
    fprintf(map, "# name  guest_base  size  flags\n");
    for (const auto& section : image.sections)
    {
        fprintf(map, "%-10s 0x%08llX 0x%08X %u\n", section.name.c_str(),
                (unsigned long long)section.base, section.size, section.flags);
        printf("  %-10s 0x%08llX size 0x%08X flags %u\n", section.name.c_str(),
               (unsigned long long)section.base, section.size, section.flags);
    }
    fclose(map);

    printf("wrote %s (+ .sections)\n", argv[2]);
    return 0;
}
