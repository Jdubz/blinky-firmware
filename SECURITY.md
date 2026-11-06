# Security Policy

## Supported Versions

Only the latest **stable** firmware release is supported: the GitHub Release marked *Latest* (versioned `bN`). Candidates (pre-releases) and dev builds (`bN-dev-<sha>`) are not. See [docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md).

## Reporting a Vulnerability

If you discover a security vulnerability in the firmware, please report it responsibly:

1. **Do not** open a public GitHub issue for security vulnerabilities
2. Contact the maintainer directly (via the GitHub profile of the repository owner)
3. Include:
   - Description of the vulnerability
   - Steps to reproduce
   - Potential impact
   - Suggested fix (if any)

Vulnerabilities in the fleet server, the web console or the test box go to [blinky_time](https://github.com/Jdubz/blinky_time/blob/master/SECURITY.md) the same way.

## Security Considerations

### Device Interfaces

This is an embedded LED controller (XIAO nRF52840 Sense). Ways to reach a device:

- **USB serial**: the full command console (physical access required).
- **Bluetooth**: the firmware advertises a Nordic UART service (NUS) that carries the same console, and a BLE DFU service. Neither uses pairing, so any Bluetooth device in range can connect. It also listens for fleet broadcast commands from blinky-server.
- **No WiFi** on the nRF52840 fleet.

### Firmware Updates

- **Signed OTA:** `ota manifest` / `ota chunk` / `ota commit` accept only an image whose manifest carries a valid Ed25519 signature from the release key (public half in `blinky-things/ota/OtaKeys.h`) and is newer than the running build. This is the path the web updater uses ([docs/WEB_FLASHER.md](docs/WEB_FLASHER.md)).
- **Release signing key:** held only in this repo's GitHub environment `release-signing` (master branch only). No offline backup has been verified. Only gated stable releases are signed. Never sign a dev or test build with it; use the bench key (see CLAUDE.md).
- **What still bypasses signing** (see [docs/WEB_FLASHER.md](docs/WEB_FLASHER.md#what-still-bypasses-signing)):
  - UF2 drag-and-drop and SWD, which need physical access. That is accepted: whoever holds the device can reflash it.
  - `bootloader ble` over NUS, then legacy BLE DFU with a native tool. Sealed-device recovery depends on it. Closing it needs authenticated console commands (an open item).

### Code Security

- Input validation on serial commands to prevent buffer overflows
- Invalid state fails loudly instead of falling back silently (see CLAUDE.md)

### Archive Notice

The `archive/` directory contains legacy sketches from before `blinky-things` that may include:
- Hardcoded credentials (for development/testing purposes only)
- Deprecated network code

They are **not part of the active firmware** and are kept for historical reference only. Do not use archive code in production.

## Best Practices for Contributors

1. Never commit real credentials, API keys, signing keys, or secrets
2. Use placeholder values in example configurations
3. Keep sensitive test data in `.gitignore`d files
4. Review code for potential buffer overflows before submitting PRs
