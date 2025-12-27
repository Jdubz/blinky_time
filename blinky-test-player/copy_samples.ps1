# Copy drum samples from Ableton library to test player
#
# Usage:
#   .\copy_samples.ps1                          # Prompts for source directory
#   .\copy_samples.ps1 "E:\Ableton\Drums"      # Uses provided path
#   $env:ABLETON_DRUMS_DIR="E:\Ableton\Drums"; .\copy_samples.ps1

param(
    [string]$SourcePath
)

$destDir = ".\samples"

# Determine source directory (priority: param > env var > prompt)
if ($SourcePath) {
    $sourceDir = $SourcePath
} elseif ($env:ABLETON_DRUMS_DIR) {
    $sourceDir = $env:ABLETON_DRUMS_DIR
    Write-Host "Using ABLETON_DRUMS_DIR: $sourceDir" -ForegroundColor Cyan
} else {
    $sourceDir = Read-Host "Enter the path to your Ableton drum library (e.g., E:\Ableton\Drums)"
}

# Validate source directory
if (-not (Test-Path -LiteralPath $sourceDir)) {
    Write-Host "Error: Source directory '$sourceDir' does not exist!" -ForegroundColor Red
    Write-Host "Please provide a valid path to your drum sample library." -ForegroundColor Yellow
    exit 1
}

Write-Host "`nCopying drum samples from: $sourceDir" -ForegroundColor Cyan

# Reusable function to copy drum samples
function Copy-DrumSamples {
    param(
        [string]$Instrument,
        [string[]]$Filters,
        [int]$Count
    )

    Write-Host "`nCopying $Instrument samples..." -ForegroundColor Yellow

    # Ensure destination directory exists
    $instrumentDestDir = Join-Path $destDir $Instrument
    if (-not (Test-Path $instrumentDestDir)) {
        New-Item -ItemType Directory -Path $instrumentDestDir | Out-Null
    }

    # Find and copy samples
    $files = Get-ChildItem -Path $sourceDir -Recurse -Include $Filters | Select-Object -First $Count

    if ($files.Count -eq 0) {
        Write-Host "  Warning: No files found matching $Filters" -ForegroundColor Yellow
        return
    }

    $i = 1
    foreach ($file in $files) {
        $newName = "${Instrument}_$('{0:D2}' -f $i).wav"
        Copy-Item $file.FullName -Destination (Join-Path $instrumentDestDir $newName)
        Write-Host "  $newName <- $($file.Directory.Name)"
        $i++
    }
}

# Copy samples for each instrument type
Copy-DrumSamples -Instrument "kick" -Filters @("*kick*.wav") -Count 15
Copy-DrumSamples -Instrument "snare" -Filters @("*snare*.wav") -Count 15
Copy-DrumSamples -Instrument "hat" -Filters @("*hat*.wav", "*hihat*.wav") -Count 15
Copy-DrumSamples -Instrument "tom" -Filters @("*tom*.wav") -Count 10
Copy-DrumSamples -Instrument "clap" -Filters @("*clap*.wav") -Count 10

# Summary
Write-Host "`nDone! Sample summary:" -ForegroundColor Green
Write-Host "  Kicks:  $((Get-ChildItem "$destDir\kick\*.wav" -ErrorAction SilentlyContinue).Count) samples"
Write-Host "  Snares: $((Get-ChildItem "$destDir\snare\*.wav" -ErrorAction SilentlyContinue).Count) samples"
Write-Host "  Hats:   $((Get-ChildItem "$destDir\hat\*.wav" -ErrorAction SilentlyContinue).Count) samples"
Write-Host "  Toms:   $((Get-ChildItem "$destDir\tom\*.wav" -ErrorAction SilentlyContinue).Count) samples"
Write-Host "  Claps:  $((Get-ChildItem "$destDir\clap\*.wav" -ErrorAction SilentlyContinue).Count) samples"

Write-Host "`nVerify samples:" -ForegroundColor Cyan
Write-Host "  npm run dev -- samples"
