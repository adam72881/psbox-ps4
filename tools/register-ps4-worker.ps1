# Registra il worker Cursor "adam-windows-ps4" in Avvio automatico (Task Scheduler).
# Una sola volta: doppio click su tools\register-ps4-worker.cmd
# Dopo il login Windows il worker resta online senza aprire PowerShell a mano.
param(
    [string]$WorkerName = "adam-windows-ps4",
    [string]$TaskName = "Cursor PS4 Worker adam-windows-ps4",
    [switch]$Unregister
)
$ErrorActionPreference = "Stop"

if ($Unregister) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "OK: task rimosso ($TaskName)."
    exit 0
}

function Find-AgentExe {
    $fromPath = Get-Command agent -ErrorAction SilentlyContinue
    if ($fromPath -and $fromPath.Source) { return $fromPath.Source }

    $candidates = @(
        (Join-Path $env:LOCALAPPDATA "cursor-agent\agent.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\cursor\resources\app\bin\agent.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\Cursor\resources\app\bin\agent.exe"),
        (Join-Path ${env:ProgramFiles} "Cursor\resources\app\bin\agent.exe")
    )
    foreach ($p in $candidates) {
        if ($p -and (Test-Path -LiteralPath $p)) { return $p }
    }
    return $null
}

$agent = Find-AgentExe
if (-not $agent) {
    throw @"
agent.exe non trovato. Prima installa il CLI Cursor una volta:
  irm 'https://cursor.com/install?win32=true' | iex
  agent login
Poi rilancia questo script.
"@
}

Write-Host "agent: $agent"
Write-Host "worker name: $WorkerName"

$action = New-ScheduledTaskAction -Execute $agent -Argument ("worker start --name `"{0}`"" -f $WorkerName)
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -RestartCount 5 `
    -RestartInterval (New-TimeSpan -Minutes 1) `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited

Register-ScheduledTask `
    -TaskName $TaskName `
    -Action $action `
    -Trigger $trigger `
    -Settings $settings `
    -Principal $principal `
    -Force | Out-Null

# Avvia subito se non e' gia' in esecuzione (senza finestra interattiva).
$running = Get-CimInstance Win32_Process -Filter "Name = 'agent.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match [regex]::Escape($WorkerName) }
if (-not $running) {
    Write-Host "avvio worker ora..."
    Start-Process -FilePath $agent -ArgumentList @("worker", "start", "--name", $WorkerName) -WindowStyle Minimized
} else {
    Write-Host "worker gia' in esecuzione."
}

Write-Host "OK: Task Scheduler registrato ($TaskName)."
Write-Host "Al prossimo login Windows parte da solo. Per i deploy PS4: avvia l'agente da cursor.com -> My Machines -> $WorkerName"
Write-Host "Per rimuovere: powershell -File .\tools\register-ps4-worker.ps1 -Unregister"
