#pragma once
// THE WINDOW ICON, read out of the player's own unpacked game (part 104, operator request).
//
// The package carries the title's own 64x64 tile as assets/game/X_IMAGEID_GAME.PNG — the
// icon the console's dashboard shows for it. It is Capcom's art, so it cannot ship in any
// artifact (release-plan §2.1: no game-derived byte leaves the recompiled image), but once
// the player's package is unpacked it is THEIR file, on THEIR disk, and the window may
// wear it. Before the extract it does not exist and the windows keep SDL's default.
//
// A PNG decoder is 250 lines that nothing else in this runtime had: the LGPL ffmpeg is
// built with exactly two XMA decoders and no zlib, SDL2 has no image loader, and XenonUtils
// carries LZX rather than DEFLATE. So this is a self-contained RFC 1950/1951/2083 reader
// for the ONE shape the file has (8-bit RGBA, non-interlaced; checked at load, refused
// otherwise) — deliberately not a general image library. Colour type 2 (RGB) and 3
// (palette) are accepted too, because the four sibling *.PNG files in the same directory
// are RGB, and a decoder that only ever saw one file has not been shown to decode.
#include <cstdint>
#include <filesystem>
#include <vector>

namespace PngIcon
{
struct Image
{
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba; // width*height*4, top-down, straight alpha
};

// Decode a PNG file into RGBA8. Returns false (and says why on stderr) for anything it
// does not handle — a refused icon is a default icon, never a crash.
bool Load(const std::filesystem::path& file, Image& out);

// The title's own tile, if the game is unpacked: <assets/game>/X_IMAGEID_GAME.PNG.
// Cached after the first successful load; an empty Image when the file is absent.
const Image& GameTile();
} // namespace PngIcon
