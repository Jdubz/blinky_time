# Blinky Time - Branch Setup Script (PowerShell)
# This script initializes the staging and develop branches for the new workflow

param(
    [switch]$Force = $false
)

Write-Host "🌳 Setting up Blinky Time branch structure..." -ForegroundColor Green

# Check if we're in a git repository
try {
    git rev-parse --git-dir | Out-Null
} catch {
    Write-Host "❌ Not in a git repository. Please run this from the blinky_time directory." -ForegroundColor Red
    exit 1
}

# Check current branch
$currentBranch = git branch --show-current
if ($currentBranch -ne "master") {
    Write-Host "⚠️  Current branch is '$currentBranch'. Switching to master..." -ForegroundColor Yellow
    git checkout master
}

# Ensure master is up to date
Write-Host "📥 Updating master branch..." -ForegroundColor Blue
try {
    git pull origin master
} catch {
    Write-Host "⚠️  Could not pull from origin (maybe first time setup)" -ForegroundColor Yellow
}

# Function to check if branch exists
function Test-BranchExists {
    param($BranchName)
    try {
        git show-ref --verify --quiet "refs/heads/$BranchName"
        return $true
    } catch {
        return $false
    }
}

# Create staging branch if it doesn't exist
if (-not (Test-BranchExists "staging")) {
    Write-Host "🔧 Creating staging branch from master..." -ForegroundColor Blue
    git checkout -b staging
    git push -u origin staging
    Write-Host "✅ Staging branch created and pushed" -ForegroundColor Green
} else {
    Write-Host "✅ Staging branch already exists" -ForegroundColor Green
    git checkout staging
    try {
        git pull origin staging
    } catch {
        Write-Host "⚠️  Could not pull staging (maybe first time)" -ForegroundColor Yellow
    }
}

# Create develop branch if it doesn't exist
if (-not (Test-BranchExists "develop")) {
    Write-Host "🔧 Creating develop branch from staging..." -ForegroundColor Blue
    git checkout -b develop
    git push -u origin develop
    Write-Host "✅ Develop branch created and pushed" -ForegroundColor Green
} else {
    Write-Host "✅ Develop branch already exists" -ForegroundColor Green
    git checkout develop
    try {
        git pull origin develop
    } catch {
        Write-Host "⚠️  Could not pull develop (maybe first time)" -ForegroundColor Yellow
    }
}

# Return to master
git checkout master

Write-Host ""
Write-Host "🎉 Branch setup complete!" -ForegroundColor Green
Write-Host ""
Write-Host "📋 Available branches:" -ForegroundColor Cyan
$branches = git branch -a | Where-Object { $_ -match "(master|staging|develop)" }
foreach ($branch in $branches) {
    Write-Host "  $branch" -ForegroundColor White
}

Write-Host ""
Write-Host "🔄 Recommended workflow:" -ForegroundColor Cyan
Write-Host "  1. Feature work: checkout develop, create feature/xyz branches" -ForegroundColor White
Write-Host "  2. Integration: merge features to staging for testing" -ForegroundColor White
Write-Host "  3. Production: create PR from staging to master" -ForegroundColor White
Write-Host ""
Write-Host "📚 See .github/BRANCHING_STRATEGY.md for detailed workflow information" -ForegroundColor Cyan
Write-Host ""
Write-Host "⚡ To enable branch protection, see .github/BRANCH_PROTECTION.md" -ForegroundColor Cyan