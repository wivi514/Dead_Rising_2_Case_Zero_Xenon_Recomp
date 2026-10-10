# macOS build box (`ssh czmac`) — milestone C runbook

The macOS counterpart of `windows-build-setup.md`. Set up 2026-10-09 on the operator's
**M1 MacBook Air (8 GB, 16 KB pages, 4P+4E cores, macOS 15.7.3)**. Release plan
milestone C; this records C.0 (the arm64 build) and what C.1-C.3 turned out to be.

## Access

- `~/.ssh/config` on the Linux box: `Host czmac`, key `~/.ssh/id_cz_macbuild`
  (passphrase-less, dedicated, `IdentitiesOnly yes`), verified with `SSH_AUTH_SOCK` unset.
- **Remote Login**: `systemsetup -setremotelogin on` refuses without Full Disk Access.
  `sudo launchctl load -w /System/Library/LaunchDaemons/ssh.plist` works, and
  `systemsetup -getremotelogin` may still print `Off` afterwards. "Connection refused" on
  port 22 means sshd is not loaded.
- A program started over SSH CAN open a window on the logged-in desktop (Cocoa, MoltenVK
  swapchain, both verified), but `screencapture` from SSH returns only the wallpaper:
  sshd has no Screen Recording permission. The picture needs the operator's eye or a
  `CZ_VK_FRAME_DUMP`.

## Toolchain

Apple clang 17 from the Command Line Tools; Homebrew at `/opt/homebrew`:
`cmake ninja pkg-config python@3 sdl2 ffmpeg vulkan-headers vulkan-loader molten-vk
vulkan-tools glslang spirv-tools zstd`. Every ssh command starts with
`eval "$(/opt/homebrew/bin/brew shellenv)"` (`~/.zprofile` carries it for login shells).

## Layout and how source moves

```
~/cz/Dead_Rising_2_Case_Zero_Xenon_Recomp   clone of the public repo
~/cz/{XenonRecomp,XenosRecomp,XenonLive}    rsynced from Linux (patched / private)
```
Commit on Linux, then
`git push -f czmac:cz/Dead_Rising_2_Case_Zero_Xenon_Recomp <branch>:refs/heads/from-linux`
and `git reset --hard from-linux` there. `assets/game` and `assets/shader_spv` were rsynced.

## Build

```
cd ~/cz/XenonRecomp && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build
cd ~/cz/Dead_Rising_2_Case_Zero_Xenon_Recomp && mkdir -p ppc && cd config && \
  ~/cz/XenonRecomp/build/XenonRecomp/XenonRecomp CaseZero.toml ~/cz/XenonRecomp/XenonUtils/ppc_context.h
cmake -S runtime -B runtime/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DXENON_ROOT=$HOME/cz/XenonRecomp -DXENOS_ROOT=$HOME/cz/XenosRecomp -DXLIVE_ROOT=$HOME/cz/XenonLive
ninja -C runtime/build -j4        # NOT more: 8 GB, swap reached 3.4 of 4 GB at -j4
```
The 228 recompiled TUs take ~6-7 min at -j4.

## What milestone C needed (all on branch `macos-port`)

| item | what it was |
|---|---|
| C.0 arm64 build | `-msse4.1 -mavx` made x86-only; `-Wl,-force_load` for ld64. **228 TUs, zero errors.** Generated code differs from Linux only in weak wrappers replacing `alias` (Mach-O has none) |
| XenonRecomp | **`lwsync`/`eieio` were compiler-only fences, which is correct only under x86 TSO** — on arm64 they are now `dmb ish` via `PPC_LWSYNC()`/`PPC_EIEIO()` in `ppc_context.h` (x86 codegen unchanged); its arm64 `__rdtsc()` yields to a consumer's macro; FPCR write widened to 64 bits |
| C.1 16 KB pages | null-page trap sized by `sysconf(_SC_PAGESIZE)` (covers guest 0..0x3FFF on Apple Silicon) |
| C.2 memfd | `shm_open` + immediate `shm_unlink`; the aliasing self-test passes |
| C.3 MoltenVK | **does not fire**: every REQUIRED feature present. But the bindless heap was sized from the ORDINARY limit (256 on MoltenVK) instead of the UPDATE_AFTER_BIND one (1,000,000): 256 slots, every-frame texture re-upload, **240 ms title frames -> 15.7 ms** once fixed |
| misc | Vulkan portability enumeration + `VK_KHR_portability_subset`; crash-report pc from the arm64 mcontext; sysctl for CPU/OS/RAM; `hw.physicalcpu`; thread names; no weak undefined `__llvm_profile_write_file` |

## Measured (2026-10-09, headless and windowed)

- `--smoke` OK (58,289 entries); `--diag` verdict: the renderer CAN run.
- Title screen headless: ~3,050 draws, **15.7 ms/frame median**, GPU 7.0 ms.
- Windowed: Cocoa + MoltenVK swapchain 1280x720 FIFO, 1,866 frames in 25 s.
- **The first windowed runs were BLACK** while `CZ_VK_SWAPCHAIN_DUMP` read the correct title
  picture back out of the swapchain: `SDL_Vulkan_CreateSurface` makes its NSView on the
  calling thread, which is the renderer thread. The Metal view is now made on the main
  thread at window creation and the surface built from its layer with
  `vkCreateMetalSurfaceEXT`. **Operator-verified 2026-10-10: the title screen shows.**
  For Case West: any SDL2 + Vulkan port whose renderer thread creates the surface has
  this bug on macOS only.

## Owed

- gameplay on the Mac: input, audio, saves, the crowd's frame time;
- DXC on macOS (first-sight vertex-shader translation; the cache built on Linux is
  complete for the shipped game, so this is a release item, not a blocker to play);
- fence park / wait-any on macOS (`os_sync_wait_on_address` or `__ulock_wait`): today
  they fall back to a paused spin;
- packaging (`.app`, bundled MoltenVK/SDL/ffmpeg, ad-hoc `codesign` AFTER strip — C.4);
- the gates on this platform (C.5).
