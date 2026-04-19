# All JSON Files Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking. Do not create commits unless the user explicitly asks.

**Goal:** Extend `--all` so each directory node includes direct files with sizes while preserving recursive directory totals and compact JSON output.

**Architecture:** Keep the existing directory tree for `--all`, and add a parallel file collection captured during scanning. During JSON emission, build per-directory child-directory and direct-file indexes, sort both by the active size mode, then write each directory as `{ path, bytes, files, children }`.

**Tech Stack:** C, Win32 API, yyjson, Visual Studio/MSBuild, PowerShell 7 (`pwsh`).

---

## Chunk 1: Preserve file nodes during scanning

### Task 1: Extend scan context with files

**Files:**
- Modify: `src/model.h`
- Modify: `src/aggregate.c`

- [ ] Add `MftscanFileNode` and `MftscanFileVector`.
- [ ] Add file storage to `MftscanContext`.
- [ ] Free file names and file buffers in context cleanup.
- [ ] Store file nodes during `mftscan_ingest_record`.

### Task 2: Pass file names from platform scanning

**Files:**
- Modify: `src/platform_scan.c`

- [ ] Extend the platform file ingest helper to accept the direct file name.
- [ ] Copy the file name into `MftscanRecordInfo` before ingest.

## Chunk 2: Emit files in `--all` JSON

### Task 3: Build directory/file indexes for `--all`

**Files:**
- Modify: `src/output_json.c`

- [ ] Add per-directory direct-file index arrays alongside child-directory arrays.
- [ ] Sort `children` by directory totals.
- [ ] Sort `files` by single-file size.

### Task 4: Write `files` arrays

**Files:**
- Modify: `src/output_json.c`

- [ ] Add a helper to build full file paths from parent directory plus file name.
- [ ] Emit `files` before `children` in each directory node.
- [ ] Apply `--min-size` to files and child directories.
- [ ] Apply `--limit` separately to each directory node’s `files` and `children`.

## Chunk 3: Documentation and validation

### Task 5: Update docs

**Files:**
- Modify: `README.md`

- [ ] Document the `files` field in `--all`.
- [ ] Clarify the `bytes` meaning for directories vs files.
- [ ] Clarify the per-layer `--limit` behavior.

### Task 6: Build and smoke test

**Commands:**
- Run: `msbuild .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64`
- Run: `.\x64\Release\folder-size-ranker-cli.exe --location . --sort logical --all --limit 1`

**Expected:**
- Build succeeds.
- Output JSON can be parsed.
- Root node contains `files` and `children`.
