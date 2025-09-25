# Branch Protection Configuration

This document outlines the recommended branch protection settings for the blinky_time repository.

## Master Branch Protection

**Settings to enable in GitHub repository settings:**

- ✅ **Require a pull request before merging**
  - Require approvals: 1
  - Dismiss stale PR approvals when new commits are pushed
  - Require review from code owners (if CODEOWNERS file exists)

- ✅ **Require status checks to pass before merging**
  - Require branches to be up to date before merging
  - Required status checks:
    - `test-suite (1)` - Hat configuration tests
    - `test-suite (2)` - Tube Light configuration tests  
    - `test-suite (3)` - Bucket Totem configuration tests
    - `quality-checks` - Code quality and documentation

- ✅ **Require conversation resolution before merging**

- ✅ **Require signed commits** (optional but recommended)

- ✅ **Require linear history** (optional, prevents merge commits)

- ✅ **Include administrators** (applies rules to admins too)

- ✅ **Restrict pushes that create files** (optional)

## Staging Branch Protection

**Settings to enable:**

- ✅ **Require a pull request before merging**
  - Require approvals: 1 (can be less strict than master)

- ✅ **Require status checks to pass before merging**
  - Required status checks:
    - `test-suite (1)` - Hat configuration tests
    - `test-suite (2)` - Tube Light configuration tests
    - `test-suite (3)` - Bucket Totem configuration tests

- ⚠️ **Allow force pushes** (for rebasing and cleanup)

## Repository Settings

### General Settings
- **Default branch**: `develop`
- **Merge button**: Allow merge commits, squash merging, and rebase merging
- **Automatically delete head branches**: ✅ Enabled

### Security Settings
- **Dependency vulnerability alerts**: ✅ Enabled  
- **Token scanning alerts**: ✅ Enabled
- **Secret scanning**: ✅ Enabled (if available)

### Actions Settings
- **Allow all actions and reusable workflows**: ✅ Enabled
- **Require approval for all outside collaborators**: ✅ Enabled

## Setup Commands

To configure these settings via GitHub CLI:

```bash
# Enable branch protection for master
gh api repos/Jdubz/blinky_time/branches/master/protection \
  --method PUT \
  --field required_status_checks='{"strict":true,"contexts":["test-suite (1)","test-suite (2)","test-suite (3)","quality-checks"]}' \
  --field enforce_admins=true \
  --field required_pull_request_reviews='{"required_approving_review_count":1,"dismiss_stale_reviews":true}' \
  --field restrictions=null

# Enable branch protection for staging  
gh api repos/Jdubz/blinky_time/branches/staging/protection \
  --method PUT \
  --field required_status_checks='{"strict":true,"contexts":["test-suite (1)","test-suite (2)","test-suite (3)"]}' \
  --field enforce_admins=false \
  --field required_pull_request_reviews='{"required_approving_review_count":1}' \
  --field restrictions=null \
  --field allow_force_pushes=true
```

## Recommended Workflow

1. **Feature Development**: Work in `feature/` branches from `develop`
2. **Integration Testing**: Merge features to `staging` for testing
3. **Production Release**: Create PR from `staging` to `master`
4. **Emergency Fixes**: Hotfix branches can merge directly to `master`

## Status Check Requirements

The CI/CD pipeline provides these status checks:

- **test-suite (1)**: Hat configuration compilation and testing
- **test-suite (2)**: Tube Light configuration compilation and testing  
- **test-suite (3)**: Bucket Totem configuration compilation and testing
- **quality-checks**: Code formatting, documentation, static analysis
- **pr-validation**: PR branch validation (master PRs must come from staging)

## Monitoring

Track these metrics to ensure the workflow is effective:

- 📊 **PR merge time**: Time from PR creation to merge
- 🔄 **Build success rate**: Percentage of successful CI builds
- 🐛 **Bug escape rate**: Issues found in production vs staging
- 📈 **Release frequency**: How often releases are deployed
- ⏱️ **Time to fix**: Time from bug report to fix deployment

This protection strategy ensures code quality while maintaining development velocity.