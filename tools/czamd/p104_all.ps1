# Part 104 item 3, the whole campaign as one detached task (schtasks /run /tn cz_p104all):
# 100 rapid boots (p104_loop.ps1, each ended at its first vblank #1000 or at 150 s), then
# six full-length crowd-route runs (p104_full.ps1, 330 s each, guest log on the even ones)
# - the part-103 cadence that produced the park. Either form's PARKED line in
# p104\summary.txt is the catch; its <tag>.err.log holds the instruments' output.
$r = "C:\Users\lisab\Desktop\CaseZeroRecomp"
& powershell -ExecutionPolicy Bypass -File "$r\p104_loop.ps1" -Count 100 -HangSecs 150
for ($i = 1; $i -le 6; $i++) {
    if ($i % 2 -eq 0) { & powershell -ExecutionPolicy Bypass -File "$r\p104_full.ps1" -Tag "full$i" -GuestLog }
    else { & powershell -ExecutionPolicy Bypass -File "$r\p104_full.ps1" -Tag "full$i" }
}
"CAMPAIGN DONE $(Get-Date -Format s)" | Out-File -FilePath "$r\p104\summary.txt" -Append
