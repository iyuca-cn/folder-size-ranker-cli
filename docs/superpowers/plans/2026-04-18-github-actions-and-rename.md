# GitHub Actions and Project Rename Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add GitHub Actions for CI and tag-based releases, and rename the project/output to `folder-size-ranker-cli`.

**Architecture:** Use two GitHub Actions workflows: one for push/PR build validation and one for annotated-tag release publishing. Rename the Visual Studio solution/project/output files to the new product name and update documentation so local and CI builds use the same relative `msbuild` invocation.

**Tech Stack:** GitHub Actions, Windows runners, Visual Studio/MSBuild, PowerShell, Git annotated tags

---

## Chunk 1: Rename the Visual Studio project

### Task 1: Rename solution, project, filters, and output artifact

**Files:**
- Move: `mftscan.sln` -> `folder-size-ranker-cli.sln`
- Move: `mftscan.vcxproj` -> `folder-size-ranker-cli.vcxproj`
- Move: `mftscan.vcxproj.filters` -> `folder-size-ranker-cli.vcxproj.filters`
- Modify: `folder-size-ranker-cli.sln`
- Modify: `folder-size-ranker-cli.vcxproj`

- [ ] **Step 1: Rename the files**

Rename the solution and project files to the new product name.

- [ ] **Step 2: Update internal project references**

Adjust the `.sln` project path and the `.vcxproj` target/output names to `folder-size-ranker-cli`.

- [ ] **Step 3: Build locally**

Run:

```powershell
msbuild .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64
```

Expected: `x64\Release\folder-size-ranker-cli.exe` is produced successfully.

## Chunk 2: Add CI workflow

### Task 2: Add push/PR build validation

**Files:**
- Create: `.github/workflows/ci.yml`

- [ ] **Step 1: Create Windows CI workflow**

Trigger on `push` and `pull_request`.

- [ ] **Step 2: Configure checkout and MSBuild**

Use:
- `actions/checkout`
- `microsoft/setup-msbuild`

- [ ] **Step 3: Build the project**

Run:

```powershell
msbuild .\folder-size-ranker-cli.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64
```

- [ ] **Step 4: Verify workflow file structure**

Ensure the YAML is syntactically valid and references the renamed project file.

## Chunk 3: Add release workflow

### Task 3: Publish releases on annotated tag push

**Files:**
- Create: `.github/workflows/release.yml`

- [ ] **Step 1: Trigger on `v*` tags**

Limit workflow to tag pushes matching `v*`.

- [ ] **Step 2: Build release artifact**

Build `Release|x64` on `windows-latest`.

- [ ] **Step 3: Read annotated tag message**

Use Git on the runner to read the full annotated tag contents for `${GITHUB_REF_NAME}` into a notes file.

- [ ] **Step 4: Create GitHub Release**

Use a release action to create/update the release for the tag using the notes file as the body.

- [ ] **Step 5: Upload executable**

Attach `x64\Release\folder-size-ranker-cli.exe` to the release.

## Chunk 4: Update README

### Task 4: Align docs with new name and CI usage

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Update project name**

Replace `mftscan` references with `folder-size-ranker-cli` where they refer to the repo/product name.

- [ ] **Step 2: Remove machine-specific MSBuild path**

Document using Developer PowerShell / Developer Command Prompt and a relative `msbuild` command.

- [ ] **Step 3: Document tag release usage**

Briefly mention annotated tags and that multi-line tag descriptions become Release notes.

## Chunk 5: Validate and publish

### Task 5: Verify repository state and push

**Files:**
- Modify: repository metadata only

- [ ] **Step 1: Build locally after rename**

Run:

```powershell
msbuild .\folder-size-ranker-cli.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64
```

Expected: successful build with renamed output binary.

- [ ] **Step 2: Inspect workflow files**

Confirm both `.github/workflows/ci.yml` and `.github/workflows/release.yml` reference the renamed project file and artifact path.

- [ ] **Step 3: Commit the changes**

Use a commit message such as:

```text
Add GitHub Actions and rename project
```

- [ ] **Step 4: Push to `origin/main`**

Push the implementation so GitHub Actions can pick up the workflows.
