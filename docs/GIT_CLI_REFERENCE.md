# Git & GitHub CLI Command Reference

## Standard Git Commands (Instead of GitKraken)

### Status & Information
```bash
git status                          # Check working tree status
git log --oneline -10              # Show recent commits (one line each)
git log --graph --oneline --all    # Show branch history with graph
git branch -vv                     # Show branches with remote tracking
git remote -v                      # Show remote repositories
```

### Staging & Committing
```bash
git add .                          # Stage all changes
git add filename.txt               # Stage specific file
git commit -m "commit message"     # Commit with message
git commit -am "message"           # Stage and commit (tracked files only)
```

### Branching
```bash
git branch                         # List local branches
git branch -a                      # List all branches (local + remote)
git branch new-branch              # Create new branch
git checkout branch-name           # Switch to branch
git checkout -b new-branch         # Create and switch to new branch
git merge branch-name              # Merge branch into current
git branch -d branch-name          # Delete local branch
```

### Remote Operations
```bash
git fetch                          # Fetch latest from remote
git pull                           # Fetch and merge from remote
git push                           # Push current branch
git push origin branch-name        # Push specific branch
git push -u origin branch-name     # Push and set upstream
```

### Stashing
```bash
git stash                          # Stash current changes
git stash list                     # List stashes
git stash pop                      # Apply and remove latest stash
git stash apply                    # Apply latest stash (keep in list)
git stash drop                     # Delete latest stash
```

### Viewing Changes
```bash
git diff                           # Show unstaged changes
git diff --staged                  # Show staged changes
git diff HEAD~1                    # Compare with previous commit
git show HEAD                      # Show latest commit details
git blame filename.txt             # Show who changed each line
```

## GitHub CLI Commands

### Repository Information
```bash
gh repo view                       # View repository info
gh repo view --web                 # Open repository in browser
gh repo clone owner/repo           # Clone repository
gh repo fork owner/repo            # Fork repository
```

### Issues
```bash
gh issue list                      # List issues
gh issue view 123                  # View issue #123
gh issue create                    # Create new issue
gh issue close 123                 # Close issue #123
gh issue reopen 123                # Reopen issue #123
```

### Pull Requests
```bash
gh pr list                         # List pull requests
gh pr view 123                     # View PR #123
gh pr create                       # Create new PR
gh pr merge 123                    # Merge PR #123
gh pr close 123                    # Close PR #123
gh pr review 123                   # Review PR #123
```

### Releases
```bash
gh release list                    # List releases
gh release view v1.0.0             # View specific release
gh release create v1.0.1           # Create new release
gh release upload v1.0.1 file.zip  # Upload asset to release
```

### Authentication & Configuration
```bash
gh auth login                      # Login to GitHub
gh auth logout                     # Logout from GitHub
gh auth status                     # Check auth status
gh config set git_protocol https   # Set git protocol
```

## Common Workflows

### Standard Development Workflow
```bash
# Check status
git status

# Stage and commit changes
git add .
git commit -m "feat: add new feature"

# Push to remote
git push

# Or push new branch with upstream
git push -u origin feature-branch
```

### Create Pull Request
```bash
# Push branch
git push -u origin feature-branch

# Create PR using GitHub CLI
gh pr create --title "Add new feature" --body "Description of changes"

# Or open PR in browser
gh pr create --web
```

### Release Workflow
```bash
# Update version
echo "1.1.0" > VERSION
make version

# Commit version changes
git add .
git commit -m "chore: bump version to 1.1.0"

# Create tag
git tag v1.1.0
git push origin v1.1.0

# Create GitHub release
gh release create v1.1.0 --title "Version 1.1.0" --notes "Release notes"
```

### Working with Remotes
```bash
# Add remote
git remote add upstream https://github.com/original/repo.git

# Fetch from upstream
git fetch upstream

# Merge upstream changes
git merge upstream/master
```

## Your Current Setup

- **Repository**: Jdubz/blinky_time
- **Current Branch**: staging
- **Default Branch**: master
- **Last Push**: 2025-09-25T16:33:01Z (just now!)

## Quick Commands for Your Project

```bash
# Check build system
make help

# Update version and build
echo "1.0.2" > VERSION
make upload DEVICE=2

# Standard git workflow
git add .
git commit -m "fix: bug fix description"
git push

# Create PR to merge staging -> master
gh pr create --base master --head staging --title "Arduino Versioning System" --body "Implement comprehensive Arduino versioning"
```

## Environment Setup

✅ Git: Already configured  
✅ GitHub CLI: Installed and authenticated as Jdubz  
✅ Arduino CLI: Installed and working  
✅ Make: Installed and working  

You're all set for standard command-line git workflows! 🚀