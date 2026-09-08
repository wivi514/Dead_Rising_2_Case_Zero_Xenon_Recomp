# Waits for p104_all.ps1's CAMPAIGN DONE line, then four cold armed boots (p104_cold.ps1).
$r = "C:\Users\lisab\Desktop\CaseZeroRecomp"
while (-not (Select-String -Path "$r\p104\summary.txt" -Pattern "CAMPAIGN DONE" -Quiet)) { Start-Sleep -Seconds 20 }
for ($i = 1; $i -le 4; $i++) {
    if ($i % 2 -eq 0) { & powershell -ExecutionPolicy Bypass -File "$r\p104_cold.ps1" -Tag "cold$i" -GuestLog }
    else { & powershell -ExecutionPolicy Bypass -File "$r\p104_cold.ps1" -Tag "cold$i" }
}
"COLD DONE $(Get-Date -Format s)" | Out-File -FilePath "$r\p104\summary.txt" -Append
