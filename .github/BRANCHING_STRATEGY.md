# Branching Strategy and Workflow

## 🌳 Branch Structure

### `master` - Production Branch
- **Purpose**: Stable, production-ready code
- **Protection**: Requires PR from `staging` branch
- **Releases**: Automatic semantic versioning (e.g., v1.2.3)
- **CI/CD**: Full test suite + production release creation

### `staging` - Integration Branch  
- **Purpose**: Integration testing and pre-release validation
- **Protection**: Requires PR review
- **Versioning**: Automatic staging versions (e.g., 2025.01.15-staging.42+abc123)
- **CI/CD**: Full test suite + pre-release creation

### `develop` - Development Branch
- **Purpose**: Active development and feature integration
- **Protection**: Basic compilation checks
- **CI/CD**: Compilation tests only

### Feature Branches
- **Naming**: `feature/description` or `fix/description`
- **Merge to**: `develop` or `staging`
- **Lifecycle**: Delete after merge

## 🔄 Workflow Process

### 1. Feature Development
```bash
# Create feature branch from develop
git checkout develop
git pull origin develop
git checkout -b feature/new-effect

# Work on feature
git add .
git commit -m "Add new visual effect"
git push origin feature/new-effect

# Create PR to develop
# PR gets basic compilation checks
```

### 2. Integration Testing
```bash
# Merge feature to staging for testing
git checkout staging
git pull origin staging
git merge feature/new-effect
git push origin staging

# Automatic versioning triggers
# Creates staging release: v2025.01.15-staging.42+abc123
```

### 3. Production Release
```bash
# When staging is stable, create PR to master
git checkout master
git pull origin master

# Create PR from staging to master
# Full test suite runs
# Manual review required
# After merge: automatic production release v1.2.3
```

## 🤖 Automated Versioning

### Staging Versions
Format: `YYYY.MM.DD-staging.COMMIT_COUNT+SHORT_SHA`

Example: `2025.01.15-staging.42+abc123`
- `2025.01.15`: Date of commit
- `staging`: Branch identifier  
- `42`: Commit count on staging
- `abc123`: Short commit SHA

### Production Versions
Format: `MAJOR.MINOR.PATCH` (Semantic Versioning)

Example: `1.2.3`
- Auto-increments patch version on each master merge
- Manual version bumps for major/minor changes

## 🧪 Testing Strategy

### On Every Push to Any Branch
- ✅ Compilation for all device types
- ✅ Code quality checks
- ✅ Basic static analysis

### On Push to `staging`
- ✅ Full test suite
- ✅ Memory usage validation  
- ✅ Effect test framework validation
- ✅ Documentation checks
- 🏷️ Automatic versioning and pre-release

### On Push to `master`
- ✅ Complete integration tests
- ✅ All device configuration validation
- ✅ Release note generation
- 🚀 Production release creation

### On Pull Requests
- ✅ Branch validation (master PRs must come from staging)
- ✅ Automated PR comments with status
- ✅ Review requirement enforcement

## 📋 PR Guidelines

### To `develop`
- Basic feature PRs
- Bug fixes
- Documentation updates
- **Review**: Optional but recommended
- **Tests**: Compilation only

### To `staging`  
- Integration of completed features
- Pre-release preparations
- **Review**: Required
- **Tests**: Full test suite

### To `master`
- **Source**: Must be from `staging` branch only
- **Review**: Required (maintainer approval)
- **Tests**: Complete validation suite
- **Result**: Automatic production release

## 🔧 Local Development Setup

```bash
# Clone and setup
git clone https://github.com/Jdubz/blinky_time.git
cd blinky_time

# Setup all branches
git checkout develop
git checkout staging  
git checkout master

# Start feature work
git checkout develop
git checkout -b feature/my-new-feature

# When ready for integration testing
git checkout staging
git merge feature/my-new-feature
git push origin staging

# Monitor staging release creation
# Test staging release on hardware
# When validated, create PR to master
```

## 🏷️ Version Management

### Staging Release Example
```
Version: 2025.01.15-staging.42+abc123
Tag: v2025.01.15-staging.42+abc123
Release: Pre-release ✓
Description: Staging release for testing
```

### Production Release Example  
```
Version: 1.2.3
Tag: v1.2.3
Release: Latest release ✓
Description: Stable production release
```

## 🚨 Emergency Hotfixes

For critical production issues:

```bash
# Create hotfix branch from master
git checkout master
git checkout -b hotfix/critical-fix

# Fix issue
git commit -m "Fix critical issue"

# Merge directly to master (skip staging)
git checkout master
git merge hotfix/critical-fix
git push origin master

# Backport to staging
git checkout staging  
git merge hotfix/critical-fix
git push origin staging
```

## 📊 Release Metrics

The CI/CD system tracks:
- ✅ Compilation success rate
- 📦 Binary size trends
- 🧪 Test coverage
- ⏱️ Build time
- 🔄 Release frequency
- 🐛 Issue resolution time

This workflow ensures high code quality, comprehensive testing, and reliable releases while maintaining development velocity.