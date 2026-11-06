# Security Policy

This repo is the tooling: blinky-server (the fleet server), blinky-console (the web console and browser updater), ml-training and the test box. The firmware's policy, covering supported releases, device interfaces, signed OTA and the release signing key, is [blinky-firmware's SECURITY.md](https://github.com/Jdubz/blinky-firmware/blob/master/SECURITY.md).

## Reporting a Vulnerability

If you discover a security vulnerability in Blinky Time, please report it responsibly:

1. **Do not** open a public GitHub issue for security vulnerabilities
2. Contact the maintainer directly (via the GitHub profile of the repository owner)
3. Include:
   - Description of the vulnerability
   - Steps to reproduce
   - Potential impact
   - Suggested fix (if any)

## Security Considerations

### Fleet Server

blinky-server's flash and deploy endpoints need an API key (`BLINKY_API_KEY` or `~/.blinky-api-key`). Device-mutating requests also need the `X-Deploy-Tool` header that only `scripts/deploy.sh` sets. See [docs/SAFETY.md](docs/SAFETY.md).

### Browser Updater

The console's `#firmware` page installs only stable releases, through the firmware's signed OTA path; the device checks the signature, so the page holds no key. Design: blinky-firmware's [docs/WEB_FLASHER.md](https://github.com/Jdubz/blinky-firmware/blob/master/docs/WEB_FLASHER.md).

### Secrets

CI secrets (the review token, the Firebase service account) are repository secrets here. The release signing key is not in this repo; it lives only in blinky-firmware's `release-signing` environment.

## Best Practices for Contributors

1. Never commit real credentials, API keys, signing keys, or secrets
2. Use placeholder values in example configurations
3. Keep sensitive test data in `.gitignore`d files
