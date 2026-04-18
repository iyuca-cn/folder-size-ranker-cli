# MFT Leaf Directory Scanner CLI Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a single-EXE Windows CLI in C that reads a specified NTFS volume's MFT and outputs sorted leaf-directory sizes in table or JSON format.

**Architecture:** The program is a single Visual Studio EXE project split across focused C source files. It opens an NTFS volume directly, enumerates MFT file records with `FSCTL_GET_NTFS_FILE_RECORD`, parses only the fields needed for leaf-directory accounting, aggregates direct-file sizes into parent directories, filters to directories without child directories, then renders table or JSON output.

**Tech Stack:** C17, Win32 API, NTFS volume control codes, Visual Studio/MSBuild, yyjson

---

## Chunk 1: Project Skeleton

### Task 1: Create the Visual Studio project files

**Files:**
- Create: `mftscan.sln`
- Create: `mftscan.vcxproj`
- Create: `mftscan.vcxproj.filters`
- Create: `app.manifest`

- [ ] **Step 1: Write the solution file**

Create `mftscan.sln` containing one C executable project targeting Visual Studio 2022.

- [ ] **Step 2: Write the project file**

Create `mftscan.vcxproj` with:
- `ConfigurationType=Application`
- `WindowsTargetPlatformVersion=10.0`
- C language compilation enabled
- Unicode character set
- source/header/resource file inclusions
- embedded manifest support for `app.manifest`

- [ ] **Step 3: Add filters**

Create `mftscan.vcxproj.filters` so Visual Studio groups `src`, `include`, and `third_party`.

- [ ] **Step 4: Add the admin manifest**

Create `app.manifest` with `requestedExecutionLevel level="requireAdministrator"`.

- [ ] **Step 5: Build skeleton**

Run:

```powershell
& 'D:\vs\2022\Community\MSBuild\Current\Bin\MSBuild.exe' .\mftscan.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
```

Expected: project resolves and fails only because source files are not yet present, or succeeds once placeholder files are added.

## Chunk 2: Core Types and CLI Surface

### Task 2: Define shared models and CLI option parsing

**Files:**
- Create: `include/mftscan.h`
- Create: `src/model.h`
- Create: `src/main.c`
- Create: `src/util.c`

- [ ] **Step 1: Define enums and data structures**

Add option, result, directory node, dynamic array, and error code types with exact responsibilities documented in the headers.

- [ ] **Step 2: Implement argument parsing**

Support:
- `--volume`
- `--sort`
- `--min-size`
- `--format`
- `--limit`
- `--help`

- [ ] **Step 3: Implement usage/help text**

Print a concise help message and return success for `--help`.

- [ ] **Step 4: Add minimal main flow**

Wire argument parsing, admin check stub, and placeholder output so the executable runs end-to-end.

- [ ] **Step 5: Build and run help**

Run:

```powershell
& 'D:\vs\2022\Community\MSBuild\Current\Bin\MSBuild.exe' .\mftscan.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
.\x64\Debug\mftscan.exe --help
```

Expected: build succeeds and help text is printed.

## Chunk 3: NTFS Access and Record Parsing

### Task 3: Implement volume open, MFT enumeration, and record parsing

**Files:**
- Create: `src/admin.c`
- Create: `src/ntfs_volume.c`
- Create: `src/ntfs_mft.c`
- Create: `src/ntfs_record.c`
- Modify: `include/mftscan.h`
- Modify: `src/model.h`

- [ ] **Step 1: Implement admin/runtime checks**

Add a Win32 admin detection helper so runtime can fail cleanly if elevation is missing.

- [ ] **Step 2: Implement NTFS volume open**

Open `\\.\X:` with `CreateFileW`, verify filesystem name with `GetVolumeInformationW`, and fetch `NTFS_VOLUME_DATA_BUFFER`.

- [ ] **Step 3: Implement MFT record enumeration**

Enumerate records downward using `FSCTL_GET_NTFS_FILE_RECORD`, starting from `MftValidDataLength / BytesPerFileRecordSegment`.

- [ ] **Step 4: Implement record fixup and attribute parsing**

Parse:
- `FILE` signature
- update sequence array
- in-use / directory flags
- `FILE_NAME` attribute

- [ ] **Step 5: Capture record fields**

Populate record summaries with:
- FRN
- parent FRN
- chosen display name
- `is_directory`
- `logical_size`
- `allocated_size`

- [ ] **Step 6: Build and smoke-test record scan**

Run:

```powershell
& 'D:\vs\2022\Community\MSBuild\Current\Bin\MSBuild.exe' .\mftscan.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
.\x64\Debug\mftscan.exe --volume C: --sort logical --limit 5
```

Expected: either valid scan output or a clear runtime error explaining the missing admin / NTFS requirement.

## Chunk 4: Aggregation, Path Recovery, and Output

### Task 4: Implement leaf-directory aggregation and rendering

**Files:**
- Create: `src/aggregate.c`
- Create: `src/path_resolver.c`
- Create: `src/output_table.c`
- Create: `src/output_json.c`
- Create: `third_party/yyjson/yyjson.h`
- Create: `third_party/yyjson/yyjson.c`
- Create: `third_party/yyjson/LICENSE`
- Modify: `include/mftscan.h`
- Modify: `src/main.c`
- Modify: `src/model.h`

- [ ] **Step 1: Implement directory and file indexes**

Maintain:
- a directory-node array or map keyed by FRN
- a file FRN dedupe set
- child-directory markers

- [ ] **Step 2: Implement direct-file aggregation**

For each non-directory record counted once by FRN:
- add logical bytes to its direct parent directory
- add allocated bytes to its direct parent directory

- [ ] **Step 3: Filter, sort, and limit results**

Keep only directories where `has_child_dir == false`, then filter by requested size field, sort descending, and apply `--limit`.

- [ ] **Step 4: Restore full paths**

Build `C:\...` paths lazily for final result rows by walking `parent_frn`.

- [ ] **Step 5: Add table and JSON outputs**

Use `yyjson` for JSON generation and a fixed-width text table for console output.

- [ ] **Step 6: Build and validate both formats**

Run:

```powershell
& 'D:\vs\2022\Community\MSBuild\Current\Bin\MSBuild.exe' .\mftscan.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64
.\x64\Release\mftscan.exe --volume C: --sort logical --format table --limit 10
.\x64\Release\mftscan.exe --volume C: --sort allocated --format json --min-size 1048576 --limit 10
```

Expected: both commands succeed and the JSON output is valid UTF-8 JSON.

## Chunk 5: Finish and Validate

### Task 5: Polish diagnostics and verify edge cases

**Files:**
- Modify: `src/*.c`
- Modify: `include/mftscan.h`
- Modify: `src/model.h`

- [ ] **Step 1: Normalize error messages**

Make CLI failures concise and actionable:
- invalid argument
- not elevated
- unsupported filesystem
- scan failure

- [ ] **Step 2: Validate filtering and empty results**

Run:

```powershell
.\x64\Release\mftscan.exe --volume C: --sort allocated --min-size 18446744073709551615 --format json
```

Expected: valid JSON with `items: []`.

- [ ] **Step 3: Validate sort modes**

Run:

```powershell
.\x64\Release\mftscan.exe --volume C: --sort logical --format table --limit 20
.\x64\Release\mftscan.exe --volume C: --sort allocated --format table --limit 20
```

Expected: both succeed and ordering reflects the selected field.

- [ ] **Step 4: Document current repository limitation**

Note that the workspace is not a git repository, so commit steps are intentionally skipped.

- [ ] **Step 5: Final build**

Run:

```powershell
& 'D:\vs\2022\Community\MSBuild\Current\Bin\MSBuild.exe' .\mftscan.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64
```

Expected: `Build succeeded.`

Plan complete and saved to `docs/superpowers/plans/2026-04-18-mftscan-cli.md`. Execution proceeds in the current session because no dedicated plan-execution skill is available in this workspace.
