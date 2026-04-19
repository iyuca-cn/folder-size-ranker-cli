# Dual-Arch Release Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking. Do not create commits unless the user explicitly asks.

**Goal:** Enable local `x86` Release builds and make GitHub Release publish renamed standalone `x86` and `x64` exe assets.

**Architecture:** Extend the existing Visual Studio solution/project with `Win32` configurations instead of creating a second project. Keep build outputs separated by architecture, build both targets in the Release workflow, and stage renamed exe files for upload. Use static CRT in Release so each published asset remains a single exe.

**Tech Stack:** Visual Studio solution/vcxproj, MSBuild, GitHub Actions, PowerShell 7 (`pwsh`), C.

---

## Chunk 1: Project Configuration

### Task 1: Add `Win32` solution mappings

**Files:**
- Modify: `folder-size-ranker-cli.sln`

- [ ] Add `Debug|Win32` and `Release|Win32` to `SolutionConfigurationPlatforms`.
- [ ] Map the existing project GUID to `Debug|Win32` and `Release|Win32`.

### Task 2: Add `Win32` project configurations

**Files:**
- Modify: `folder-size-ranker-cli.vcxproj`

- [ ] Add `Debug|Win32` and `Release|Win32` under `ProjectConfigurations`.
- [ ] Add matching `PropertyGroup` configuration blocks.
- [ ] Add matching `ImportGroup` property-sheet blocks.
- [ ] Add `ItemDefinitionGroup` blocks for `Debug|Win32` and `Release|Win32`.
- [ ] Separate output directories into `x86\$(Configuration)` and `x64\$(Configuration)`.
- [ ] Set Release runtime library to static CRT for both architectures.

## Chunk 2: Release Workflow

### Task 3: Build and stage both release assets

**Files:**
- Modify: `.github/workflows/release.yml`

- [ ] Build `Release|Win32`.
- [ ] Build `Release|x64`.
- [ ] Copy output exe files into a staging directory.
- [ ] Rename staged files to `folder-size-ranker-cli-x86.exe` and `folder-size-ranker-cli-x64.exe`.
- [ ] Upload both staged files in the GitHub Release step.

## Chunk 3: Documentation

### Task 4: Update build and release docs

**Files:**
- Modify: `README.md`

- [ ] Document local `x86` and `x64` Release build commands.
- [ ] Document the `x86\Release` and `x64\Release` output paths.
- [ ] Document that GitHub Release publishes two renamed exe assets.
- [ ] Clarify that Release artifacts are standalone single-exe builds.

## Chunk 4: Validation

### Task 5: Verify local `x86` Release build

**Commands:**
- Run: `msbuild .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32 /m`

**Expected:**
- Build succeeds.
- Output file exists at `x86\Release\folder-size-ranker-cli.exe`.

### Task 6: Inspect staged release expectations

**Commands:**
- Run: `Test-Path .\x86\Release\folder-size-ranker-cli.exe`
- Run: `Select-String -Path .\.github\workflows\release.yml -Pattern 'folder-size-ranker-cli-x86.exe|folder-size-ranker-cli-x64.exe'`

**Expected:**
- Local x86 exe path exists after build.
- Workflow references both renamed release assets.
