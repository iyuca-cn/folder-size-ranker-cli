# `--all` 扫描性能优化 Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce `--all` scan overhead by removing Win32 allocated-size backfill and deriving WOF/AppX allocated bytes directly from NTFS metadata.

**Architecture:** Keep the MFT scan and JSON tree builder intact. Extend `src/ntfs_record.c` to recognize the `WofCompressedData` named stream as the physical backing store for WOF-compressed files, then remove the aggregate-layer `GetCompressedFileSizeW` fallback so all outputs share the same MFT-derived allocated-size path.

**Tech Stack:** C17, Win32 API, NTFS MFT parser, Visual Studio MSBuild, `pwsh`.

---

## Chunk 1: WOF Allocated-Size Parsing

### Task 1: Capture `WofCompressedData`

**Files:**
- Modify: `src/ntfs_record.c`

- [ ] **Step 1: Extend parse state**

Add a dedicated candidate to `MftscanNtfsParseState`:

```c
MftscanNtfsDataSizeCandidate wof_backing_size;
```

- [ ] **Step 2: Add stream-name helpers**

Add helpers to compare attribute names and attribute-list entry names with:

```c
mftscan_attribute_name_equals(..., L"WofCompressedData")
```

- [ ] **Step 3: Capture direct named stream**

When parsing non-directory named `$DATA`, route `WofCompressedData` into `wof_backing_size` instead of generic fallback metadata.

- [ ] **Step 4: Capture attribute-list named stream**

Update `$ATTRIBUTE_LIST` resolution so `WofCompressedData` fragments also land in `wof_backing_size`.

- [ ] **Step 5: Apply allocated-size fallback**

Before writing `record_info->allocated_size`, if the unnamed stream resolved to `0` but `wof_backing_size` is present, use the WOF allocated size.

## Chunk 2: Remove Win32 Backfill

### Task 2: Delete aggregate-layer fallback

**Files:**
- Modify: `src/model.h`
- Modify: `src/main.c`
- Modify: `src/aggregate.c`

- [ ] **Step 1: Remove public declaration**

Delete `mftscan_backfill_zero_allocated_files(...)` from `src/model.h`.

- [ ] **Step 2: Remove main call site**

Delete the call from `src/main.c`.

- [ ] **Step 3: Remove aggregate helpers**

Delete:

```c
mftscan_try_query_allocated_size
mftscan_build_file_path
mftscan_find_filter_root_frn
mftscan_directory_is_descendant_of
mftscan_backfill_zero_allocated_files
```

Expected: NTFS fast path no longer depends on `GetCompressedFileSizeW`.

## Chunk 3: Verification

### Task 3: Validate behavior and performance

**Files:**
- Modify: none unless verification exposes an issue.

- [ ] **Step 1: Build both platforms**

Run:

```powershell
& "D:\vs\2022\Community\MSBuild\Current\Bin\MSBuild.exe" .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64
& "D:\vs\2022\Community\MSBuild\Current\Bin\MSBuild.exe" .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32
```

Expected: both builds succeed with 0 errors.

- [ ] **Step 2: Check allocated `--all` correctness**

Run:

```powershell
$json = & "x64/Release/folder-size-ranker-cli.exe" --location "C:\Program Files\WindowsApps" --sort allocated --all | ConvertFrom-Json
$json.bytes
```

Expected: root `bytes` stays correct without任何 Win32 API 回填。

- [ ] **Step 3: Check logical `--all` correctness**

Run:

```powershell
$json = & "x64/Release/folder-size-ranker-cli.exe" --location "C:\Program Files\WindowsApps" --sort logical --all | ConvertFrom-Json
$json.bytes
```

Expected: root `bytes` is unchanged from pre-optimization logical output.

- [ ] **Step 4: Measure target timings**

Run:

```powershell
$commands = @(
    @{ Name = "WindowsApps allocated all"; Args = @("--location", "C:\Program Files\WindowsApps", "--sort", "allocated", "--all") },
    @{ Name = "WindowsApps logical all"; Args = @("--location", "C:\Program Files\WindowsApps", "--sort", "logical", "--all") }
)
$results = foreach ($cmd in $commands) {
    $elapsed = Measure-Command { & "x64/Release/folder-size-ranker-cli.exe" @($cmd.Args) | Out-Null }
    [pscustomobject]@{ Name = $cmd.Name; Seconds = [math]::Round($elapsed.TotalSeconds, 3) }
}
$results | Format-Table -AutoSize
```

Expected: logical mode improves noticeably; allocated mode should improve or remain similar while preserving fixed output.

- [ ] **Step 5: Commit implementation**

Run:

```powershell
git add src/model.h src/main.c src/aggregate.c
git add src/ntfs_record.c README.md docs/plans/2026-04-22-all-scan-performance-design.md docs/superpowers/plans/2026-04-22-all-scan-performance.md
git commit -m "fix: derive WOF allocated size from MFT"
```
