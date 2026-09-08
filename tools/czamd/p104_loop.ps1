# Part 104 item 3: loop czamd boots until the one-in-five PRE-FRAME PARK recurs, with the
# hang instruments armed on every boot (part99-amd-hang.md s5.3 built them; part 103's
# gpusplit1 run parked with none of them on).
#
# WHY A LOOP AND NOT A RUN. The park is intermittent (one czamd boot in five in part 103)
# and it has never been caught with its instruments on, so a single armed run answers
# nothing either way: a boot that comes up is the four-in-five case, not a refutation.
# Twelve boots, each ended at its first `vblank #1000` (the first presented frame - the
# park never reaches it) or at 150 s (part 103's rule: a czamd boot with no vblank #1000
# by 150 s is the hang, not a slow boot). A parked boot is left running 30 s longer so
# the periodic dumps (CZ_KOBJ_DUMP every 20 s, the [kcall+] milestone backtraces, the
# APC summary every 10 s) land in its log at least once more while it is parked.
#
# Two arm sets alternate, because CZ_GUEST_DIAG clears a byte the TITLE reads (it
# un-silences the engine's own asserts and printfs) and is therefore a guest-state change
# and not just an observer: odd boots carry the runtime-side instruments only, even boots
# add the engine's own log. A park under either set is a caught park; if it only ever
# parks under one set, that is a finding too.
#
# Copies the p103_run.ps1 boot environment verbatim (same route, same flags) so the boot
# is the boot part 103 saw park. The press sequence never matters pre-frame, but a
# different environment would be a second variable.
#
#   powershell -ExecutionPolicy Bypass -File p104_loop.ps1 [-Count 12] [-HangSecs 150]
#   -> p104\boot<N>.err.log per boot, p104\summary.txt one line per boot
param([int]$Count = 12, [int]$HangSecs = 150)
$r = "C:\Users\lisab\Desktop\CaseZeroRecomp"
$o = "$r\p104"
New-Item -ItemType Directory -Force -Path $o | Out-Null
"LOOP START $(Get-Date -Format s) count=$Count hangSecs=$HangSecs" | Tee-Object -FilePath "$o\summary.txt" -Append

for ($i = 1; $i -le $Count; $i++) {
    $tag = "boot$i"
    $env:CZ_NO_WINDOW="1"; $env:CZ_VKDRAW="1"; $env:CZ_LAUNCHER="0"; $env:CZ_FPS_CAP="500"; $env:CZ_FPS_LOG="5"
    $env:CZ_VK_RES="1920x1080"; $env:CZ_DEBUG_MENU="1"; $env:CZ_FAKE_START_MS="100"
    $env:CZ_DEBUG_FLAGS="CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS"
    $env:CZ_FAKE_PRESS_SEQ=(Get-Content "$r\crowd_seq.txt" -Raw).Trim()
    # The hang arms (all default OFF, docs/instruments.md):
    #   CZ_KCALL_WHO   guest backtrace on the first call AND every 65536th hit of these
    #                  imports - the milestone form is what names a parked hot loop's caller
    #                  (gpusplit1 parked with KeDelayExecutionThread at 720k hits and the
    #                  critical-section pair at 1.37M, callers unknown)
    #   CZ_APC_TRACE   IO-completion APC queued/drained per thread (refuted starvation once;
    #                  the balanced summary every 10 s says whether it is balanced NOW)
    #   CZ_KOBJ_DUMP   every live semaphore/event with waiters and last signaller
    #   CZ_WAIT_TRACE  any infinite wait past 5 s, with its guest callers
    #   CZ_CS_TRACE    the owner of a critical section a thread cannot get, every 4 s
    #   CZ_SCREEN_TRACE the title's own screen transitions (where the boot got to)
    #   CZ_FILE_TRACE  every open/read - part 99 located the stall by the last read issued
    $env:CZ_KCALL_WHO="KeDelayExecutionThread,RtlEnterCriticalSection,KeWaitForMultipleObjects,KeSetEvent,NtWaitForSingleObjectEx"
    $env:CZ_APC_TRACE="1"; $env:CZ_KOBJ_DUMP="20"; $env:CZ_WAIT_TRACE="1"; $env:CZ_CS_TRACE="1"
    $env:CZ_SCREEN_TRACE="1"; $env:CZ_FILE_TRACE="1"
    $guestlog = ($i % 2 -eq 0)
    if ($guestlog) { $env:CZ_GUEST_LOG="1"; $env:CZ_GUEST_DIAG="1" }
    else { Remove-Item Env:CZ_GUEST_LOG -ErrorAction SilentlyContinue; Remove-Item Env:CZ_GUEST_DIAG -ErrorAction SilentlyContinue }

    Set-Location $r
    $log = "$o\$tag.err.log"
    $t0 = Get-Date
    $p = Start-Process -FilePath "$r\cz_runtime.exe" -RedirectStandardError $log -RedirectStandardOutput "$o\$tag.out.log" -PassThru -WindowStyle Hidden
    $verdict = "PARKED"
    $readable = $true
    while (((Get-Date) - $t0).TotalSeconds -lt $HangSecs) {
        Start-Sleep -Seconds 5
        if ($p.HasExited) { $verdict = "EXITED " + $p.ExitCode; break }
        # The log is read while the process still holds it open. If Windows refuses the
        # shared read, say so in the summary rather than silently classifying by time alone.
        try {
            if (Select-String -Path $log -Pattern "vblank #1000" -Quiet -ErrorAction Stop) { $verdict = "BOOTED"; break }
        } catch { $readable = $false }
    }
    $secs = [int]((Get-Date) - $t0).TotalSeconds
    if ($verdict -eq "PARKED" -and -not $p.HasExited) { Start-Sleep -Seconds 30 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Start-Sleep -Seconds 3
    if (-not $readable) {
        # Re-classify from the closed file: the process is gone, the file is readable now.
        if (Select-String -Path $log -Pattern "vblank #1000" -Quiet) { $verdict = "BOOTED(after-kill)" }
    }
    $size = (Get-Item $log).Length
    "$tag $verdict at ${secs}s guestlog=$guestlog logbytes=$size readable=$readable $(Get-Date -Format s)" | Tee-Object -FilePath "$o\summary.txt" -Append
}
"LOOP DONE $(Get-Date -Format s)" | Tee-Object -FilePath "$o\summary.txt" -Append
