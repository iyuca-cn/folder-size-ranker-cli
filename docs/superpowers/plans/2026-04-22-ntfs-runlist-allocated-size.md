# NTFS Runlist Allocated Size Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix NTFS allocated-size parsing by deriving non-resident allocation from runlists and using that corrected logic for system records under `$Extend`.

**Architecture:** Keep the existing MFT parsing pipeline, but replace non-resident allocated-size derivation with runlist-based counting of actually allocated runs. Preserve unnamed `$DATA` as the primary file-size source, then allow system-only fallback to named system streams and directory metadata attributes once the core allocation math is correct.

**Tech Stack:** C17, NTFS on-disk structures, Win32 volume I/O, Visual Studio/MSBuild, PowerShell 7 (`pwsh`).

---

## Chunk 1: Correct non-resident allocation math

### Task 1: Add runlist-based allocation helper

**Files:**
- Modify: `src/ntfs_record.c`

- [ ] Parse mapping pairs directly from non-resident attributes.
- [ ] Sum only runs with real LCN mappings.
- [ ] Ignore sparse holes when computing `allocated_size`.

### Task 2: Reuse the helper for unnamed `$DATA`

**Files:**
- Modify: `src/ntfs_record.c`

- [ ] Replace direct header `allocated_size` usage for non-resident unnamed `$DATA`.
- [ ] Keep resident unnamed `$DATA` allocated size at `0`.
- [ ] Accumulate all unnamed `$DATA` fragments referenced by `$ATTRIBUTE_LIST`.

## Chunk 2: Re-enable system-only fallback

### Task 3: Limit fallback to system records

**Files:**
- Modify: `src/model.h`
- Modify: `src/ntfs_record.c`
- Modify: `src/aggregate.c`

- [ ] Mark records that belong to the `$Extend` metadata subtree using MFT parent/child relationships.
- [ ] Use named system streams / index metadata only for system records with no unnamed `$DATA`.
- [ ] Let system directory metadata contribute to `allocated_size` only.

## Chunk 3: Document and validate

### Task 4: Update docs

**Files:**
- Modify: `README.md`
- Create: `docs/plans/2026-04-22-ntfs-runlist-allocated-size-design.md`

- [ ] Document runlist-based allocation and sparse-hole exclusion.
- [ ] Document the system-only fallback rule.

### Task 5: Validate on target scenarios

**Commands:**
- Run: `msbuild .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64`
- Run: `.\x64\Release\folder-size-ranker-cli.exe --location 'E:\$Extend' --sort allocated --all`

**Expected:**
- x64 Release build succeeds.
- `$UsnJrnl` returns to a tens-of-megabytes range instead of `0` or `1GB+`.
- resident and ordinary non-resident file regressions do not appear.
