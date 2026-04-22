# NTFS Attribute List Logical Size Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking. Do not create commits unless the user explicitly asks.

**Goal:** Resolve NTFS logical-size inflation by parsing `$ATTRIBUTE_LIST` and extension records so unnamed `$DATA` drives file sizes instead of falling back to `FILE_NAME.real_size` for complex records.

**Architecture:** Keep the current NTFS MFT scan pipeline, but refactor record reading into reusable helpers and upgrade the parser to resolve unnamed `$DATA` across base records, `$ATTRIBUTE_LIST`, and extension records. Preserve the existing aggregation and JSON output paths so directory totals improve automatically once per-file sizes are corrected.

**Tech Stack:** C17, Win32 API, NTFS on-disk structures, Visual Studio/MSBuild, PowerShell 7 (`pwsh`).

---

## Chunk 1: Reuse NTFS record reads

### Task 1: Extend volume metadata and parser entry points

**Files:**
- Modify: `src/model.h`
- Modify: `src/ntfs_volume.c`
- Modify: `src/ntfs_mft.c`

- [ ] Add any missing NTFS volume geometry needed for nonresident attribute reads, including cluster size.
- [ ] Change the NTFS parser interface so `mftscan_parse_file_record` can access the open volume handle instead of only raw sector size arguments.
- [ ] Keep the public scan flow unchanged for callers outside the NTFS backend.

### Task 2: Extract a reusable file-record read helper

**Files:**
- Modify: `src/model.h`
- Modify: `src/ntfs_mft.c`

- [ ] Move the `FSCTL_GET_NTFS_FILE_RECORD` logic into a helper that reads a specific FRN into a caller-provided buffer.
- [ ] Return both the actual FRN read and a consistent `MftscanError` mapping for EOF, malformed results, and I/O failures.
- [ ] Reuse the helper in the existing descending MFT enumeration loop so the main scan behavior does not regress.

## Chunk 2: Parse `$ATTRIBUTE_LIST`

### Task 3: Add attribute-list parsing state

**Files:**
- Modify: `src/ntfs_record.c`

- [ ] Add packed NTFS structs for `$ATTRIBUTE_LIST` entries and any parser-local state needed to track unnamed `$DATA`, seen extension FRNs, and whether an attribute list exists.
- [ ] Centralize final size resolution so `logical_size` and `allocated_size` are assigned in one place instead of multiple ad-hoc branches.
- [ ] Preserve current file-name extraction and directory metadata behavior.

### Task 4: Read resident and nonresident attribute-list payloads

**Files:**
- Modify: `src/ntfs_record.c`
- Modify: `src/ntfs_mft.c`

- [ ] Parse resident `$ATTRIBUTE_LIST` values with strict bounds checks.
- [ ] Add runlist-driven reading for nonresident `$ATTRIBUTE_LIST` payloads using the opened NTFS volume handle.
- [ ] Filter list entries so only unnamed `$DATA` candidates continue to the next stage; ignore ADS and unrelated attributes.

## Chunk 3: Resolve extension-record data sizes

### Task 5: Follow attribute-list entries into extension records

**Files:**
- Modify: `src/ntfs_record.c`

- [ ] For each unnamed `$DATA` list entry, read the referenced record when it is not already the base record.
- [ ] Validate the fetched record belongs to the same file via base-record linkage before trusting any attribute content.
- [ ] Detect loops and cap the number of followed extension records per file.

### Task 6: Use unnamed `$DATA` as the authoritative size source

**Files:**
- Modify: `src/ntfs_record.c`

- [ ] Match unnamed `$DATA` fragments across base and extension records.
- [ ] Treat the fragment with `lowest_vcn == 0` as the authoritative source of `data_size` and `allocated_size`.
- [ ] Only fall back to `FILE_NAME.real_size` when no unnamed `$DATA` exists and no `$ATTRIBUTE_LIST` is present.
- [ ] Return `MFTSCAN_ERROR_MFT_PARSE` instead of silently falling back when an attribute list exists but cannot be resolved correctly.

## Chunk 4: Documentation and validation

### Task 7: Update documentation for NTFS size semantics

**Files:**
- Modify: `README.md`

- [ ] Clarify that NTFS logical and allocated sizes come from the unnamed `$DATA` stream.
- [ ] Clarify that named ADS are not counted toward directory sizes.
- [ ] Keep user-facing CLI behavior unchanged.

### Task 8: Build and smoke test

**Commands:**
- Run: `msbuild .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64`
- Run: `.\x64\Release\folder-size-ranker-cli.exe --location C:\Temp --sort logical --all`
- Run: `.\x64\Release\folder-size-ranker-cli.exe --location C:\Temp --sort allocated --all`

**Expected:**
- x64 Release build succeeds.
- `--sort logical --all` runs successfully in an elevated terminal and the root `bytes` value no longer appears inflated versus Explorer.
- `--sort allocated --all` still works and does not regress.
