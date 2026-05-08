# Démarre le serveur pour jouer avec des amis (UDP). Ferme l'ancien serveur sur ce port.
# Usage :
#   powershell -ExecutionPolicy Bypass -File scripts\start-public-server.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\start-public-server.ps1 -Port 6544
# Double-clic : start-public-server.bat

param(
    [int] $Port = 6543
)

$ErrorActionPreference = 'SilentlyContinue'
$repoRoot = Split-Path -Parent $PSScriptRoot

$exe = $null
foreach ($rel in @('build\Release\slimyjourney_server.exe', 'build\Debug\slimyjourney_server.exe')) {
    $p = Join-Path $repoRoot $rel
    if (Test-Path $p) { $exe = $p; break }
}
if (-not $exe) {
    Write-Host "slimyjourney_server.exe introuvable. Compile d'abord :" -ForegroundColor Red
    Write-Host "  .\build-release.bat" -ForegroundColor Yellow
    exit 1
}

Write-Host "[launcher] Arrêt de l'ancien serveur (si actif)..." -ForegroundColor Cyan
Get-Process -Name 'slimyjourney_server' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 600

$busy = netstat -ano | Select-String -Pattern ":$Port\s.*UDP"
if ($busy) {
    Write-Host "[launcher] Attention : quelque chose utilise déjà UDP $Port :" -ForegroundColor Yellow
    $busy | Select-Object -First 5 | ForEach-Object { Write-Host "  $_" }
    Write-Host "  Ferme ce programme ou relance avec : -Port 6544" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "=== SlimyJourney — serveur public (self-hosted) ===" -ForegroundColor Green
$pub = $null
try {
    $pub = (Invoke-RestMethod -Uri 'https://api.ipify.org' -TimeoutSec 4).ToString().Trim()
} catch { }
if ($pub) {
    Write-Host "IP publique (Internet) : $pub" -ForegroundColor White
    Write-Host "Commande client (copier-coller) :" -ForegroundColor White
    Write-Host "  slimyjourney.exe --client $pub $Port" -ForegroundColor Cyan
} else {
    Write-Host "IP publique : (non récupérée — vérifie ta connexion ou utilise ipconfig pour le LAN)" -ForegroundColor Yellow
}
Write-Host ""
Write-Host "Pare-feu Windows (à faire UNE fois en admin si les amis ne joignent pas) :" -ForegroundColor White
Write-Host "  netsh advfirewall firewall add rule name=`"SlimyJourney UDP $Port`" dir=in action=allow protocol=UDP localport=$Port" -ForegroundColor DarkGray
Write-Host ""
Write-Host "Routeur : redirige UDP $Port vers ce PC." -ForegroundColor White
Write-Host "Démarrage du serveur (Ctrl+C pour arrêter)..." -ForegroundColor Green
Write-Host ""

& $exe --port $Port
