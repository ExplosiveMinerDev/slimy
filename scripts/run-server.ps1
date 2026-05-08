# 24/7 launcher for slimyjourney_server.exe. Restarts the server on:
#   - exit code 100 (server announced an update and asked to be restarted)
#   - any non-zero exit code (crash recovery), bounded by a backoff
# Stops cleanly on Ctrl+C.
#
# If a remote manifest URL is provided, the launcher fetches it before each
# launch and writes the result to a local manifest file the server polls.
#
# Example:
#   powershell -ExecutionPolicy Bypass -File run-server.ps1 `
#     -Exe "..\build\Release\slimyjourney_server.exe" `
#     -Port 6543 `
#     -LocalManifest "server-manifest.txt" `
#     -RemoteManifest "https://example.com/slimyjourney/manifest.txt"
#
# Manifest format (one entry per line, optional):
#   build=11
#   url=https://example.com/release

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]  [string] $Exe,
    [Parameter(Mandatory = $false)] [int]    $Port = 6543,
    [Parameter(Mandatory = $false)] [string] $LocalManifest = 'server-manifest.txt',
    [Parameter(Mandatory = $false)] [string] $RemoteManifest = '',
    [Parameter(Mandatory = $false)] [int]    $RemoteFetchEverySeconds = 300
)

$ErrorActionPreference = 'Continue'

if (-not (Test-Path $Exe)) {
    Write-Error "Server exe not found: $Exe"
    exit 1
}

$lastFetch = [DateTime]::MinValue

function Update-LocalManifest {
    if ([string]::IsNullOrEmpty($RemoteManifest)) { return }
    $now = Get-Date
    $delta = ($now - $lastFetch).TotalSeconds
    if ($delta -lt $RemoteFetchEverySeconds) { return }
    try {
        Invoke-WebRequest -Uri $RemoteManifest -OutFile $LocalManifest -UseBasicParsing -TimeoutSec 15
        Set-Variable -Name lastFetch -Value $now -Scope 1
        Write-Host ("[launcher] fetched manifest from {0}" -f $RemoteManifest)
    } catch {
        Write-Warning "[launcher] manifest fetch failed: $($_.Exception.Message)"
    }
}

$crashCount = 0
while ($true) {
    Update-LocalManifest

    $args = @('--port', "$Port", '--manifest', "$LocalManifest", '--auto-restart')
    Write-Host ("[launcher] launching {0} {1}" -f $Exe, ($args -join ' '))
    $proc = Start-Process -FilePath $Exe -ArgumentList $args -NoNewWindow -PassThru -Wait
    $code = $proc.ExitCode

    if ($code -eq 100) {
        Write-Host '[launcher] server requested restart for update'
        $crashCount = 0
        # Pull the latest manifest immediately, in case ops just bumped it.
        Set-Variable -Name lastFetch -Value ([DateTime]::MinValue) -Scope 0
        Update-LocalManifest
        Start-Sleep -Seconds 2
        continue
    }
    if ($code -eq 0) {
        Write-Host '[launcher] server exited cleanly (code 0). bye.'
        break
    }
    $crashCount++
    $sleep = [Math]::Min(60, 5 * $crashCount)
    Write-Warning "[launcher] server exited with code $code — restart in $sleep s (crash #$crashCount)"
    Start-Sleep -Seconds $sleep
}
