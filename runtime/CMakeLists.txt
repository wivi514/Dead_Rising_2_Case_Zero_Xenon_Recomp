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

project(CaseZeroRuntime CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING
        "Building the recompiled image with ${CMAKE_CXX_COMPILER_ID}. "
        "ppc_context.h uses __builtin_assume, which is Clang-only; expect a "
        "compile error in every generated function.")
endif()

# Phase 0.2: build the recompiled image and prove it links. There is no kernel, no
# window and no GPU here yet — those are phases 1, 3 and 4. See docs/runtime-plan.md.

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

add_executable(cz_smoke
    main.cpp
    cpu/timebase.cpp
    kernel/import_stubs.cpp          # generated by tools/gen_import_stubs.py
)
target_link_libraries(cz_smoke PRIVATE ppc_image)

# Whole-archive: the point of the phase 0.2 gate is that EVERY generated symbol
# resolves. A normal static-library link pulls in only the objects it needs, so an
# unreferenced TU with an undefined symbol would link cleanly and the gate would
# pass while proving nothing. --whole-archive forces every object in.
target_link_options(cz_smoke PRIVATE
    "-Wl,--whole-archive" "$<TARGET_FILE:ppc_image>" "-Wl,--no-whole-archive")
