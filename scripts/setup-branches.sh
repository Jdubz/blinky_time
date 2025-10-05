#!/bin/bash

# Blinky Time - Branch Setup Script
# This script initializes the staging and develop branches for the new workflow

set -e

echo "🌳 Setting up Blinky Time branch structure..."

# Check if we're in a git repository
if ! git rev-parse --git-dir > /dev/null 2>&1; then
    echo "❌ Not in a git repository. Please run this from the blinky_time directory."
    exit 1
fi

# Check if we're on master
current_branch=$(git branch --show-current)
if [ "$current_branch" != "master" ]; then
    echo "⚠️  Current branch is '$current_branch'. Switching to master..."
    git checkout master
fi

# Ensure master is up to date
echo "📥 Updating master branch..."
git pull origin master || echo "⚠️  Could not pull from origin (maybe first time setup)"

# Create staging branch if it doesn't exist
if ! git show-ref --verify --quiet refs/heads/staging; then
    echo "🔧 Creating staging branch from master..."
    git checkout -b staging
    git push -u origin staging
    echo "✅ Staging branch created and pushed"
else
    echo "✅ Staging branch already exists"
    git checkout staging
    git pull origin staging || echo "⚠️  Could not pull staging (maybe first time)"
fi

# Create develop branch if it doesn't exist  
if ! git show-ref --verify --quiet refs/heads/develop; then
    echo "🔧 Creating develop branch from staging..."
    git checkout -b develop
    git push -u origin develop
    echo "✅ Develop branch created and pushed"
else
    echo "✅ Develop branch already exists"
    git checkout develop
    git pull origin develop || echo "⚠️  Could not pull develop (maybe first time)"
fi

# Return to master
git checkout master

echo ""
echo "🎉 Branch setup complete!"
echo ""
echo "📋 Available branches:"
git branch -a | grep -E "(master|staging|develop)" | sed 's/^/  /'
echo ""
echo "🔄 Recommended workflow:"
echo "  1. Feature work: checkout develop, create feature/xyz branches"
echo "  2. Integration: merge features to staging for testing" 
echo "  3. Production: create PR from staging to master"
echo ""
echo "📚 See .github/BRANCHING_STRATEGY.md for detailed workflow information"
echo ""
echo "⚡ To enable branch protection, see .github/BRANCH_PROTECTION.md"