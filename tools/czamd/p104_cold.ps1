# Part 104 item 3, the COLD form: p103_cold.ps1's session-one-shaped boot (the AMD driver
# cache emptied, our pipeline cache parked under a fresh XDG_CACHE_HOME) with the hang
# instruments armed, to the 330 s timeout. The one boot condition the loop and the
# full-length runs do not cover: every part-99/100 hang and part 103's park were boots
# that followed a fresh exe deploy, and a cold cache is the only thing such a boot has
# that a warm re-boot does not.
#   powershell -ExecutionPolicy Bypass -File p104_cold.ps1 -Tag cold1 [-GuestLog]
param([string]$Tag = "cold", [switch]$GuestLog)
$r = "C:\Users\lisab\Desktop\CaseZeroRecomp"
$o = "$r\p104"
$x = "$o\xdg_$Tag"
New-Item -ItemType Directory -Force -Path $o | Out-Null
if (Test-Path $x) { Remove-Item $x -Recurse -Force }
New-Item -ItemType Directory -Force -Path $x | Out-Null
$vk = Get-ChildItem "$env:LOCALAPPDATA\AMD\VkCache" -File -ErrorAction SilentlyContinue
$before = ($vk | Measure-Object).Count
$vk | Remove-Item -Force -ErrorAction SilentlyContinue
$env:XDG_CACHE_HOME = $x
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
$after = (Get-ChildItem "$env:LOCALAPPDATA\AMD\VkCache" -File -ErrorAction SilentlyContinue | Measure-Object).Count
"$Tag $verdict at ${secs}s guestlog=$($GuestLog.IsPresent) logbytes=$size COLD vkcache=$before->$after $(Get-Date -Format s)" | Tee-Object -FilePath "$o\summary.txt" -Append
