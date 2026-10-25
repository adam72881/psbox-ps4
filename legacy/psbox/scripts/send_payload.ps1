# Invia psbox-install.bin al BinLoader di GoldHEN (TCP 9090).
# Il file è shellcode raw: non inizia con il magic ELF.
# Uso: powershell -ExecutionPolicy Bypass -File .\send_payload.ps1
param(
    [string]$Ps4 = "192.168.1.19",
    [int]$Port = 9090,
    [string]$File = ""
)
$ErrorActionPreference = "Stop"
if (-not $File) {
    $here = Split-Path -Parent $MyInvocation.MyCommand.Path
    $File = Join-Path $here "..\dist\psbox-install.bin"
}
$bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $File))
if ($bytes.Length -ge 4 -and $bytes[0] -eq 0x7F -and $bytes[1] -eq 0x45 -and $bytes[2] -eq 0x4C) {
    throw "questo file è un ELF. Invia psbox-install.bin (shellcode, senza header)"
}
$client = New-Object System.Net.Sockets.TcpClient
$client.Connect($Ps4, $Port)
$stream = $client.GetStream()
$stream.Write($bytes, 0, $bytes.Length)
$stream.Flush()
$client.Client.Shutdown([System.Net.Sockets.SocketShutdown]::Send)
Start-Sleep -Seconds 2
$client.Close()
Write-Host ("inviati {0} byte di shellcode a {1}:{2}" -f $bytes.Length, $Ps4, $Port)
