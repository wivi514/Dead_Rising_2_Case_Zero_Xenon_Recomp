# Part 118: the crowd route on a Windows box, one run per invocation, for an A/B.
#
# The Windows spelling of tools/part80_crowdroute.sh: the operator's own route into the
# ~9,000-draw crowd (config/part80_crowd_route.seq, the same CZ_FAKE_PRESS_SEQ), headless,
# 1920x1080, cap 500, one [fps] line every 10 s, then a soak standing still. The lead-in
# is the one fragile number (a boot's depth in wall time is a distribution — gotcha 75)
# and it is per machine: czamd boots in 90-130 s, czwin in ~40. Read the result with
# tools/part116_guestcpu.py on the .err.log (matched 250-draw bands, CPU per stage).
#
#   powershell -ExecutionPolicy Bypass -File crowd_ab.ps1 -Tag pin1 -LeadInMs 45000 `
#       -SoakMs 70000 -Env @{CZ_GUEST_PIN="0"}
#
# -Root is where cz_runtime.exe lives (the game's assets resolve from the exe, HostPaths);
# -Seq the route file (default: the repo's config/part80_crowd_route.seq beside -Root's
# tree, or crowd_seq.txt in -Root on a deployed box).
param(
    [string]$Tag = "run",
    [string]$Root = "",
    [string]$Seq = "",
    [int]$LeadInMs = 45000,
    [int]$SoakMs = 70000,
    [int]$TimeoutS = 320,
    [hashtable]$Env = @{}
)
if (-not $Root) { $Root = Split-Path -Parent $MyInvocation.MyCommand.Path | Split-Path -Parent | Split-Path -Parent | Join-Path -ChildPath "runtime\build" }
if (-not $Seq) {
    $cand = Join-Path (Split-Path -Parent $Root | Split-Path -Parent) "config\part80_crowd_route.seq"
    if (Test-Path $cand) { $Seq = $cand } else { $Seq = Join-Path $Root "crowd_seq.txt" }
}
$line = (Get-Content $Seq | Where-Object { $_ -match '^CZ_FAKE_PRESS_SEQ=' } | Select-Object -Last 1)
if ($line) { $base = $line.Substring($line.IndexOf('=') + 1) } else { $base = (Get-Content $Seq -Raw).Trim() }
# the recorded lead-in -> this machine's; the soak appended
$base = $base -replace '^NONE@\d+,', "NONE@$LeadInMs,"
$seq = "$base,NONE@$SoakMs,NONE"
$o = Join-Path $Root "part118"
New-Item -ItemType Directory -Force -Path $o | Out-Null
$env:CZ_NO_WINDOW = "1"; $env:CZ_VKDRAW = "1"; $env:CZ_LAUNCHER = "0"; $env:CZ_FPS_CAP = "500"; $env:CZ_FPS_LOG = "10"
$env:CZ_VK_RES = "1920x1080"; $env:CZ_NO_AUDIO_OUT = "1"; $env:CZ_FAKE_START_MS = "100"
# The route's DebugJump entry lives in the main menu only with the debug menu on, and the
# same safety flags as the Linux script (the first Windows run without them sat at the
# title for 320 s: 31 windows at 2,480 draws).
$env:CZ_DEBUG_MENU = "1"
$env:CZ_DEBUG_FLAGS = "CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS"
$env:CZ_FAKE_PRESS_SEQ = $seq
foreach ($k in $Env.Keys) { Set-Item -Path "Env:$k" -Value $Env[$k] }
Set-Location $Root
$log = "$o\$Tag.err.log"
$t0 = Get-Date
$p = Start-Process -FilePath "$Root\cz_runtime.exe" -WorkingDirectory $Root -RedirectStandardError $log -RedirectStandardOutput "$o\$Tag.out.log" -PassThru -WindowStyle Hidden
if (-not $p.WaitForExit($TimeoutS * 1000)) { Stop-Process -Id $p.Id -Force }
$secs = [int]((Get-Date) - $t0).TotalSeconds
$fps = Select-String -Path $log -Pattern '^\[fps\]' | ForEach-Object { $_.Line }
$peak = ($fps | ForEach-Object { if ($_ -match 'draws med (\d+)') { [int]$Matches[1] } } | Measure-Object -Maximum).Maximum
"$Tag : ${secs}s, $($fps.Count) fps windows, peak draws med $peak -> $log"
