# pgmem / relmem → closed-source plugin

> Status: **plan — nothing executed yet.** Check items off as they land.
> Companion to `docs/repo-cleanup-plan.md` (decisions #1, #6, #7).
> Goal: pgmem + relmem become a saleable closed-source plugin; this repo stays
> clean MIT with no schema, no op lists, no "how to use" copy.

---

## Verified current state

Facts checked against the tree, not assumed:

| Fact | Evidence |
|---|---|
| One source copy each | `cc_tools/cc_pgmem_ipc.ailang`, `cc_tools/cc_relmem_ipc.ailang` |
| Sources are gitignored and were **never** tracked | `.gitignore:2-4`; `git ls-files` → no `.ailang` hits |
| SCSL is **one line** per file | pgmem line 32, relmem line 26 (`Copyright 2026 Sean Collins, 2 Paws Machine and Engineering. SCSL.`) |
| Binaries are tracked and public | `cc_pgmem_ipc.x`, `cc_relmem_ipc.x` in `git ls-files` |
| Binaries embed **no** license string | `strings -a *.x \| grep -i -e scsl -e "2 Paws"` → none |
| Docs already leaked to public HEAD | `docs/DESIGN_PGMEM.md`, `skills/halcode/pgmem/SKILL.md`, `skills/halcode/relmem/SKILL.md` all tracked |
| Repo already pushed | `origin` = `github.com/AiLang-Author/HalCodeGTK.git`, `main` = `4b3692b`, in sync |
| `.bak` gap | `.gitignore` covers `cc_pgmem_ipc.ailang.bak` but **not** the relmem one |
| README advertises both | lines 16 and 37 |

Consequence: the header rewrite is trivial (a line each). The real work is
Phase 2 — the design doc and two SKILL.md files were committed in `645d19c`
and are already on GitHub.

---

## Phase 0 — pre-flight

- [ ] Add `cc_tools/*.ailang.bak` to `.gitignore` (relmem `.bak` is currently uncovered; an editor backup of the ops list would be stageable)
- [ ] Confirm both tools still run from local builds: `./cc_tools/cc_pgmem_ipc.x`, `./cc_tools/cc_relmem_ipc.x`
- [ ] Snapshot before touching headers (Olympus snapshot, so the one-line edits are revertable)

## Phase 1 — proprietary header (local only, never committed)

Replace the single SCSL line in each file with:

```
// Copyright 2026 Sean Collins, 2 Paws Machine and Engineering.
// All rights reserved. Proprietary and confidential.
// Not licensed for redistribution. Commercial license available on request.
```

- [ ] `cc_pgmem_ipc.ailang` line 32 → block above
- [ ] `cc_relmem_ipc.ailang` line 26 → block above
- [ ] Keep the descriptive comment block above it in **pgmem** — it documents the wire ops; it belongs in the plugin repo, not in a public diff
- [ ] Header-only change: re-run `./analyzer.x` on both, expect pass, no rebuild needed
- [ ] Verify: `grep -c SCSL` → 0 on both files

## Phase 2 — stop the docs bleed from HEAD

- [ ] `git rm docs/DESIGN_PGMEM.md skills/halcode/pgmem/SKILL.md skills/halcode/relmem/SKILL.md`
- [ ] Move those three files to the plugin repo **before** deleting on disk (they are the design + usage docs you are selling)
- [ ] Strip README lines 16 and 37 (drop pgmem/relmem from the optional-deps and setup copy)
- [ ] Audit `docs/architecture.md` and `docs/qol-roadmap.md` for pgmem/relmem feature copy
- [ ] Verify: `git ls-files | grep -iE 'pgmem|relmem'` → only the two `.x` files

## Phase 3 — decide what to do about history

The docs are public at `645d19c` and pushed. Deleting from HEAD does not unpublish.

- [ ] Decision:
  - **(a) Accept** — recommended. Sources never leaked; only design prose did. Removing HEAD copy stops future readers.
  - (b) Rewrite history (`git filter-repo`) + force-push — breaks existing clones, GitHub may still serve cached objects.
- [ ] If (b): force-push, then verify with a fresh clone that the paths are gone
- [ ] Record the decision here so it is not re-litigated

## Phase 4 — plugin repo

- [ ] Create private repo (e.g. `AiLang-Author/halcode-plugins`), or `Packager/`-style subtree
- [ ] Land: both sources, `LICENSE-PROPRIETARY`, `build.sh`, the three docs from Phase 2
- [ ] `cc_tools.json` registration stays in the public repo (it names the tool, not the source)
- [ ] Install path documented **only** in the plugin repo (decision #7: public README does not advertise)
- [ ] Decide licensing mechanics for the binaries (per-machine key? signed build? plain honor system to start?)

## Phase 5 — binaries (only when actually selling)

- [ ] `git rm --cached cc_pgmem_ipc.x cc_relmem_ipc.x`, add to `.gitignore`
- [ ] Publish them as release artifacts instead
- [ ] Verify a clean clone still builds/runs after the plugin install step

---

## Out of scope / never in this repo

- DB schema, SQL, table names
- The op list and wire protocol
- "How to use pgmem" copy of any kind
- This plan mentions no schema detail on purpose — keep it that way if it is committed
