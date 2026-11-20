# SideLantern Wireless Display Interface Implementation Plan

> Use superpowers:executing-plans to implement the steps below.

**Goal** — Expose three versioned modes to paired iPad clients and route them to the chosen screen.

**Architecture** — Register discovery and launch handling on the authenticated GameStream HTTPS listener. Bind explicit launch tickets to a stable display device ID and resolve that same device on capture initialization and reinitialization. The secondary-only mode temporarily changes the display layout and restores it when the last owner of its launch ticket or stream releases the lease.

**Spec** — [Wireless display interface](../specs/2026-11-20-side-lantern-wireless-display.md)

- [x] Add HTTPS tests for explicit discovery, busy capture, and authentication using `tests/unit/test_nvhttp_client_auth.cpp`. Observe the failure before implementing the routes in `src/nvhttp.cpp`.
- [x] Add display selection and temporary topology restoration coverage in `tests/unit/test_display_device.cpp`. Implement the topology handling in `src/display_device.*`.
- [x] Carry the required display through `src/rtsp.*` and `src/video.*`. Require the selected device at capture initialization and prevent explicit requests from joining an already running capture.
- [x] Build the Windows host and run focused native and desktop tests. Verify a paired primary launch ticket and expiry, and run the ordinary two-client Moonlight video smoke test.
- [ ] On a machine with a configured virtual extended display, verify that the secondary-only mode restores the physical display and that extended mode keeps it active. Connect the iPad client to exercise both modes.
