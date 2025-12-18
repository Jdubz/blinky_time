#
# Install git hooks for blinky_time project (Windows PowerShell)
#
# Usage: .\scripts\install-hooks.ps1
#

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$HooksSrc = Join-Path $RepoRoot "hooks"
$HooksDst = Join-Path $RepoRoot ".git\hooks"

Write-Host "Installing git hooks..." -ForegroundColor Cyan

# Check if we're in a git repository
if (-not (Test-Path (Join-Path $RepoRoot ".git"))) {
    Write-Host "Error: Not a git repository" -ForegroundColor Red
    exit 1
}

# Create hooks directory if it doesn't exist
if (-not (Test-Path $HooksDst)) {
    New-Item -ItemType Directory -Path $HooksDst | Out-Null
}

# Install each hook
$hooks = Get-ChildItem -Path $HooksSrc -File
foreach ($hook in $hooks) {
    Write-Host "  Installing $($hook.Name)..." -ForegroundColor White
    Copy-Item -Path $hook.FullName -Destination (Join-Path $HooksDst $hook.Name) -Force
}

Write-Host ""
Write-Host "Git hooks installed successfully!" -ForegroundColor Green
Write-Host ""
Write-Host "Installed hooks:"
Get-ChildItem -Path $HooksDst | Where-Object { $_.Name -notmatch "\.sample$" } | ForEach-Object { Write-Host "  - $($_.Name)" }

Write-Host ""
Write-Host "To skip hooks temporarily, use: git push --no-verify" -ForegroundColor Yellow
