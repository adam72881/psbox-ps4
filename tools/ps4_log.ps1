# Scarica i log PSBox da GoldHEN FTP (anonimo, porta 2121).
# Uso:
#   powershell -ExecutionPolicy Bypass -File .\tools\ps4_log.ps1
#   powershell -ExecutionPolicy Bypass -File .\tools\ps4_log.ps1 -OutDir .\logs
param(
    [string]$Ps4 = "192.168.1.19",
    [int]$Port = 2121,
    [string]$OutDir = "",
    [string]$RemoteDir = "/data/psbox"
)
$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path (Join-Path $here "..")
if (-not $OutDir) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutDir = Join-Path $root ("logs\ps4-{0}" -f $stamp)
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$ftpDir = "ftp://${Ps4}:${Port}${RemoteDir}"

function Get-FtpFile([string]$RemoteName, [string]$LocalPath) {
    & curl.exe --silent --show-error --fail `
        -o $LocalPath `
        "$ftpDir/$RemoteName"
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "non scaricato: $RemoteDir/$RemoteName (exit $LASTEXITCODE)"
        return $false
    }
    $len = (Get-Item -LiteralPath $LocalPath).Length
    Write-Host ("OK {0} -> {1} ({2} byte)" -f $RemoteName, $LocalPath, $len)
    return $true
}

Write-Host "FTP $ftpDir"
Write-Host "out $OutDir"

$ok = $false
if (Get-FtpFile "xcloud.log" (Join-Path $OutDir "xcloud.log")) { $ok = $true }
if (Get-FtpFile "xcloud.prev.log" (Join-Path $OutDir "xcloud.prev.log")) { $ok = $true }

if (-not $ok) {
    throw "nessun log scaricato da $RemoteDir (FTP attivo? PSBox ha scritto /data/psbox?)"
}

Write-Host "Fatto. Apri i file in $OutDir (cerca: primo blit, flip probe, crash guard, ICE, Consent)."
