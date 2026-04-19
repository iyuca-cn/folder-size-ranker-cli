# Release Tag Notes Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking. Do not create commits unless the user explicitly asks.

**Goal:** Make GitHub Release bodies exactly match annotated tag notes and fail fast for lightweight or empty-note tags.

**Architecture:** Replace the current file-based fallback flow with a validation step that proves the pushed ref is an annotated tag, reads the full tag note text, and passes that exact multi-line content directly to `softprops/action-gh-release` as the release body. Keep the release title unchanged and update README to document the annotated-tag requirement.

**Tech Stack:** GitHub Actions YAML, PowerShell 7 (`pwsh`), Git.

---

## Chunk 1: Release workflow behavior

### Task 1: Validate annotated tags

**Files:**
- Modify: `.github/workflows/release.yml`

- [ ] Add an `id` to the tag-note step.
- [ ] Use `git cat-file -t` to verify `refs/tags/$env:GITHUB_REF_NAME` is a `tag`.
- [ ] Fail the workflow if the ref is not an annotated tag.

### Task 2: Pass exact tag notes to the release action

**Files:**
- Modify: `.github/workflows/release.yml`

- [ ] Read the tag note text without fallback to tag name.
- [ ] Fail the workflow if the note text is empty or whitespace.
- [ ] Write the note text to a multi-line step output.
- [ ] Change `softprops/action-gh-release` to use `body` instead of `body_path`.

## Chunk 2: Documentation and verification

### Task 3: Update release documentation

**Files:**
- Modify: `README.md`

- [ ] State that releases must use annotated tags.
- [ ] State that the GitHub Release body is taken from the full annotated tag note.
- [ ] State that lightweight tags will fail the release workflow.

### Task 4: Validate locally

**Commands:**
- Run: `Get-Content -Raw .github/workflows/release.yml`
- Run: `Get-Content -Raw README.md`

**Expected:**
- The workflow contains no fallback-to-tag-name behavior.
- The release action consumes the tag note text directly.
- The README matches the workflow behavior.
