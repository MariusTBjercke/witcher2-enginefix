---
name: w2-release
description: Use when preparing a Witcher 2 Engine Fix release - bumping the version, building the zip, the GitHub release, and updating the Nexus page text and changelog. Not for deciding whether a change is ready (w2-review).
---

# Release

A release goes to two places that must agree, and a script in the zip or a mismatch in version
gets caught late. This role keeps the steps in order.

Read first: `AGENTS.md` ("Build, install, release"), `docs/conventions.md` ("Versions and
releases") and, when present, the local `docs/local/nexus.md`.

## Method

1. **Check it is ready.** `w2-review` has passed, the owner has run the build in game, and
   anything unproven is named in the notes.
2. **Version.** The top heading of `CHANGELOG.md` and both values in `src/version.h` move together.
   If patched scripts changed, check `OV_CACHE_LAYOUT`.
3. **Player text.** Update `docs/README.txt`, `README.md` and the settings comments.
4. **Package.** `tools\package-release.ps1` builds the zip and refuses a version mismatch or a
   script in the zip. Record the size and SHA-256 locally.
5. **Publish, on the owner's word.** Commit, tag, push and `gh release create`. Each of these is
   outward-facing: ask first, even when the previous release went the same way.
6. **Nexus.** The owner uploads. Prepare the checklist in `docs/local/nexus.md`: new main file, the
   previous one archived, Version field, changelog entry, description when behaviour changed.
7. **Afterwards.** Record the virus scan result locally. Replies to players are drafted in
   `docs/local/`, and the owner posts them.

Drafts, replies, hashes and upload notes stay in `docs/local/`, which is git-ignored.

## Done when

- `tools\check-repo.ps1` passes on the tagged commit.
- The zip's version, the tag, the GitHub release and the Nexus page all match.
- The Nexus scan result is recorded.

## Delegating this pass

Spawn a subagent with: "You are the release checker for Witcher 2 Engine Fix. Load the `w2-release`
skill and check that <version> is ready: version fields, changelog, README text and zip contents.
Report what is missing. Do not edit, tag, push or publish."
