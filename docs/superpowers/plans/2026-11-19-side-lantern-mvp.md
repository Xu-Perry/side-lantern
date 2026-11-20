# SideLantern v0.1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Build the Windows host, then validate that a clean installation supports a complete Moonlight streaming workflow.

**Architecture:** A small Electron UI manages a private instance of the Sunshine native host through authenticated loopback HTTPS. An isolated development runner exercises the core path before the signed Windows installer and native Windows service are introduced.

**Tech Stack:** Electron, Node.js, Windows PowerShell, Sunshine C++, CMake/MSYS2 and an IddCx-based display driver.

**Spec:** [Windows host design](../specs/2026-11-19-side-lantern-mvp-design.md)

## Global Constraints

- Windows 11 x64 is the first verification target. The installed Sunshine service and its installation and data remain intact during development.
- v0.1 supports original Moonlight PIN pairing, one shared source, multiple streaming clients, and the bundled Desktop and Steam Big Picture applications.
- Closing the window preserves streaming. Explicit exit, renderer-independent shutdown, and main-process termination must stop a SideLantern-owned host.
- Installer changes and Windows service registration are tested separately from the development runner. Detect installed virtual input/display components before changing them.

## Review Focus

- A port collision must never make the app attach to, authenticate against, or stop an unrelated host.
- TLS validation must reject a mismatched certificate before sending the management credentials.
- Saving supported settings must preserve existing Sunshine keys outside the UI's scope.
- Multiple pending pairing requests must be associated with their request IDs and keep individual actions isolated.
- Crash recovery must only stop the SideLantern process tree. A missing or incomplete optional driver must leave primary-screen streaming available.

## Development Slice

### Task 1. Sunshine management API

- [x] Test config mapping, unknown-key preservation, PIN request identity and certificate pin comparison with `node --test desktop/test/*.test.cjs`.
- [x] Implement `createSunshineClient({port, username, password, certificate})` in `desktop/lib/sunshine-api.cjs` with read config, save config, list devices, list requests, PIN approval/cancel, and unpair.
- [x] Run the management tests without starting or mutating the installed Sunshine.

### Task 2. Desktop renderer and IPC

- [x] Validate the contract between the bundled renderer, preload and main process.
- [x] Implement native window, tray, theme preferences and authenticated IPC. Build the host, streaming settings, display and app settings screens in `desktop/renderer/`.
- [x] Test state transitions, renderer settings and both PIN flows through the host/API contract. Display unreleased native capabilities as unavailable.

### Task 3. Private development host

- [x] Test exclusive ownership, repeat start and stop, failed startup, and configuration staging using Node's built-in test runner.
- [x] Implement a Windows runner in `desktop/lib/` and an isolated profile that uses a private alternate port range. Use an existing Sunshine binary and assets as inputs, without modifying the source installation or pointing the private instance at the existing profile.
- [x] Run the Electron and private-engine checks in `desktop/test/windows-smoke.ps1`, then check both the process teardown and original service state.

## Native Release Slice

### Task 4. Service, streaming state and virtual display

- [x] Build a separate on-demand Windows service using a protected installation directory. Run its command-line, named-pipe and job-termination self-tests.
- [ ] Verify the installed Windows service as a standard Windows user, including Electron process loss and Windows shutdown.
- [x] Add a read-only active-session management contract in Sunshine. Verify that two local original Moonlight clients receive video concurrently, and that unpairing one leaves the other active.
- [ ] Validate the display driver on Windows 11 x64. Add the small adapter that enumerates supported modes, owns a single extended monitor and removes its own monitor on service teardown.

### Task 5. Installer, migration and release acceptance

- [x] Build the Windows engine and produce a setup candidate with the native service, desktop app, virtual-display probe and default-checked ViGEmBus support.
- [ ] Verify installation and reuse of the ViGEmBus component from the setup candidate, and package a verified SideLantern-owned virtual-display driver.
- [x] Test encrypted migration against backed-up inputs by preserving identity and application entries and keeping both profiles independently restorable.
- [ ] Execute all acceptance items from the design in a clean and in a pre-existing installation. Verify signed driver distribution and test using two original Moonlight clients.
