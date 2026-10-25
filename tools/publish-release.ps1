# Pubblica dist/PSBox-PS4-01.38.pkg su GitHub Releases (serve: gh auth login)
param(
  [string]$Tag = "v1.0.38",
  [string]$Pkg = ""
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $Pkg) { $Pkg = Join-Path $root "dist\PSBox-PS4-01.38.pkg" }
if (-not (Test-Path $Pkg)) { throw "Pkg non trovato: $Pkg" }
$notes = Join-Path $root "docs\RELEASE.md"
gh release create $Tag $Pkg --repo adam72881/psbox-ps4 --title "PSBox PS4 01.38" --notes-file $notes
Write-Host "OK: https://github.com/adam72881/psbox-ps4/releases/tag/$Tag"
