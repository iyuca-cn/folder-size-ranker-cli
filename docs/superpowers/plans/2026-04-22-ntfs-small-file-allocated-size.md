# NTFS Small File Allocated Size Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make NTFS allocated-size parsing match Explorer-style small-file behavior by treating resident unnamed `$DATA` as zero allocated bytes.

**Architecture:** Keep the existing MFT scan pipeline and only adjust how `src/ntfs_record.c` derives `allocated_size` from unnamed `$DATA`. Resident data returns zero allocated bytes, while non-resident data keeps using the NTFS attribute header values, including when `$ATTRIBUTE_LIST` resolves through extension records.

**Tech Stack:** C17, Win32/NTFS on-disk structures, Visual Studio/MSBuild, PowerShell 7 (`pwsh`).

---

## Chunk 1: Fix parser semantics

### Task 1: Update unnamed `$DATA` size extraction

**Files:**
- Modify: `src/ntfs_record.c`

- [ ] **Step 1: Adjust resident unnamed `$DATA` candidate generation**

Set resident unnamed `$DATA` to:

```c
logical_size = resident_header->value_length;
allocated_size = 0;
```

- [ ] **Step 2: Keep non-resident unnamed `$DATA` logic unchanged**

Preserve:

```c
logical_size = non_resident_header->data_size;
allocated_size = non_resident_header->allocated_size;
```

- [ ] **Step 3: Verify `$ATTRIBUTE_LIST` resolution still uses the same helper**

Confirm extension-record parsing continues to flow through `mftscan_capture_data_size_candidate()` so the resident/non-resident rule stays consistent everywhere.

## Chunk 2: Document and validate

### Task 2: Clarify user-facing semantics

**Files:**
- Modify: `README.md`
- Create: `docs/plans/2026-04-22-ntfs-small-file-allocated-size-design.md`

- [ ] **Step 1: Document resident vs non-resident allocated-size behavior**

Clarify that Explorer-style allocated-size results depend on whether the unnamed `$DATA` stream is currently resident or non-resident.

### Task 3: Build validation

**Commands:**
- Run: `msbuild .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64`

**Expected:**
- x64 Release build succeeds.
- The parser compiles cleanly after the resident-data size change.
