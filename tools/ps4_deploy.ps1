# Carica PSBox-PS4-*.pkg su GoldHEN FTP (anonimo, porta 2121).
# Cancella solo i vecchi PSBox-PS4-*.pkg in /data/pkg (non tocca DiPlay_*).
# Uso:
#   powershell -ExecutionPolicy Bypass -File .\tools\ps4_deploy.ps1
#   powershell -ExecutionPolicy Bypass -File .\tools\ps4_deploy.ps1 -Pkg .\dist\PSBox-PS4-01.38.pkg
param(
    [string]$Ps4 = "192.168.1.19",
    [int]$Port = 2121,
    [string]$Pkg = "",
    [string]$RemoteDir = "/data/pkg"
)
$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path (Join-Path $here "..")
if (-not $Pkg) {
    $candidates = Get-ChildItem -Path (Join-Path $root "dist") -Filter "PSBox-PS4-*.pkg" -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending
    if (-not $candidates) { throw "nessun PSBox-PS4-*.pkg in dist\. Specifica -Pkg." }
    $Pkg = $candidates[0].FullName
}
$Pkg = (Resolve-Path $Pkg).Path
$name = Split-Path -Leaf $Pkg
$ftpRoot = "ftp://${Ps4}:${Port}"
$ftpDir = "$ftpRoot$RemoteDir"

function Invoke-Curl([string[]]$CurlArgs) {
    & curl.exe @CurlArgs
    if ($LASTEXITCODE -ne 0) { throw "curl.exe fallito (exit $LASTEXITCODE): $($CurlArgs -join ' ')" }
}

Write-Host "FTP $ftpDir"
Write-Host "pkg  $Pkg"

# Elenco remoto (nomi file).
$listFile = Join-Path $env:TEMP ("ps4-pkg-list-{0}.txt" -f [guid]::NewGuid().ToString("n"))
try {
    Invoke-Curl @("--silent", "--show-error", "--list-only", "$ftpDir/", "-o", $listFile)
    $remote = @(Get-Content -LiteralPath $listFile -ErrorAction SilentlyContinue | ForEach-Object { $_.Trim() } | Where-Object { $_ })
} finally {
    Remove-Item -LiteralPath $listFile -ErrorAction SilentlyContinue
}

$keepPrefix = "DiPlay_"
$toDelete = @($remote | Where-Object {
        $_ -like "PSBox-PS4-*.pkg" -and $_ -ne $name -and -not ($_ -like "$keepPrefix*")
    })

foreach ($f in $toDelete) {
    Write-Host "elimino remoto $RemoteDir/$f"
    Invoke-Curl @("--silent", "--show-error", "-Q", "DELE $RemoteDir/$f", "$ftpRoot/")
}

Write-Host "carico $name -> $RemoteDir/$name"
Invoke-Curl @(
    "--silent", "--show-error",
    "--upload-file", $Pkg,
    "$ftpDir/$name"
)

Write-Host "verifica --list-only:"
$verifyFile = Join-Path $env:TEMP ("ps4-pkg-verify-{0}.txt" -f [guid]::NewGuid().ToString("n"))
try {
    Invoke-Curl @("--silent", "--show-error", "--list-only", "$ftpDir/", "-o", $verifyFile)
    $after = @(Get-Content -LiteralPath $verifyFile | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $after | ForEach-Object { Write-Host "  $_" }
    if ($after -notcontains $name) {
        throw "upload non trovato in $RemoteDir (manca $name)"
    }
} finally {
    Remove-Item -LiteralPath $verifyFile -ErrorAction SilentlyContinue
}

Write-Host "OK: $name in $RemoteDir sulla PS4 ($Ps4)."
Write-Host "Poi: Package Installer / auto-update da /data/pkg, oppure avvia PSBox se legge già quella cartella."
