# PowerShell script to download free drum samples for blinky-test-player
# Uses publicly available CC0/public domain samples

Write-Host "Downloading free drum samples for blinky-test-player..." -ForegroundColor Cyan

# Create a temporary download directory
$tempDir = Join-Path $PSScriptRoot "temp_downloads"
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null

# Sample sources (CC0/Public Domain samples from archive.org and other sources)
# These are placeholder URLs - you'll need to replace with actual sample URLs

$samples = @{
    "kick" = @(
        # Add actual URLs to kick samples here
        # Example: "https://example.com/kick01.wav"
    )
    "snare" = @(
        # Add actual URLs to snare samples here
    )
    "hat" = @(
        # Add actual URLs to hi-hat samples here
    )
    "tom" = @(
        # Add actual URLs to tom samples here
    )
    "clap" = @(
        # Add actual URLs to clap samples here
    )
}

Write-Host @"

=============================================================================
IMPORTANT: Sample Download Script Template
=============================================================================

This script is a TEMPLATE. To use it, you need to:

1. Find free/CC0 drum samples from sources like:
   - Freesound.org (search for 'drum one shot')
   - Archive.org (public domain audio collections)
   - Your own recordings

2. Add the direct download URLs to the arrays above

3. Run this script to download and organize samples

Alternative: Manual Download
-----------------------------
The easiest approach is to manually download samples:

1. Visit https://freesound.org
2. Search for: "kick drum" (filter: duration 0-2s, license CC0)
3. Download 5-10 samples
4. Save to: $(Join-Path $PSScriptRoot "kick")

Repeat for: snare, hat, tom, clap

Then verify with:
  npm run dev -- samples

=============================================================================
"@ -ForegroundColor Yellow

# Cleanup
Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "`nPress any key to continue..." -ForegroundColor Gray
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
