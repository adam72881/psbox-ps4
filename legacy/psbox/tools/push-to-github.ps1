# Pusha questo progetto su https://github.com/adam72881/psbox-ps4
# Prerequisito: Git installato e login GitHub (gh auth login, oppure Git Credential Manager).
param(
    [string]$RepoUrl = "https://github.com/adam72881/psbox-ps4.git"
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $root
$existing = git remote | Where-Object { $_ -eq "github" }
if (-not $existing) {
    git remote add github $RepoUrl
} else {
    git remote set-url github $RepoUrl
}
git push -u github main
Write-Host "OK: https://github.com/adam72881/psbox-ps4"
