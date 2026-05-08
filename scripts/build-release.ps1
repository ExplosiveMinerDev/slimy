# Ferme les exe SlimyJourney qui verrouillent le linker (LNK1104), puis compile en Release.
# Usage (depuis n'importe où) :
#   powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1
# Ou double-clic : build-release.bat à la racine du repo.

$ErrorActionPreference = 'SilentlyContinue'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot 'build'

foreach ($name in @('slimyjourney', 'slimyjourney_server', 'slimyjourney_mapedit')) {
    Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 800

if (-not (Test-Path $buildDir)) {
    Write-Host "Dossier build introuvable. Lance d'abord cmake depuis la racine :" -ForegroundColor Yellow
    Write-Host "  cmake -S . -B build" -ForegroundColor Yellow
    exit 1
}

Push-Location $buildDir
try {
    cmake --build . --config Release
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
