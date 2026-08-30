# Assemble the Windows release artifact (docs/release-plan.md E.2) — the sibling of
# tools/release_package_linux.sh, run ON the Windows box (ssh czwin; see
# docs/windows-build-setup.md for the tree it assumes under C:\cz).
#
# The one structural difference from Linux, stated rather than left to be rediscovered:
# THE DLLS SIT BESIDE THE EXE, NOT IN lib\. Windows' loader searches the executable's
# own directory and knows nothing about lib\ (no RPATH exists to teach it), so a lib\
# layout would need a manifest or a launcher — both of which are exactly the indirection
# the Linux script's RPATH note argues against. dxcompiler.dll is found by the shader
# translator's own search, whose second candidate is the exe directory.
#
# The MSVC runtime (vcruntime140*.dll, msvcp140.dll) IS bundled, from the toolchain's
# own redist directory — the one thing a player's machine is allowed to lack that the
# build machine cannot see missing. ucrtbase is NOT bundled: it ships with Windows 10+.
#
# Usage (through the vcvars wrapper so VCToolsRedistDir is set):
#   C:\cz\vc.bat powershell -ExecutionPolicy Bypass -File tools\release_package_windows.ps1
param(
    [string]$BuildDir = "",
    [string]$OutDir = ""
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $Root "runtime\build" }
if (-not $OutDir) { $OutDir = Join-Path $Root "dist" }
$Name = "CaseZeroRecomp"
$Stage = Join-Path $OutDir $Name

function Fail($msg) { Write-Error "FAIL: $msg"; exit 1 }

$exe = Join-Path $BuildDir "cz_runtime.exe"
if (-not (Test-Path $exe)) { Fail "no executable at $exe - build the runtime first (windows-build-setup.md section 6)" }

Write-Host "==> staging $Stage"
# PRESERVE PLAYER DATA ACROSS A REPACKAGE. The staged folder doubles as a play copy
# on this machine (the operator's sessions run from it), so assets\ can hold their
# package, extracted game, save and grown shader cache. Part 85 lost exactly that:
# an out-of-band "move assets aside" command failed silently inside an
# ssh->PowerShell->cmd quoting sandwich and the wipe below took the play copy with
# it. Preservation is THIS SCRIPT'S job now — a shell quirk cannot reach it here.
$keep = $null
$stageAssets = Join-Path $Stage "assets"
if (Test-Path $stageAssets) {
    $player = Get-ChildItem $stageAssets -Directory |
        Where-Object { $_.Name -in "game", "save", "shader_spv" }
    $pkgFiles = @(Get-ChildItem (Join-Path $stageAssets "package") -Recurse -File `
        -ErrorAction SilentlyContinue | Where-Object { $_.Length -gt 1MB })
    if ($player -or $pkgFiles) {
        $keep = Join-Path $OutDir "CaseZeroRecomp.assets.keep"
        if (Test-Path $keep) { Fail "leftover $keep exists - a previous repackage did not restore it; resolve by hand" }
        Move-Item $stageAssets $keep
        Write-Host "    preserving play-copy assets ($((Get-ChildItem $keep -Directory | ForEach-Object Name) -join ', '))"
    }
}
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
New-Item -ItemType Directory -Force -Path (Join-Path $Stage "tools"), (Join-Path $Stage "assets\package") | Out-Null

Copy-Item $exe $Stage

# The DLLs, each from the tree windows-build-setup.md section 7 builds. A hand-written
# list rather than a dependency walk, because Windows has no ldd whose output is
# trustworthy from a script — and the gate below RUNS the staged exe, which is the
# check that cannot lie about what resolves.
$dlls = @(
    @{ src = "C:\cz\thirdparty\sdl2\bin\SDL2.dll";                          why = "SDL2 (real, not sdl2-compat)" },
    @{ src = "C:\cz\thirdparty\ffmpeg-lgpl\bin\avcodec-62.dll";             why = "ffmpeg LGPL, xma only" },
    @{ src = "C:\cz\thirdparty\ffmpeg-lgpl\bin\avutil-60.dll";              why = "ffmpeg LGPL" },
    @{ src = "C:\cz\XenosRecomp\thirdparty\dxc-bin\bin\x64\dxcompiler.dll"; why = "DXC - the shader translator dlopens it" }
)
foreach ($d in $dlls) {
    if (-not (Test-Path $d.src)) { Fail "missing $($d.src) ($($d.why))" }
    Copy-Item $d.src $Stage
    $kb = [math]::Round((Get-Item $d.src).Length / 1KB)
    Write-Host ("    {0,-20} {1,8} KB  {2}" -f (Split-Path -Leaf $d.src), $kb, $d.why)
}

# The MSVC runtime, from the toolchain's redist directory (vc.bat sets
# VCToolsRedistDir). Copying from System32 instead would work on this machine and be
# the wrong files to redistribute.
if (-not $env:VCToolsRedistDir) { Fail "VCToolsRedistDir not set - run through C:\cz\vc.bat" }
$crt = Join-Path $env:VCToolsRedistDir "x64\Microsoft.VC143.CRT"
if (-not (Test-Path $crt)) { Fail "no CRT redist at $crt" }
foreach ($f in "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll") {
    Copy-Item (Join-Path $crt $f) $Stage
    Write-Host ("    {0,-20}          MSVC runtime redist" -f $f)
}

# The pre-warm seed (part 85): pipeline keys from an operator playthrough, read only
# when the player has no per-user key file yet — their first session. Header-checked
# so a truncated copy cannot ship (magic ZCPW, v1, 12 + n*56 bytes).
$pk = Join-Path $Root "tools\release\prewarm.keys"
if (-not (Test-Path $pk)) { Fail "no tools\release\prewarm.keys" }
$pkb = [IO.File]::ReadAllBytes($pk)
$pkMagic = [BitConverter]::ToUInt32($pkb, 0); $pkVer = [BitConverter]::ToUInt32($pkb, 4)
$pkN = [BitConverter]::ToUInt32($pkb, 8)
if ($pkMagic -ne 0x5750435A -or $pkVer -ne 1 -or $pkN -eq 0 -or $pkb.Length -ne (12 + $pkN * 56)) {
    Fail "prewarm.keys failed its header check (magic=$pkMagic ver=$pkVer n=$pkN len=$($pkb.Length))"
}
Copy-Item $pk $Stage
Write-Host ("    {0,-20}          pre-warm seed, {1} keys" -f "prewarm.keys", $pkN)

Copy-Item (Join-Path $Root "tools\extract_stfs.py") (Join-Path $Stage "tools")
Copy-Item (Join-Path $Root "LICENSE") $Stage
Copy-Item (Join-Path $Root "tools\release\README.md") $Stage
Copy-Item (Join-Path $Root "tools\licenses\LICENSE.DXC.txt") (Join-Path $Stage "LICENSE.DXC")

# Same defaults contract as Linux: applied by main.cpp only for variables the
# environment leaves unset, printed when applied.
@"
# Defaults for a shipped build. KEY=VALUE, one per line, # comments.
# Anything you set in the environment overrides these.
CZ_VKDRAW=1
"@ | Set-Content -Encoding ascii (Join-Path $Stage "cz_defaults.env")

@"
Put your own copy of the Dead Rising 2: Case Zero XBLA package in this directory.

It is the file your Xbox 360 downloaded. On the console's storage it lives at

    Content\0000000000000000\58410A8D\000D0000\<a long hash, no file extension>

and it is about 825 MB. Copying the whole 58410A8D folder in here works too - the
runtime looks recursively.

This build ships no game data and cannot supply it. Nothing else goes in this
directory; the unpacked files are written to ..\game\ on first run.
"@ | Set-Content -Encoding ascii (Join-Path $Stage "assets\package\PUT_YOUR_GAME_HERE.txt")

# THIRD_PARTY.md, generated like the Linux one, from the list actually copied above.
@"
# Third-party components in this build

Generated by ``tools/release_package_windows.ps1`` from the libraries staged beside
the executable. Do not edit by hand.

| component | licence | how it is here |
|---|---|---|
| XenonRecomp / XenosRecomp (hedge-dev) | MIT | the recompiled image and the translated shaders are their output |
| SDL2 | zlib | ``SDL2.dll`` beside the executable |
| ffmpeg - libavcodec, libavutil | LGPL 2.1 or later | ``avcodec-62.dll`` / ``avutil-60.dll``; built from unmodified upstream 8.1.2 with GPL parts disabled, xma1+xma2 only (recipe: ``tools/build_ffmpeg_lgpl.sh``); dynamically linked and replaceable |
| DirectX Shader Compiler (DXC) | University of Illinois/NCSA | ``dxcompiler.dll``, loaded at run time to translate shaders; licence in ``LICENSE.DXC`` |
| MSVC runtime | Microsoft redistributable | ``vcruntime140*.dll``, ``msvcp140.dll`` from the build toolchain's redist |
| o1heap | MIT | compiled in (the guest heaps) |
| SIMDe | MIT | compiled in (the guest VMX unit) |
| Vulkan loader | Apache 2.0 | NOT bundled - the GPU driver supplies it |
"@ | Set-Content -Encoding utf8 (Join-Path $Stage "THIRD_PARTY.md")

# THE GATE THAT RUNS THE STAGED EXE. Every import DLL resolves from the staged
# directory or this fails at process start - the check a dependency listing cannot be
# trusted to make. --smoke also proves the recompiled image inside the binary.
Write-Host "==> gate: --smoke on the STAGED executable"
$smoke = & (Join-Path $Stage "cz_runtime.exe") --smoke 2>&1 | Out-String
Write-Host ($smoke -split "`n" | Select-Object -Last 3 | ForEach-Object { "    $_" })
if ($LASTEXITCODE -ne 0 -or $smoke -notmatch "OK: every generated symbol resolved") {
    Fail "the staged executable did not pass --smoke"
}

Write-Host "==> archive"
$zip = Join-Path $OutDir "$Name-windows-x86_64.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $Stage -DestinationPath $zip
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
"$hash  $(Split-Path -Leaf $zip)" | Set-Content -Encoding ascii "$zip.sha256"
$mb = [math]::Round((Get-Item $zip).Length / 1MB)
Write-Host ("    {0}  {1} MB" -f (Split-Path -Leaf $zip), $mb)
Write-Host ("    sha256 {0}" -f $hash)

# Restore the preserved play-copy assets AFTER the zip, so the artifact ships the
# clean skeleton while the staged folder goes back to being the play copy.
if ($keep) {
    Remove-Item -Recurse -Force $stageAssets
    Move-Item $keep $stageAssets
    Write-Host "    play-copy assets restored into the stage (the zip carries the clean skeleton)"
}
Write-Host ""
Write-Host "==> NEXT: the one check this script cannot make - run the staged exe on a"
Write-Host "    machine (or account) without the dev tree and watch the first-run flow."
