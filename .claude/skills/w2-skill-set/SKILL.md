---
name: w2-skill-set
description: Use when the same kind of Witcher 2 Engine Fix work happens a second time and you are deciding whether it belongs in a project skill, an AGENTS.md rule, docs/ or personal notes; also use when creating, changing, merging or deleting anything under .agents/skills, .claude/skills or .claude/agents.
---

# Growing and pruning the project skill set

The general method is the user-level `project-skill-set` skill; read it when it is available. This
file adds what is specific to this repository.

## Where knowledge goes

| It is... | Goes to |
| --- | --- |
| a fix: symptom, addresses, proof, off switch | `docs/fixes/<topic>.md`, listed in `docs/fixes/README.md` |
| a fact about the game or its formats | `docs/research.md` |
| an address the DLL uses | a `#define` in `src/`, then `docs/addresses.md` by `tools/address-table.py` |
| output from a run in the game | `docs/validation/` |
| a rule a later edit must not break | `AGENTS.md` ("Rules") |
| something a script can check | `tools/check-repo.ps1` |
| machine-specific, or plans, status, handoffs and Nexus drafts | `docs/local/` or `docs/plans/`, both git-ignored |
| a repeating role | a skill here |

Promote on the second occurrence. A skill that restates facts is a second, worse `AGENTS.md`.

## Two trees, kept identical

1. Edit only `.agents/skills/<name>/SKILL.md`.
2. Run `tools\sync-agent-skills.ps1`; `-Check` exits 1 on drift, and `tools\check-repo.ps1` runs it.
3. Add the row to "Specialist passes" in `AGENTS.md`.
4. End every skill with a delegation prompt.

## Cap and pruning

Six or seven skills. Merge overlaps, and delete what became a check in `tools/check-repo.ps1` or
stopped being true.

## Delegating this pass

Spawn a subagent with: "You are the skill-set reviewer for Witcher 2 Engine Fix. Load the
`w2-skill-set` skill, read .agents/skills, .claude/agents and AGENTS.md, and run
tools\sync-agent-skills.ps1 -Check. Report skills that restate facts, overlap, lack a trigger
sentence, are missing from AGENTS.md, or differ between the trees. Do not edit or commit."
