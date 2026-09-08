# Part 104 item 3, the FULL-LENGTH form: p103_run.ps1's crowd-route boot with the hang
# instruments armed, run to the 330 s timeout like the part-103 runs were. Exists because
# p104_loop.ps1's rapid re-boots (each killed 5 s in, at its first vblank #1000) may not be
# the cadence the park needs: part 103's one park in five was among full-length runs, the
# first of a session, with ~6 minutes of gameplay and a TerminateProcess between boots.
# This reproduces that shape exactly, arms and all. CZ_KCALL_WHO's value carries commas,
# which p103_run.ps1's -Extra parser splits on, so the arms are inline here.
#
#   powershell -ExecutionPolicy Bypass -File p104_full.ps1 -Tag full1 [-GuestLog]
#   -> p104\<Tag>.err.log, one line in p104\summary.txt
param([string]$Tag = "full", [switch]$GuestLog)
$r = "C:\Users\lisab\Desktop\CaseZeroRecomp"
$o = "$r\p104"
New-Item -ItemType Directory -Force -Path $o | Out-Null
$env:CZ_NO_WINDOW="1"; $env:CZ_VKDRAW="1"; $env:CZ_LAUNCHER="0"; $env:CZ_FPS_CAP="500"; $env:CZ_FPS_LOG="5"
$env:CZ_VK_RES="1920x1080"; $env:CZ_DEBUG_MENU="1"; $env:CZ_FAKE_START_MS="100"
$env:CZ_DEBUG_FLAGS="CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS"
$env:CZ_FAKE_PRESS_SEQ=(Get-Content "$r\crowd_seq.txt" -Raw).Trim()
$env:CZ_KCALL_WHO="KeDelayExecutionThread,RtlEnterCriticalSection,KeWaitForMultipleObjects,KeSetEvent,NtWaitForSingleObjectEx"
$env:CZ_APC_TRACE="1"; $env:CZ_KOBJ_DUMP="20"; $env:CZ_WAIT_TRACE="1"; $env:CZ_CS_TRACE="1"
$env:CZ_SCREEN_TRACE="1"; $env:CZ_FILE_TRACE="1"
if ($GuestLog) { $env:CZ_GUEST_LOG="1"; $env:CZ_GUEST_DIAG="1" }
else { Remove-Item Env:CZ_GUEST_LOG -ErrorAction SilentlyContinue; Remove-Item Env:CZ_GUEST_DIAG -ErrorAction SilentlyContinue }
Set-Location $r
$log = "$o\$Tag.err.log"
$t0 = Get-Date
$p = Start-Process -FilePath "$r\cz_runtime.exe" -RedirectStandardError $log -RedirectStandardOutput "$o\$Tag.out.log" -PassThru -WindowStyle Hidden
$p | Wait-Process -Timeout 330 -ErrorAction SilentlyContinue
$verdict = "PARKED"
if ($p.HasExited) { $verdict = "EXITED " + $p.ExitCode } else { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 3
if (Select-String -Path $log -Pattern "vblank #1000" -Quiet) { $verdict = "BOOTED" }
$secs = [int]((Get-Date) - $t0).TotalSeconds
$size = (Get-Item $log).Length
"$Tag $verdict at ${secs}s guestlog=$($GuestLog.IsPresent) logbytes=$size FULL $(Get-Date -Format s)" | Tee-Object -FilePath "$o\summary.txt" -Append
