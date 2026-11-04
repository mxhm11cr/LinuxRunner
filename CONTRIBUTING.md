# Contributing to LinuxRunner

LinuxRunner derives from iSH through ios-linuxkit. Preserve the existing [license](LICENSE.md), [App Store terms](LICENSE.IOS), copyright notices and [third-party attribution](THIRD_PARTY_NOTICES.md).

## Report a Bug or Compatibility Result

Use [GitHub Issues](https://github.com/mxhm11cr/LinuxRunner/issues/new/choose). Include device model, iOS/iPadOS version, LinuxRunner version/build (Settings → About LinuxRunner), distribution, package version and desktop/graphics mode. Describe reproducible steps, expected behavior and the first relevant error.

Distinguish a successful launch, basic interaction and a longer feature test. Include diagnostic reports where useful; [Troubleshooting](docs/TROUBLESHOOTING.md#collect-logs-and-report-a-problem) explains the export controls. Review logs and screenshots for messages, account identifiers, addresses, tokens and private paths before sharing. Report vulnerabilities through the [private security channel](SECURITY.md), not public issues.

## Propose a Change

Read [Architecture](docs/ARCHITECTURE.md) and [Building](docs/BUILDING.md). Keep each pull request focused, preserve existing distributions and explain the behavior that changed and how it was verified. Use meaningful tests for runtime changes and state the actual test environment; do not turn a development test into a physical-device claim.

Follow the surrounding C, Objective-C, Swift and shell style. Do not commit signing credentials, provisioning profiles, private logs, personal data or unrelated binaries. License-check third-party code and retain its preferred source/build instructions and notices.

## Documentation and Screenshots

Check instructions against the current interface. The [screenshot handoff](assets/screenshots/README.md) lists missing captures and their intended placement. Use genuine app captures, accurate provenance and short software captions. Do not fabricate screenshots or claim universal device/application support.
