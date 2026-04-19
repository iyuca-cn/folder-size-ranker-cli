# Location Volume Root Fallback Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking. Do not create commits unless the user explicitly asks.

**Goal:** Fix `--location <folder>` on local virtual drives where `GetVolumePathNameW` returns a folder path instead of a drive root, while still rejecting true folder-mounted volumes.

**Architecture:** Keep the existing `GetVolumePathNameW` fast path. When it returns a non-drive-root path, distinguish between a real folder mount point and a buggy/driver-specific subdirectory result via `GetVolumeNameForVolumeMountPointW`. Reject the former, and fall back to the drive-letter prefix for the latter.

**Tech Stack:** C17, Win32 API, MSBuild, PowerShell 7 (`pwsh`).

---

## Chunk 1: Location Parsing

### Task 1: Add mount-point probe helpers

**Files:**
- Modify: `src/util.c`

- [ ] Add a helper to extract `X:\` from a normalized path.
- [ ] Add a helper that appends `\` and probes `GetVolumeNameForVolumeMountPointW`.

### Task 2: Apply fallback in `--location`

**Files:**
- Modify: `src/util.c`

- [ ] Keep the existing direct drive-root path branch unchanged.
- [ ] After `GetVolumePathNameW`, detect non-drive-root results.
- [ ] Reject real folder mount points with a clearer Chinese error.
- [ ] Fall back to the drive-letter root for non-mount-point anomalies.

## Chunk 2: Docs and Validation

### Task 3: Update behavior notes

**Files:**
- Modify: `README.md`

- [ ] Document the fallback for local virtual drives.
- [ ] Document that folder-mounted volumes remain unsupported.

### Task 4: Build and smoke test

**Commands:**
- Run: `msbuild .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64`
- Run: `.\x64\Release\folder-size-ranker-cli.exe --location C:\Users --sort allocated --format table --limit 5`
- Run: `.\x64\Release\folder-size-ranker-cli.exe --location X:\qq files --sort allocated --format table --limit 5`
- Run: `.\x64\Release\folder-size-ranker-cli.exe --location X: --sort allocated --format table --limit 5`

**Expected:**
- Build succeeds.
- `C:\Users` remains valid.
- `X:\qq files` no longer errors on the current RamDisk.
- `X:` behavior remains unchanged.
