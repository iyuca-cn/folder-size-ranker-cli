# Release Run Failure Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking. Do not create commits unless the user explicitly asks.

**Goal:** Fix the Release workflow failure on GitHub Actions without weakening the annotated-tag validation rules.

**Architecture:** Replace the fragile custom `ProcessStartInfo` git wrapper with a simpler `Start-Process` + redirected output file flow. Keep the existing annotated-tag and non-empty-note validation, then pass the exact note text into the release action body through `GITHUB_OUTPUT`.

**Tech Stack:** GitHub Actions YAML, PowerShell 7 (`pwsh`), Git.

---

## Chunk 1: Workflow fix

### Task 1: Simplify tag note extraction

**Files:**
- Modify: `.github/workflows/release.yml`

- [ ] Remove the `Get-GitOutput` helper based on `ProcessStartInfo`.
- [ ] Keep `git cat-file -t` for annotated-tag validation.
- [ ] Replace note extraction with `Start-Process -RedirectStandardOutput`.
- [ ] Read the redirected file back as full text.

### Task 2: Preserve release-body semantics

**Files:**
- Modify: `.github/workflows/release.yml`

- [ ] Keep failure behavior for lightweight tags.
- [ ] Keep failure behavior for empty notes.
- [ ] Keep multi-line `GITHUB_OUTPUT` body emission.
- [ ] Leave release title and artifacts unchanged.

## Chunk 2: Docs and verification

### Task 3: Update docs only if behavior text changes

**Files:**
- Modify: `README.md`

- [ ] Ensure README still matches the annotated-tag requirement.
- [ ] Avoid expanding scope beyond release behavior.

### Task 4: Validate locally

**Commands:**
- Run: `git for-each-ref "refs/tags/v1.0.1" --format="%(refname:short)|%(objecttype)|%(contents)"`
- Run: `git status --short`

**Expected:**
- Annotated tag metadata remains readable.
- The workflow no longer contains the fragile custom process wrapper.
