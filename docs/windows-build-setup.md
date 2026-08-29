# The Windows build laptop — a verified runbook

> **Every step here was executed and its result observed.** This is not a plan; the
> plan is `docs/release-plan.md` milestone B. Written after the fact precisely because
> an untested runbook is a document that reads like knowledge and behaves like a guess
> (gotcha 13).
>
> **Machine class:** Windows 11 23H2, a 20-thread laptop with 16 GB and a discrete
> NVIDIA GPU. The discrete GPU is the part that matters: it is what makes B.4's renderer
> gates runnable there rather than `--smoke` only.
>
> **The actual host, user and address are deliberately NOT in this repository.** They
> live in `~/DR2CZ-troubleshooting/windows-build-host.txt`, outside the tree, for the
> same reason the operator screenshots do. Substitute them for the `<...>` placeholders
> below.

## 1. Reaching it

`~/.ssh/config` on the Linux box:

```
Host czwin
  HostName <BUILD_HOST_IP>
  User <BUILD_HOST_USER>
  IdentityFile ~/.ssh/id_cz_winbuild
  IdentitiesOnly yes
  ServerAliveInterval 30
  ServerAliveCountMax 6
```

The key is **passphrase-less and deliberately so**: a build that outlives the operator's
login session cannot depend on an ssh-agent, and the first attempt at this did. It is
authorized only on this laptop. `IdentitiesOnly yes` is not decoration — without it ssh
offers the agent's keys first and a green result tells you nothing about whether the
dedicated key works. Test it the way that can actually fail:

```
env -u SSH_AUTH_SOCK ssh czwin whoami        # prints <HOST>\<BUILD_HOST_USER>
```

**Enabling the server, once, in an ADMIN PowerShell on the laptop:**

```powershell
Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
Set-Service -Name sshd -StartupType Automatic; Start-Service sshd
New-NetFirewallRule -Name sshd -DisplayName 'OpenSSH Server' -Enabled True `
  -Direction Inbound -Protocol TCP -Action Allow -LocalPort 22
New-ItemProperty -Path HKLM:\SOFTWARE\OpenSSH -Name DefaultShell -Force `
  -Value "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"
```

**Authorizing a key is the part that goes wrong.** For an ADMINISTRATOR account Windows
OpenSSH ignores `~/.ssh/authorized_keys` entirely — `sshd_config` ends with

```
Match Group administrators
       AuthorizedKeysFile __PROGRAMDATA__/ssh/administrators_authorized_keys
```

and that file must be **owned by Administrators** with only Administrators and SYSTEM on
its ACL. Owner, not just permissions: `icacls /inheritance:r` leaves the creating user as
owner and `StrictModes` rejects it.

```powershell
$f = "C:\ProgramData\ssh\administrators_authorized_keys"
[System.IO.File]::WriteAllText($f, $key + "`n", (New-Object System.Text.ASCIIEncoding))
$acl = Get-Acl $f
$acl.SetOwner((New-Object System.Security.Principal.SecurityIdentifier('S-1-5-32-544')))
Set-Acl $f $acl
icacls $f /inheritance:r /grant "*S-1-5-32-544:F" /grant "*S-1-5-18:F"
ssh-keygen -l -f $f          # fingerprint must match `ssh-keygen -lf` on the client
```

**And when it still fails, read the server's log rather than guessing at the file.**
Setting `LogLevel DEBUG3` — *before* the first `Match` line, because directives after
`Match` belong to that block and `LogLevel` is illegal there, so sshd refuses to start —
turns an opaque `Permission denied` into the exact reason. See gotcha 490 for why this
matters more than it sounds.

```powershell
Get-WinEvent -LogName OpenSSH/Operational -MaxEvents 60 | Sort TimeCreated |
  ForEach-Object { $_.TimeCreated.ToString('HH:mm:ss') + '  ' + $_.Message }
```

## 2. Running commands on it

Three quoting layers (bash -> ssh -> PowerShell) is one too many. `-EncodedCommand` takes
UTF-16LE base64 and has none:

```bash
b64=$(python3 -c "import sys;from base64 import b64encode;print(b64encode(open(sys.argv[1]).read().encode('utf-16-le')).decode())" script.ps1)
ssh czwin "powershell -NoProfile -NonInteractive -EncodedCommand $b64"
```

**And run every native tool through `cmd`**, or PowerShell turns each stderr line into an
`ErrorRecord` and the whole transcript comes back as CLIXML:

```powershell
function sh($c) { & cmd /c "$c 2>&1" }
```

## 3. The toolchain

```powershell
winget install --id Kitware.CMake -e --silent --accept-source-agreements --accept-package-agreements
winget install --id Ninja-build.Ninja -e --silent ...
winget install --id LLVM.LLVM -e --silent ...
winget install --id Python.Python.3.12 -e --silent ...
winget install --id MSYS2.MSYS2 -e --silent ...           # ffmpeg's configure is a shell script
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --silent --override `
  "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
```

**winget installs LLVM without putting it on PATH** — add it, or `clang-cl` reads as
missing while sitting on disk:

```powershell
[Environment]::SetEnvironmentVariable("PATH",
  [Environment]::GetEnvironmentVariable("PATH","Machine") + ";C:\Program Files\LLVM\bin", "Machine")
Restart-Service sshd      # sshd hands each session ITS OWN environment block, captured at service start
```

The Vulkan SDK (`KhronosGroup.VulkanSDK`) was already present at `C:\VulkanSDK\1.4.350.0`.

**`C:\cz\vc.bat` is the single entry point for every build.** Everything MSVC needs —
`INCLUDE`, `LIB`, the linker, the SDK — exists only inside a `vcvars64` environment, and a
command run outside it fails in ways that read like missing source rather than a missing
environment:

```bat
@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=C:\Program Files\LLVM\bin;%PATH%
%*
```

## 4. Which compiler, and it is not a preference

There are three on the box and they are not interchangeable:

| target | compiler | why not the others |
|---|---|---|
| **XenonRecomp** | `clang-cl` | `cl` rejects `XenonUtils/byteswap.h`, which calls `__builtin_bswap16/32/64` with no MSVC fallback. Plain `clang` is the GNU-like driver, and CMake then rejects the `MSVC_DEBUG_INFORMATION_FORMAT` XenonRecomp's own CMakeLists sets. `clang-cl` is the MSVC-like driver **with** the GCC builtins. |
| **SDL2** | `cl` | Under `clang-cl`, SDL2's own libc substitution loses: the compiler still lowers string ops to intrinsics nothing provides — `lld-link: undefined symbol: strlen / wcslen`. SDL2 is plain C with no clang dependency. |
| **the runtime** | `clang-cl` | `ppc_context.h` needs `__builtin_assume`; the recompiled image will not build under MSVC at all. |

## 5. The tree

```
C:\cz\
  vc.bat                                   the vcvars wrapper — build only through this
  Dead_Rising_2_Case_Zero_Xenon_Recomp\    cloned from GitHub (public); git is the ONLY
                                           way source moves between the two machines
  XenonRecomp\                             copied as a tarball: our Linux checkout is a
                                           SHALLOW clone and cannot produce a usable bundle
  XenosRecomp\                             part 84 (release D.2): a 19 MB tarball SUBSET —
                                           XenosRecomp/ sources, thirdparty/dxc-bin/inc,
                                           and the two dxcompiler libraries. Nothing is
                                           BUILT from it: the runtime compiles
                                           shader_recompiler.cpp from source and dlopens
                                           dxcompiler.dll. Ours carries local patches, so
                                           a fresh GitHub clone is NOT a substitute
  thirdparty\sdl2\  thirdparty\ffmpeg-lgpl\
  seed\                                    the source tarballs, byte-identical to Linux
```

## 6. Building

```powershell
# XenonRecomp
C:\cz\vc.bat cmake -S C:\cz\XenonRecomp -B C:\cz\XenonRecomp\build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
C:\cz\vc.bat cmake --build C:\cz\XenonRecomp\build

# ppc/  -- the directory must EXIST first or XenonRecomp segfaults inside fwrite
cd C:\cz\Dead_Rising_2_Case_Zero_Xenon_Recomp\config
C:\cz\vc.bat C:\cz\XenonRecomp\build\XenonRecomp\XenonRecomp.exe CaseZero.toml `
  C:\cz\XenonRecomp\XenonUtils\ppc_context.h
#   -> 228 TUs, 152.7 MB, zero errors, 8 seconds. The recompilation is fully portable.

# the runtime
C:\cz\vc.bat cmake -S ...\runtime -B ...\runtime\build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
  -DXENON_ROOT=C:/cz/XenonRecomp -DXENON_BUILD=C:/cz/XenonRecomp/build `
  -DXENOS_ROOT=C:/cz/XenosRecomp `
  -DCMAKE_PREFIX_PATH=C:/cz/thirdparty/sdl2 -DCZ_SDL2_PREFIX=C:/cz/thirdparty/sdl2 `
  -DCZ_FFMPEG_PREFIX=C:/cz/thirdparty/ffmpeg-lgpl -DCZ_SPLIT_DEBUG=OFF -DCZ_BUNDLE_RPATH=OFF
C:\cz\vc.bat cmake --build ...\runtime\build -- -k 0     # -k 0 goes to NINJA: keep going,
                                                         # so ONE run enumerates every failure

# dxcompiler.dll must sit beside cz_runtime.exe (or in lib\, or be named by CZ_DXC_LIB) —
# the in-process shader translator dlopens it and prints one [shxlate] line saying which
copy /y C:\cz\XenosRecomp\thirdparty\dxc-bin\bin\x64\dxcompiler.dll ...\runtime\build\
```

**After any pull-and-build, verify the pulled HEAD (`git log --oneline -1`), not the absence
of error text.** Part 84 chained `git pull >nul 2>&1 && cmake --build`; the pull failed
silently, the build had nothing to do, and `--smoke` passed by exercising the PREVIOUS
binary — two claims were made against a two-commit-stale tree before a new CLI flag falling
through to old code gave it away (gotcha 502).

## 7. The dependencies, built from the SAME sources as Linux

Deliberate: the two platforms then differ in their toolchain and in nothing else.

* **SDL2 2.32.10**, `cl`, shared. Not `sdl2-compat` — see gotcha 485.
* **ffmpeg 8.1.2**, LGPL, `xma1,xma2` only, via MSYS2 with `--toolchain=msvc`.
  `MSYS2_PATH_TYPE=inherit` is load-bearing: without it `bash -l` resets PATH, `cl.exe`
  vanishes, and configure reports "C compiler test failed" as though the compiler were
  broken. **This build HAS its x86 assembly** (MSYS2 supplies `nasm`) where the Linux one
  does not — the Linux artifact should be rebuilt once `nasm` is installed there.
