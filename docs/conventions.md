# Conventions

Same conventions as [witcher-enginefix](https://github.com/MariusTBjercke/witcher-enginefix).

## Commits

Plain, descriptive subject lines in English; no Conventional Commits prefixes. A
release commit starts with the version and lists what it contains. Examples:

- `0.1.0: save previews, graphics presets, distance hooks`
- `Save previews: write the late 640x360 capture through Steam storage`
- `research: save capture/save race in 009B63DE`

One finished fix or plan item is one commit. Work on `main`; use a short-lived
branch only for an experiment that may be thrown away.

## Versions and releases

- Semantic versions. The version lives in two places that must match:
  the top `## x.y.z` heading of `CHANGELOG.md` and `ENGINEFIX_VERSION` /
  `ENGINEFIX_VERSION_BIN` in `src/version.h`. `tools/package-release.ps1` refuses to package if they differ.
- Tag `v<version>`; GitHub release title `Witcher 2 Engine Fix <version>`, with
  `dist/Witcher2EngineFix-<version>.zip` attached. The zip extracts straight into
  the game folder (`bin/dinput8.dll`, `bin/witcher2_enginefix.ini`, plus
  `README_Witcher2EngineFix.txt` and `LICENSE_Witcher2EngineFix.txt`).
- Release notes: what the player notices, how to turn each part off, how to
  upgrade, and the exact game version tested.
- Nexus starts at 0.5.0 with one cumulative initial changelog entry. Earlier
  GitHub versions remain in `CHANGELOG.md`; do not create retrospective Nexus
  releases for them. Nexus page and file versions must match the uploaded zip.

## Fixes and evidence

- Every runtime patch gets a `docs/fixes/` entry: symptom, addresses, original
  bytes or calls, how it was verified, how to turn it off.
- In-game results and probe output go to `docs/validation/`.
- Plans and status: `docs/plans/progress.json`.

## Language

English in code, comments, docs and commit messages. Chat with the owner in
Norwegian.
