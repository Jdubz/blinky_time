# Copy drum samples from Ableton library to test player
$sourceDir = "E:\Ableton\Drums"
$destDir = ".\samples"

Write-Host "Copying drum samples from Ableton library..." -ForegroundColor Cyan

# Copy kicks (15 samples)
Write-Host "`nCopying kick samples..." -ForegroundColor Yellow
$kicks = Get-ChildItem -Path $sourceDir -Recurse -Filter "*kick*.wav" | Select-Object -First 15
$i = 1
foreach ($file in $kicks) {
    $newName = "kick_$("{0:D2}" -f $i).wav"
    Copy-Item $file.FullName -Destination "$destDir\kick\$newName"
    Write-Host "  $newName <- $($file.Directory.Name)"
    $i++
}

# Copy snares (15 samples)
Write-Host "`nCopying snare samples..." -ForegroundColor Yellow
$snares = Get-ChildItem -Path $sourceDir -Recurse -Filter "*snare*.wav" | Select-Object -First 15
$i = 1
foreach ($file in $snares) {
    $newName = "snare_$("{0:D2}" -f $i).wav"
    Copy-Item $file.FullName -Destination "$destDir\snare\$newName"
    Write-Host "  $newName <- $($file.Directory.Name)"
    $i++
}

# Copy hi-hats (15 samples)
Write-Host "`nCopying hi-hat samples..." -ForegroundColor Yellow
$hats = Get-ChildItem -Path $sourceDir -Recurse -Include "*hat*.wav","*hihat*.wav" | Select-Object -First 15
$i = 1
foreach ($file in $hats) {
    $newName = "hat_$("{0:D2}" -f $i).wav"
    Copy-Item $file.FullName -Destination "$destDir\hat\$newName"
    Write-Host "  $newName <- $($file.Directory.Name)"
    $i++
}

# Copy toms (10 samples)
Write-Host "`nCopying tom samples..." -ForegroundColor Yellow
$toms = Get-ChildItem -Path $sourceDir -Recurse -Filter "*tom*.wav" | Select-Object -First 10
$i = 1
foreach ($file in $toms) {
    $newName = "tom_$("{0:D2}" -f $i).wav"
    Copy-Item $file.FullName -Destination "$destDir\tom\$newName"
    Write-Host "  $newName <- $($file.Directory.Name)"
    $i++
}

# Copy claps (10 samples)
Write-Host "`nCopying clap samples..." -ForegroundColor Yellow
$claps = Get-ChildItem -Path $sourceDir -Recurse -Filter "*clap*.wav" | Select-Object -First 10
$i = 1
foreach ($file in $claps) {
    $newName = "clap_$("{0:D2}" -f $i).wav"
    Copy-Item $file.FullName -Destination "$destDir\clap\$newName"
    Write-Host "  $newName <- $($file.Directory.Name)"
    $i++
}

Write-Host "`nDone! Sample summary:" -ForegroundColor Green
Write-Host "  Kicks:  $((Get-ChildItem "$destDir\kick\*.wav").Count) samples"
Write-Host "  Snares: $((Get-ChildItem "$destDir\snare\*.wav").Count) samples"
Write-Host "  Hats:   $((Get-ChildItem "$destDir\hat\*.wav").Count) samples"
Write-Host "  Toms:   $((Get-ChildItem "$destDir\tom\*.wav").Count) samples"
Write-Host "  Claps:  $((Get-ChildItem "$destDir\clap\*.wav").Count) samples"

Write-Host "`nVerify samples:" -ForegroundColor Cyan
Write-Host "  npm run dev -- samples"
