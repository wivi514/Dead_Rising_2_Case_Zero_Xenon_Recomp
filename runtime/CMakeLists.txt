cmake_minimum_required(VERSION 3.20)

# CLANG IS REQUIRED, and this must be set before project() or CMake has already
# locked in the default compiler.
#
# Not a preference: XenonRecomp's ppc_context.h defines
#   #define PPC_FUNC_PROLOGUE() __builtin_assume(((size_t)base & 0x1F) == 0)
# and __builtin_assume is a Clang builtin. GCC has no spelling of it before the
# C++23 [[assume]] attribute, so every one of the 57,822 generated function bodies
# fails to compile under GCC — 16.1 emits "'__builtin_assume' was not declared in
# this scope" per function. Both earlier ports in this workspace build their
# runtimes with clang++ for the same reason.
#
# Override deliberately if you ever want to try otherwise:
#   cmake -DCMAKE_CXX_COMPILER=g++ ...
if(NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(CZ_CLANGXX clang++)
    if(NOT CZ_CLANGXX)
        message(FATAL_ERROR
            "clang++ not found. The recompiled image needs Clang's __builtin_assume "
            "(see the comment above this message in runtime/CMakeLists.txt).")
    endif()
    set(CMAKE_CXX_COMPILER "${CZ_CLANGXX}")
endif()

# C is enabled for exactly one file — thirdparty/o1heap/o1heap.c, the guest user
# heap. Without it CMake ignores the .c source entirely and the failure arrives at
# link time as three undefined o1heap* references, which reads like a missing
# library rather than a disabled language.
project(CaseZeroRuntime C CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING
        "Building the recompiled image with ${CMAKE_CXX_COMPILER_ID}. "
        "ppc_context.h uses __builtin_assume, which is Clang-only; expect a "
        "compile error in every generated function.")
endif()

# Phase 1 + the GPU + phase 3's window/present/input seam. There is still no
# RENDERER — that is phase 5, and a blank window is this phase's expected result.
# See docs/runtime-plan.md.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sibling XenonRecomp checkout: supplies simde (which ppc_context.h includes) and,
# from phase 1, the XEX loader in XenonUtils.
set(XENON_ROOT "$ENV{HOME}/GithubRepo/XenonRecomp" CACHE PATH "XenonRecomp source root")
set(PPC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ppc")

if(NOT EXISTS "${PPC_DIR}/ppc_func_mapping.cpp")
    message(FATAL_ERROR
        "No generated code in ${PPC_DIR}. It is gitignored (156 MB) — regenerate it:\n"
        "  mkdir -p ppc && cd config && \\\n"
        "    ${XENON_ROOT}/build/XenonRecomp/XenonRecomp CaseZero.toml \\\n"
        "    ${XENON_ROOT}/XenonUtils/ppc_context.h")
endif()

# RelWithDebInfo by default. Fable 2 measured a plain Debug build running recompiled
# VMX decode at roughly one movie frame per minute, and the addr2line workflow both
# earlier ports rely on needs the symbols.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# THE RELEASE BUILD TYPE (docs/release-plan.md A.3)
#
# `Release` is redefined here, deliberately and loudly, because CMake's default for
# it would be a PERFORMANCE CHANGE dressed as a packaging decision.
#
#   CMake's Release        -O3 -DNDEBUG
#   ours                   -O2 -g -DNDEBUG, then the debug info split off and the
#                          shipped binary stripped
#
# Two choices, each with a reason:
#
# -O2, NOT -O3. Every performance number this project has ever recorded — the whole
# corpus behind parts 47-81, the regime bands, the ns/draw decompositions, the
# pre-registered kill thresholds — was measured at -O2, because RelWithDebInfo is what
# everyone builds. Shipping at -O3 would invalidate all of it in exchange for a saving
# nobody has measured. If -O3 is ever wanted it is a measurement, run like any other
# item on this project: three runs an arm on tools/part80_crowdroute.sh. It is not a
# default to be inherited from CMake.
#
# -g KEPT, then SPLIT. The plan asks for "no -g, stripped, with a separate .debug
# artifact kept locally so a player's crash report is still resolvable" — and those
# two halves are in tension, because a binary compiled without -g has no debug info to
# keep. Compiling WITH -g and splitting afterwards satisfies both: the shipped file is
# stripped, the .debug sits beside it in the build tree, and cpu/crash_report.cpp's
# host pc — the one field that is never stale (gotcha 57) — still addr2lines.
#
# AND IT MAKES A.3's GATE FREE AND DECISIVE. -g does not affect code generation, and
# objcopy --strip-debug does not touch .text. So the release executable's .text is
# BYTE-IDENTICAL to the dev build's, and that is checkable with sha256 in a second —
# a far stronger statement than any frame-time A/B, which on this workload has a
# 10-13% noise floor at one run a side (gotcha 229). tools/release_text_identity.sh
# is that check. The route run stays in the checklist as confirmation; it is no
# longer the evidence.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g -DNDEBUG"
    CACHE STRING "Flags used by the CXX compiler during RELEASE builds." FORCE)
set(CMAKE_C_FLAGS_RELEASE   "-O2 -g -DNDEBUG"
    CACHE STRING "Flags used by the C compiler during RELEASE builds." FORCE)

# Split the debug info off the shipped executable. Defaults ON for Release and OFF
# everywhere else, so a developer never has to think about it and a release never has
# to remember it.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CZ_SPLIT_DEBUG_DEFAULT ON)
else()
    set(CZ_SPLIT_DEBUG_DEFAULT OFF)
endif()
option(CZ_SPLIT_DEBUG
       "Emit cz_runtime.debug and strip the executable (release packaging)"
       ${CZ_SPLIT_DEBUG_DEFAULT})

file(GLOB PPC_SOURCES CONFIGURE_DEPENDS "${PPC_DIR}/*.cpp")
list(LENGTH PPC_SOURCES PPC_TU_COUNT)
message(STATUS "Recompiled image: ${PPC_TU_COUNT} translation units")

# The image as a static library. Kept separate from the executable so that a
# runtime-only edit does not risk a rebuild of 227 TUs, and so the link step is
# visibly a *link* of the image rather than a compile of it.
add_library(ppc_image STATIC ${PPC_SOURCES})

target_include_directories(ppc_image PUBLIC
    ${PPC_DIR}                       # ppc_recomp_shared.h, ppc_context.h, ppc_config.h
    ${XENON_ROOT}/thirdparty/simde   # ppc_context.h includes <x86/avx.h> etc.
)

# SSE4.1 is not optional: ppc_context.h maps the guest's VMX unit onto simde, and
# several lowerings (the packs, min/max_epu32 used by the saturating vector
# arithmetic) are SSE4.1 intrinsics. Without it simde silently falls back to scalar
# emulation, which is correct but very slow.
#
# -Wno-unused-* silences the recompiler's own temporaries (`ea`, unused `temp`), not
# our code. They are emitted per-function by design and there are tens of thousands.
target_compile_options(ppc_image PRIVATE
    -msse4.1 -mavx
    -Wno-unused-variable -Wno-unused-but-set-variable)

# cpu/timebase.h is force-included over the RECOMPILED sources ONLY. `mftb` is
# lowered to __rdtsc(), which on this host runs ~70x faster than the console's
# 49.875 MHz timebase; the header shadows it with a scaled counter. The runtime's
# own sources must keep the real __rdtsc, which is why this is a source property on
# ${PPC_SOURCES} and not a target-wide option. Gotcha #1.
set_source_files_properties(${PPC_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-include;${CMAKE_CURRENT_SOURCE_DIR}/cpu/timebase.h")

# CZ_DEBUG_TUS — build named recompiled TUs at -O0 -fno-inline so a fault inside one
# resolves to an exact line, and every line in those files carries its original PPC
# mnemonic as a comment. Asura's Wrath established that host backtraces through
# recompiled frames at -O2 are not merely imprecise but actively misleading (its
# addr2line output named a function that was never called). Semicolon-separated
# basenames:  cmake .. -DCZ_DEBUG_TUS="ppc_recomp.148.cpp"
set(CZ_DEBUG_TUS "" CACHE STRING "Recompiled TU basenames to build at -O0")
foreach(tu IN LISTS CZ_DEBUG_TUS)
    if(EXISTS "${PPC_DIR}/${tu}")
        message(STATUS "CZ_DEBUG_TUS: ${tu} at -O0 -fno-inline")
        set_source_files_properties("${PPC_DIR}/${tu}" PROPERTIES COMPILE_OPTIONS
            "-include;${CMAKE_CURRENT_SOURCE_DIR}/cpu/timebase.h;-O0;-fno-inline")
    else()
        message(WARNING "CZ_DEBUG_TUS: no such TU ${PPC_DIR}/${tu}")
    endif()
endforeach()

# SDL2 — the window, the present and the pad (phase 3). REQUIRED by default rather
# than optional-with-a-fallback: the runtime can run headless (CZ_NO_WINDOW=1, and
# every gate in this project is a log diff that needs no pixels), but a build that
# silently loses its window would look exactly like a run whose input stopped
# working, and that is a session wasted. Configure with -DCZ_WINDOW=OFF to say
# "headless on purpose" out loud.
option(CZ_WINDOW "Build the SDL window / present / input seam (phase 3)" ON)

# CZ_SDL2_PREFIX — build against the SDL2 from tools/build_sdl2.sh (release builds).
#
# This exists because of a defect the A.4 clean-container gate found by RUNNING the
# packaged artifact, which `ldd` could not have found by inspecting it. Fedora's
# libSDL2-2.0.so.0 is `sdl2-compat`: a shim that dlopen()s libSDL3.so.0 at run time.
# Bundling it produced a package whose ldd was perfectly clean and which died on its
# first instruction in a container with "Failed loading SDL3 library." A dlopen is
# invisible to the tool the plan specified for exactly this job.
#
# tools/build_sdl2.sh builds real SDL2, which has no such dependency. Developing
# against the system copy stays the default — the shim is API-compatible and this
# project has run on it for eighty parts.
set(CZ_SDL2_PREFIX "" CACHE PATH
    "Prefix of an SDL2 built by tools/build_sdl2.sh (release builds)")
if(CZ_SDL2_PREFIX)
    list(PREPEND CMAKE_PREFIX_PATH "${CZ_SDL2_PREFIX}")
    message(STATUS "SDL2: ${CZ_SDL2_PREFIX} (built from source, not sdl2-compat)")
endif()

if(CZ_WINDOW)
    find_package(SDL2 QUIET)
    if(NOT SDL2_FOUND)
        message(FATAL_ERROR
            "SDL2 not found, and it is what phase 3's window, present seam and pad "
            "are built on.\n"
            "  Fedora:  sudo dnf install SDL2-devel   (sdl2-compat-devel also works)\n"
            "  Debian:  sudo apt install libsdl2-dev\n"
            "Or build without it deliberately:  cmake -S runtime -B runtime/build "
            "-DCZ_WINDOW=OFF")
    endif()
endif()

add_executable(cz_runtime
    main.cpp
    cpu/timebase.cpp
    cpu/guest_thread.cpp
    cpu/crash_report.cpp
    cpu/guest_probe.cpp
    cpu/debug_tunables.cpp
    cpu/pc_options.cpp               # part 60: the resurrected PC graphics menu
    cpu/camera_fov.cpp               # part 62: the fov-property watch (culling fix)
    cpu/thread_budget.cpp
    kernel/klog.cpp
    kernel/memory.cpp
    kernel/heap.cpp
    kernel/kobject.cpp
    kernel/imports.cpp
    kernel/file_imports.cpp
    kernel/vfs.cpp
    kernel/audio.cpp
    audio/xma_decoder.cpp            # phase A/V; ffmpeg's XMA2, one per XMA context
    audio/audio_out.cpp              # phase A/V; stubs itself out without CZ_HAVE_SDL
    kernel/content.cpp
    kernel/xex_imports.cpp
    gpu/vd.cpp
    gpu/pm4.cpp
    gpu/vk_renderer.cpp              # phase 5; inert unless CZ_VKDRAW=1
    gpu/d3d_hooks.cpp                # D3D pivot phase A; inert unless CZ_D3D_OBSERVE=1
    gpu/d3d_draw.cpp                 # D3D pivot phase C; inert unless CZ_D3D_DRAW=1
    host/host_paths.cpp              # release A.1: every path anchored to the exe
    host/first_run.cpp               # release A.2: what is missing, where it goes
    host/settings.cpp                # part 60: the PC options screen's persistent store
    host/window.cpp                  # phase 3; compiles to stubs without CZ_HAVE_SDL
    kernel/import_stubs.cpp          # generated by tools/gen_import_stubs.py
    thirdparty/o1heap/o1heap.c
)

if(CZ_WINDOW)
    target_compile_definitions(cz_runtime PRIVATE CZ_HAVE_SDL=1)
    target_link_libraries(cz_runtime PRIVATE SDL2::SDL2)
endif()

# Vulkan — phase 5's renderer. REQUIRED for the same reason SDL2 is: the renderer is
# off at runtime unless CZ_VKDRAW=1, so a machine that cannot render still builds and
# runs every gate this project owns, but a build that silently lost the renderer would
# be indistinguishable from a run where the renderer declined to start.
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    message(FATAL_ERROR
        "Vulkan not found, and phase 5's renderer is built on it.\n"
        "  Fedora:  sudo dnf install vulkan-loader-devel vulkan-headers\n"
        "  Debian:  sudo apt install libvulkan-dev")
endif()
target_link_libraries(cz_runtime PRIVATE Vulkan::Vulkan)

# ffmpeg (libavcodec) — the XMA2 decoder for guest audio, phase A/V. REQUIRED, for
# the third time and the same reason as SDL2 and Vulkan: the game's own mixer reads
# PCM that only a decoder can write, so a build that quietly lost the codec produces
# a silent game that is indistinguishable from the defect this replaced. Refusing at
# CONFIGURE time is how "no sound" stays a build message instead of a mystery.
# Headers live under /usr/include/ffmpeg on Fedora; pkg-config supplies the paths.
# CZ_FFMPEG_PREFIX — build against the LGPL ffmpeg from tools/build_ffmpeg_lgpl.sh
# rather than the system one (docs/release-plan.md §4, A.4).
#
# This is a RELEASE requirement, not a preference. Fedora's ffmpeg is configured
# --enable-gpl, which conflicts with this repo's PolyForm Noncommercial licence the
# moment the two are shipped together; and its dependency closure is 120 shared
# objects (x264, x265, librsvg, cairo, pango, OpenCL, VA-API…) against the FOURTEEN
# ffmpeg functions this runtime calls. The purpose-built one is libavcodec + libavutil,
# 1.4 MB, linking libm and libc and nothing else.
#
# Developing against the system copy stays the default: it is the same 8.1.2, so the
# headers are identical and there is no second variable.
set(CZ_FFMPEG_PREFIX "" CACHE PATH
    "Prefix of an ffmpeg built by tools/build_ffmpeg_lgpl.sh (release builds)")
if(CZ_FFMPEG_PREFIX)
    if(NOT EXISTS "${CZ_FFMPEG_PREFIX}/lib/pkgconfig/libavcodec.pc")
        message(FATAL_ERROR
            "CZ_FFMPEG_PREFIX=${CZ_FFMPEG_PREFIX} has no lib/pkgconfig/libavcodec.pc.\n"
            "  Build it:  tools/build_ffmpeg_lgpl.sh ${CZ_FFMPEG_PREFIX}")
    endif()
    message(STATUS "ffmpeg: ${CZ_FFMPEG_PREFIX} (LGPL, xma1+xma2 only)")
endif()

# Two ways to find ffmpeg, and which one runs is decided by whether we were TOLD
# where it is.
#
#   CZ_FFMPEG_PREFIX set  -> look under it directly. No pkg-config.
#   not set               -> pkg-config, the system copy, the developer default.
#
# The direct path exists because Windows has no pkg-config, and adding one as a build
# dependency to satisfy a lookup we can do ourselves would be paying for a tool to
# tell us something we already passed in on the command line. It also removes the
# .pc-file path-translation problem entirely: an MSYS2-built .pc names
# `/c/cz/...`, which a Windows CMake cannot open.
if(CZ_FFMPEG_PREFIX)
    find_path(CZ_AVCODEC_INC libavcodec/avcodec.h
              PATHS "${CZ_FFMPEG_PREFIX}/include" NO_DEFAULT_PATH REQUIRED)
    # Both spellings, because the two toolchains name the import library
    # differently and neither is wrong: libavcodec.so on Linux, avcodec.lib from
    # ffmpeg's MSVC toolchain.
    find_library(CZ_AVCODEC_LIB NAMES avcodec libavcodec
                 PATHS "${CZ_FFMPEG_PREFIX}/lib" "${CZ_FFMPEG_PREFIX}/bin"
                 NO_DEFAULT_PATH REQUIRED)
    find_library(CZ_AVUTIL_LIB NAMES avutil libavutil
                 PATHS "${CZ_FFMPEG_PREFIX}/lib" "${CZ_FFMPEG_PREFIX}/bin"
                 NO_DEFAULT_PATH REQUIRED)
    add_library(cz_ffmpeg INTERFACE)
    target_include_directories(cz_ffmpeg INTERFACE "${CZ_AVCODEC_INC}")
    target_link_libraries(cz_ffmpeg INTERFACE "${CZ_AVCODEC_LIB}" "${CZ_AVUTIL_LIB}")
    add_library(CZ::ffmpeg ALIAS cz_ffmpeg)
    message(STATUS "ffmpeg libs: ${CZ_AVCODEC_LIB}  ${CZ_AVUTIL_LIB}")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
    add_library(cz_ffmpeg INTERFACE)
    target_link_libraries(cz_ffmpeg INTERFACE PkgConfig::FFMPEG)
    add_library(CZ::ffmpeg ALIAS cz_ffmpeg)
endif()
target_link_libraries(cz_runtime PRIVATE CZ::ffmpeg)

target_include_directories(cz_runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/o1heap
    ${XENON_ROOT}/XenonUtils         # image.h (loader), xbox.h (guest structs), xex.h
)

target_link_libraries(cz_runtime PRIVATE ppc_image)

# The XEX loader. Prebuilt XenonUtils plus the two thirdparty libs it links against.
#
# This is the same loader `tools/xex_image_dump` uses, and it is deliberate that the
# runtime shares it: this XEX is LZX-compressed under the all-zero DEVKIT key, and
# `decrypt_xex.py` cannot read it at all (gotchas 15/16). One loader in the project
# means one thing to keep patched.
set(XENON_BUILD "${XENON_ROOT}/build" CACHE PATH "XenonRecomp build tree")
foreach(lib
        "${XENON_BUILD}/XenonUtils/libXenonUtils.a"
        "${XENON_BUILD}/thirdparty/fmt/libfmt.a"
        "${XENON_BUILD}/thirdparty/xxHash/cmake_unofficial/libxxhash.a")
    if(NOT EXISTS "${lib}")
        message(FATAL_ERROR
            "Missing ${lib}. Build the sibling XenonRecomp checkout first:\n"
            "  cmake -S ${XENON_ROOT} -B ${XENON_BUILD} && cmake --build ${XENON_BUILD}")
    endif()
    target_link_libraries(cz_runtime PRIVATE "${lib}")
endforeach()

# Whole-archive: the phase 0.2 gate — that EVERY generated symbol resolves — is
# still live, and is now `cz_runtime --smoke`. A normal static-library link pulls in
# only the objects it needs, so an unreferenced TU with an undefined symbol would
# link cleanly and the gate would pass while proving nothing. --whole-archive forces
# every object in. Keep this even once the runtime references most of the image on
# its own: "most" is not "every".
target_link_options(cz_runtime PRIVATE
    "-Wl,--whole-archive" "$<TARGET_FILE:ppc_image>" "-Wl,--no-whole-archive")

# ---------------------------------------------------------------------------
# CZ_SPLIT_DEBUG — the release packaging step (docs/release-plan.md A.3)
#
# Three objcopy calls, in this order and no other:
#   1. --only-keep-debug            cz_runtime.debug gets the symbols
#   2. --strip-debug                the shipped binary loses them
#   3. --add-gnu-debuglink          and gains a pointer back, so gdb/addr2line find
#                                   the .debug file automatically when it is beside it
#
# The debuglink is the part that is easy to skip and expensive to have skipped: without
# it a stripped binary and its symbols are two files that nothing connects, and the
# crash report a player sends back is a list of addresses.
#
# `.text` is untouched by all three. That is the claim tools/release_text_identity.sh
# checks, and it is what makes A.3's regime question answerable without a route run.
# CZ_BUNDLE_RPATH — look for the bundled libraries beside the executable first
# (docs/release-plan.md A.4). $ORIGIN is resolved by the loader against the executable's
# own path, so this survives the player unpacking the archive anywhere, and it needs no
# launcher script and no LD_LIBRARY_PATH — which matters because a launcher script is a
# thing a player can bypass by running the binary directly, and then the bundle silently
# is not what loads.
#
# Defaults to ON for Release only. In a dev tree there is no lib/ beside the binary and
# the entry is simply never satisfied, so it is harmless — but it is also pointless, and
# a build-tree binary that carries release packaging decisions is a binary whose
# measurements have an extra variable in them.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CZ_BUNDLE_RPATH_DEFAULT ON)
else()
    set(CZ_BUNDLE_RPATH_DEFAULT OFF)
endif()
option(CZ_BUNDLE_RPATH "Add $ORIGIN/lib to the executable's RPATH (release packaging)"
       ${CZ_BUNDLE_RPATH_DEFAULT})
if(CZ_BUNDLE_RPATH AND NOT WIN32)
    # Literal $ORIGIN, escaped so CMake does not try to expand it as a variable.
    set_target_properties(cz_runtime PROPERTIES
        BUILD_RPATH "\$ORIGIN/lib"
        INSTALL_RPATH "\$ORIGIN/lib")
endif()

if(CZ_SPLIT_DEBUG)
    find_program(CZ_OBJCOPY NAMES llvm-objcopy objcopy)
    if(NOT CZ_OBJCOPY)
        message(FATAL_ERROR
            "CZ_SPLIT_DEBUG is on but neither llvm-objcopy nor objcopy was found.\n"
            "  Fedora:  sudo dnf install binutils   (or llvm)\n"
            "Or say -DCZ_SPLIT_DEBUG=OFF to ship an unstripped binary on purpose.")
    endif()
    message(STATUS "Release packaging: cz_runtime.debug via ${CZ_OBJCOPY}")
    add_custom_command(TARGET cz_runtime POST_BUILD
        COMMAND ${CZ_OBJCOPY} --only-keep-debug
                $<TARGET_FILE:cz_runtime> $<TARGET_FILE:cz_runtime>.debug
        COMMAND ${CZ_OBJCOPY} --strip-debug $<TARGET_FILE:cz_runtime>
        COMMAND ${CZ_OBJCOPY}
                --add-gnu-debuglink=$<TARGET_FILE:cz_runtime>.debug
                $<TARGET_FILE:cz_runtime>
        COMMENT "Splitting debug info into cz_runtime.debug and stripping the binary")
endif()
