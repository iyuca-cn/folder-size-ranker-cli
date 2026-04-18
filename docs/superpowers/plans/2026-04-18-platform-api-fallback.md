# Platform API Fallback Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a non-recursive platform API fallback scanner for non-NTFS volumes while preserving the NTFS MFT fast path.

**Architecture:** The public scanner probes the volume filesystem and dispatches to either the existing NTFS backend or a new Win32 API backend. The fallback backend uses an explicit stack, skips directory reparse points, converts entries into existing `MftscanRecordInfo` records, and reuses aggregation and output code.

**Tech Stack:** C17, Win32 API, Visual Studio/MSBuild

---

## Chunk 1: Backend Dispatch

### Task 1: Add filesystem probe and dispatcher

**Files:**
- Modify: `src/model.h`
- Modify: `src/ntfs_volume.c`
- Modify: `src/ntfs_mft.c`
- Create: `src/scan.c`

- [ ] **Step 1: Add internal filesystem enum**

Add `MftscanFilesystemKind` with `MFTSCAN_FILESYSTEM_NTFS` and `MFTSCAN_FILESYSTEM_OTHER`.

- [ ] **Step 2: Add probe function**

Expose `mftscan_probe_volume_filesystem()` from `src/ntfs_volume.c` using `GetVolumeInformationW`.

- [ ] **Step 3: Rename NTFS scanner**

Rename the old `mftscan_scan_volume()` in `src/ntfs_mft.c` to `mftscan_scan_volume_ntfs()`.

- [ ] **Step 4: Add dispatcher**

Create `src/scan.c` with the public `mftscan_scan_volume()` that probes the filesystem, checks admin only for NTFS, and dispatches.

## Chunk 2: Platform Scanner

### Task 2: Implement non-recursive fallback scanner

**Files:**
- Create: `src/platform_scan.c`
- Modify: `src/model.h`

- [ ] **Step 1: Add explicit pending-directory stack**

Store full directory paths and process-local directory IDs.

- [ ] **Step 2: Enumerate entries iteratively**

Use `FindFirstFileExW` and `FindNextFileW`; do not call a recursive function.

- [ ] **Step 3: Convert entries to records**

Create directory and file `MftscanRecordInfo` values and feed them into `mftscan_ingest_record()`.

- [ ] **Step 4: Preserve edge behavior**

Skip directory reparse points, skip inaccessible directories, and fall back to logical size when allocated-size lookup fails.

## Chunk 3: Permissions, Project, Docs

### Task 3: Wire build files and documentation

**Files:**
- Modify: `src/main.c`
- Modify: `src/util.c`
- Modify: `folder-size-ranker-cli.vcxproj`
- Modify: `folder-size-ranker-cli.vcxproj.filters`
- Modify: `app.manifest`
- Modify: `README.md`

- [ ] **Step 1: Remove global admin gate**

Let the dispatcher enforce admin only for NTFS.

- [ ] **Step 2: Add new source files to the project**

Include `src/scan.c` and `src/platform_scan.c`.

- [ ] **Step 3: Allow non-admin startup**

Change manifest execution level to `asInvoker`.

- [ ] **Step 4: Update docs**

Document NTFS fast path, non-NTFS fallback, non-recursive traversal, and reparse-point behavior.

- [ ] **Step 5: Build**

Run MSBuild for x64 Release and fix compile errors if any.

