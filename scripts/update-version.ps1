# Update version information from VERSION file
param(
    [string]$VersionFile = "VERSION",
    [string]$HeaderFile = "blinky-things/core/Version.h"
)

try {
    # Read version from VERSION file
    $version = Get-Content $VersionFile -Raw | ForEach-Object { $_.Trim() }
    Write-Host "Reading version: $version"
    
    # Parse version components
    $parts = $version -split '\.'
    if ($parts.Length -ne 3) {
        throw "Invalid version format. Expected X.Y.Z"
    }
    
    $major = $parts[0]
    $minor = $parts[1]  
    $patch = $parts[2]
    
    Write-Host "Parsed as: Major=$major, Minor=$minor, Patch=$patch"
    
    # Read current header file
    $content = Get-Content $HeaderFile -Raw
    
    # Get git information if available
    try {
        $gitBranch = git rev-parse --abbrev-ref HEAD 2>$null
        $gitCommit = git rev-parse --short HEAD 2>$null
        if (-not $gitBranch) { $gitBranch = "unknown" }
        if (-not $gitCommit) { $gitCommit = "unknown" }
    } catch {
        $gitBranch = "unknown"
        $gitCommit = "unknown"
    }
    
    Write-Host "Git info: Branch=$gitBranch, Commit=$gitCommit"
    
    # Update version defines
    $content = $content -replace '#define BLINKY_VERSION_MAJOR \d+', "#define BLINKY_VERSION_MAJOR $major"
    $content = $content -replace '#define BLINKY_VERSION_MINOR \d+', "#define BLINKY_VERSION_MINOR $minor"
    $content = $content -replace '#define BLINKY_VERSION_PATCH \d+', "#define BLINKY_VERSION_PATCH $patch"
    $content = $content -replace '#define BLINKY_VERSION_STRING "[^"]*"', "#define BLINKY_VERSION_STRING `"$version`""
    $content = $content -replace '#define BLINKY_GIT_BRANCH "[^"]*"', "#define BLINKY_GIT_BRANCH `"$gitBranch`""
    $content = $content -replace '#define BLINKY_GIT_COMMIT "[^"]*"', "#define BLINKY_GIT_COMMIT `"$gitCommit`""
    
    # Write updated content
    Set-Content $HeaderFile $content -NoNewline
    
    Write-Host "Version updated to $version in $HeaderFile"
    
    # Update library.properties file (Arduino standard)
    $libraryPropsFile = "blinky-things/library.properties"
    if (Test-Path $libraryPropsFile) {
        $propsContent = Get-Content $libraryPropsFile -Raw
        $propsContent = $propsContent -replace 'version=[\d\.]+', "version=$version"
        Set-Content $libraryPropsFile $propsContent -NoNewline
        Write-Host "Updated library.properties version to $version"
    }
    
} catch {
    Write-Error "Failed to update version: $($_.Exception.Message)"
    exit 1
}