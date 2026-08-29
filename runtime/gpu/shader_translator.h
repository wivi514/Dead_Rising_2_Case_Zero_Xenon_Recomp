#pragma once
// Release D.2 — in-process shader translation: microcode -> HLSL -> SPIR-V + sidecar,
// with no Python, no shell and no XenosRecomp executable on the player's machine.
//
// Why this exists (docs/release-plan.md §3.D). Part 82's D.1 gate established that the
// disc supplies the PIXEL half of the shader cache completely and the VERTEX half not
// at all — the title patches vertex fetch instructions at load, so no prebuilt vertex
// module can exist. A shipped build therefore cannot draw a single vertex shader until
// it can translate one itself, which promoted this from an optimisation to a hard
// prerequisite for the release. D.4 (translate on first sight) and D.3 (the first-run
// pass over the 1,265 disc pixel shaders) are both built on this entry point.
//
// What it replaces: the four-stage tools/build_shader_spv.sh pipeline
//     synth_shader_container.py -> XenosRecomp (exe) -> alu_const_sidecar.py -> dxc (exe)
// which remains the DEV tool (arm caches via CZ_DXC_DEFINES/CZ_HLSL_PATCH still build
// there). The two implementations are kept honest by one gate, run after any change to
// either: `cz_runtime --translate-shaders <ucode_dir> <out>` must reproduce every .spv
// and .meta.json in assets/shader_spv BYTE-FOR-BYTE. A disagreement names the shader.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ShaderTranslator
{
struct Result
{
    std::vector<uint8_t> spirv;  // the compiled module, byte-identical to the CLI's
    std::string metaJson;        // the .meta.json sidecar text, trailing newline included
    std::string hlsl;            // the intermediate, kept because it is the debuggable one
};

// Translate one shader's raw Xenos microcode. `name` is the runtime's cache key,
// "vs_<hash16>" / "ps_<hash16>" — the hash's low 16 bits become XE_SHADER_TAG, the
// identity each module carries so an instrument can paint a colour a decoder can turn
// back into a shader name. Returns false with `err` naming the stage that refused.
// Thread-safe: each call uses its own recompiler and a per-thread DXC instance.
bool Translate(const std::string& name, const uint8_t* ucode, size_t size,
               Result& out, std::string& err);

// Persist one translated shader as the .spv + .meta.json pair the cache is made of.
// Both writes gated on one success path so a partial pair (a .spv the runtime would
// silently drop for lacking its sidecar) cannot arise; on any failure both files are
// removed. Shared by TranslateDirectory and the disc prebuild (gpu/shader_prebuild.cpp).
bool WritePair(const std::filesystem::path& outDir, const std::string& name,
               const Result& r);

// The whole-directory driver behind `cz_runtime --translate-shaders <in> <out>`:
// every *.ucode in `ucodeDir` translated in parallel into `outDir` as .spv +
// .meta.json pairs. Returns 0 only if every shader translated — stricter than the
// shell script, which exits 0 with a failure list, because D.3's first-run pass needs
// the honest answer. CZ_TRANSLATE_KEEP_HLSL=<dir> keeps the intermediates.
int TranslateDirectory(const char* ucodeDir, const char* outDir);
} // namespace ShaderTranslator
