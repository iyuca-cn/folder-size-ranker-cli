# Location CLI Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking. Do not create commits unless the user explicitly asks.

**Goal:** Replace `--volume` with `--location`, support folder targets, preserve NTFS full-volume MFT scanning, and fix Chinese error mojibake.

**Architecture:** Parse `--location` into a normalized target path and an internal drive volume. Scan the volume exactly as before, then filter built result paths by the normalized target directory when the target is not a volume root. Initialize console UTF-8 output before any diagnostics.

**Tech Stack:** C, Win32 API, Visual Studio/MSBuild, PowerShell 7 (`pwsh`).

---

## Chunk 1: CLI Options and Path Parsing

### Task 1: Extend option model

**Files:**
- Modify: `include/mftscan.h`

- [ ] Add dynamic `location` storage to `MftscanOptions`.
- [ ] Add `filter_root` storage and `filter_by_location` flag.
- [ ] Declare `mftscan_free_options`.

### Task 2: Parse `--location`

**Files:**
- Modify: `src/util.c`

- [ ] Add helpers to duplicate wide strings and trim trailing path separators.
- [ ] Add a helper that accepts `X:`, `X:\`, or a folder path.
- [ ] Use `GetFullPathNameW` and `GetVolumePathNameW` for folder paths.
- [ ] Fill `options->volume`, `options->location`, and filter fields.
- [ ] Reject `--volume` with a clear Chinese error message.
- [ ] Update required-argument validation to require `--location` and `--sort`.
- [ ] Add `mftscan_free_options` to free dynamic option strings.

## Chunk 2: Result Filtering and Output

### Task 3: Filter results by location

**Files:**
- Modify: `src/aggregate.c`

- [ ] Add a case-insensitive path containment helper.
- [ ] After building each path, skip it if it is outside `options->filter_root`.
- [ ] Keep volume-root scans unfiltered.

### Task 4: Update JSON metadata

**Files:**
- Modify: `src/output_json.c`

- [ ] Add a `location` JSON field using `options->location`.
- [ ] Keep `volume` as the resolved internal volume for compatibility.

## Chunk 3: Encoding and Documentation

### Task 5: Fix Chinese error output

**Files:**
- Modify: `src/main.c`

- [ ] Set console output code page to UTF-8 before parsing options.
- [ ] Keep existing narrow output behavior so table and JSON remain UTF-8.
- [ ] Ensure parse-failure diagnostics use the initialized output code page.

### Task 6: Update help and README

**Files:**
- Modify: `src/util.c`
- Modify: `README.md`

- [ ] Replace usage text with `--location <path>`.
- [ ] Replace examples with `--location`.
- [ ] Document that `--volume` is no longer supported.
- [ ] Document folder targets and NTFS full-volume MFT behavior.

## Chunk 4: Validation

### Task 7: Build and smoke test

**Commands:**
- Run: `msbuild .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64`
- Run: `.\x64\Release\folder-size-ranker-cli.exe --volume X: --sort allocated --format table --limit 20`
- Run: `.\x64\Release\folder-size-ranker-cli.exe --location X: --sort allocated --format table --limit 20`
- Run: `.\x64\Release\folder-size-ranker-cli.exe --location X:\SomeFolder --sort allocated --format table --limit 20`

**Expected:**
- Build succeeds.
- Deprecated `--volume` reports readable Chinese.
- `--location X:` scans the resolved volume.
- Folder location output paths are inside the requested folder.
