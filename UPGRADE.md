# HAL Upgrade Notes

Short list of the three suggestions discussed, plus one known
shortcoming. The suggestions are additive, none change the on-disk model
architecture, and total growth stays well under 3MB (not counting
Postgres, which is the point of using it).

---

## 1. Pgmem as the session layer (mostly free — already half-built)

The JS/TS "session management song and dance" is ~70% a durable event log
and ~30% a typed in-process event bus. Postgres gives the 70% for free:

- Durability/ACID — history is a row/JSONB, WAL-backed, survives process death.
- Fork/resume/branch — fork = `INSERT...SELECT` new id, branch = `parent_id` col.
- Migrations = `ALTER TABLE`.
- Search = Postgres FTS (already exposed as `Pgmem op=search`).
- Replay/audit = append-only log + sequence column.

Already live in code: `CC_CompactSession` parks full history to
`session:<sid>:snapshot:<ts>`, and the session layer parks
`session:<id>:history` + `:meta`. So the event-log-in-a-real-DB pattern is
running, not hypothetical.

What Pgmem does NOT buy (and would still need building regardless of
substrate): the typed event bus + reversible effects (the dsh-style
undo/redo stack).

**Action:** formalize the session schema as append-only rows, expose fork /
branch / resume as first-class ops, keep the typed event bus in-process.

---

## 2. Compaction: make the cap data, not code

Current state (as implemented):

- No automatic *semantic* compaction — correct decision, keep it.
- `History.ailang` has a message ring: `MAX_MESSAGES = 10`, drops the
  oldest whole turn, bundles `tool_use`/`tool_result` pairs so a tool call
  is never orphaned.
- `CC_CompactSession` is manual snapshot+reset (park to Pgmem, clear ring,
  leave a `[compaction]` marker) — not summarization.
- No local tokenizer. Token counts are read from the API usage block
  *after* the call, for display only.

The gap: **`MAX_MESSAGES = 10` is token-blind.** Ten messages with a big
image or a 50KB tool dump can blow a small context window on one of the
smaller backends. "Let the model API handle it" is only half true — the API
doesn't truncate for you, it errors (`context_length_exceeded`).

Fix that keeps the "no per-model rules" stance:

- Add one `context_window` number per provider JSON (one integer, not N
  rules).
- Evict oldest-turn-until the sum of the API's own token counts fits under
  it.
- Keep semantic compaction manual/on-demand — that's the part that would
  actually need per-model rules (summarizer prompt/quality).

---

## 3. Hot-swappable agent loop via the LinuxCNC microkernel pattern

`HAL_Microkernel.ailang` already has every primitive:

- Supervisor daemon owns the mailbox (`ShmLayout` offsets in
  `/tmp/hal_pins.shm`).
- Config-driven registry (named pins/programs, not hardcoded).
- `ProcessFork` / `ProcessExec` / `ProcessKill` / `ProcessWait`.
- `StopAll` / `StartAll` — the skeleton of a swap.

Mapping onto HAL:

| LinuxCNC | HAL |
|---|---|
| kernel daemon | core / supervisor |
| `/tmp/hal_pins.shm` (volatile, flat) | Pgmem (durable, transactional, queryable) |
| `PinRegistry` / `ProgramRegistry` | tool registry + event bus |
| a "program" (child process) | agent loop as a supervised child |
| `StopAll` -> `StartAll` | `ProcessKill(loop_pid)` -> `ProcessExec(new_loop)` |

The swap works because **state lives in the mailbox, not the loop's
stack**: kill the loop, exec a new binary, it reads
`session:<id>:history` from Pgmem and continues. As long as the new loop
speaks the same contract (inbox: message + session id; outbox: next action
/ tool call / turn-done), it can be a different backend, strategy, or even
language.

Already doing this for workers: `Agent` spawns a linked process that pulls
from Pgmem and parks a result; `Sleep` + `Pgmem op=poll` is the mailbox
protocol. The microkernel demo is the same pattern in raw AILang syscalls.
The only remaining leap is promoting the *loop* from compiled-in to a
supervised child with a defined seam.

**Action:** swap at turn/step boundaries, never mid-tool-call.
`StopAll`/`StartAll` becomes stop-one/start-one.

---

## 4. Known shortcoming: the value is gated on the model driving it

HAL is *AI-native*: it keeps getting smaller because decisions get pushed
out to the model ("the model will figure out the key convention, read the
mailbox contract, drive the event bus"). That is the source of the 3MB
elegance, but it is also a single point of failure — the whole system is a
bet on model capability, not on the harness.

Consequences:

- **On a frontier model** the bet pays off: less scaffolding, more leverage.
- **On a mid/weak model** the same design reads as *broken*, because there
  is no deterministic layer to catch the model when it cannot drive the
  seams. A user with weak local models sees a tool that "does nothing"
  even though the tool is fine.
- **AILang's bespoke syntax compounds it.** Models are saturated on JSON
  and Python but start cold on a novel grammar, so they lean on few-shot
  generalization that only strong models have. The language is written for
  a capability tier that is one generation ahead of what most people can
  run locally.

The second-order effect: nobody clones/stars it *because they can't feel
the value through the model they have*, and the author barely uses it for
the same reason. It is not a marketing problem and not a code problem; it
is a timing problem.

Two ways to lower the floor, each a real tradeoff:

1. **Add a thin deterministic layer** that wraps the parts requiring model
   cleverness (event-bus driving, key choice, mailbox contract) so a mid
   model only does the easy 20%. This is the opposite of the current
   philosophy — it adds back the scaffolding the design deliberately
   removed.
2. **Lead with the thesis, not the syntax.** Position HAL as a *platform
   demonstrator* for what AI-native systems look like when state lives in
   a mailbox and the harness stays out of the way — not as a Cursor
   competitor. The README should answer the question nobody knows to ask
   yet, rather than only showing that the language compiles.

Positioning note: HAL was built as a platform demonstrator, not a
dedicated competitor to Cursor. It is arguably superior to most API
harnesses precisely because it is *not* a blob — it is a small set of
composable primitives with strict contracts, and the contracts (not the
size) are the point.

---

## 5. Closing the model-support gap: the deterministic layer, gated

The gap is not "weak models can't reason" — it is two specific, fixable
deficits: (1) they cannot recover from their own errors, and (2) they
cannot hold implicit state across a long agentic loop. Every piece below
targets one of those, and almost all of it already exists in HAL in some
form. The through-line is the LinuxCNC principle applied to the model
itself: *make the environment do the bookkeeping so the program can be dumb
and disposable.*

None of this needs to bloat the binary. The small size was never a target —
2.3MB just happened, organically; AILang binaries are always shockingly
small. A few KB of scaffolding is not a real cost, and gating it behind a
capability profile (5.4) means the frontier path stays exactly as lean as
it is now.

### 5.1 Toolchain-in-the-loop — verifier as oracle (highest leverage, near-free)

HALCode is not AILang-specific. AILang is the *design center* — a systems
language built for LLM/human pair programming, with long, explicit
keywords tuned for next-token convergence rather than human terseness —
but the *mechanism* dispatches to whatever toolchain the file demands.
The novel-syntax gap closes if model output is wired through the
*relevant* verifier as *feedback, not as a gate*:

```
model emits code
  -> pick verifier by file type (toolchain registry)
  -> errors -> return "[<tool>] <errors>" as the next tool result
  -> model retries (bounded N=3)
```

The registry is **data, not special cases** — one table mapping file type
to verify / build / error-format. Every language is just an entry:

```
.ailang  -> analyzer.x            | ailang.x
.c .h    -> gcc -fsyntax-only     | gcc
.py      -> python -m py_compile  | ruff / mypy
.rs      -> rustc --emit=metadata | cargo build
.go      -> go vet                | go build
.zig     -> zig ast-check         | zig build
.sh      -> bash -n               | shellcheck
.ts .js  -> tsc --noEmit          | esbuild
.json    -> jq empty              | (schema validator)
```

AILang is the *reference entry*: it is built for exactly this loop, so its
analyzer returns structured diagnostics the model corrects against
directly. The registry stays generic in mechanism — no language is
hardcoded — but AILang is the one this feature exists to serve first;
gcc/clang/rustc ride along on the same generic path. `analyzer.x` /
`ailang.x` are entry #1, not a hardcoded branch.

### Registry schema

Each entry declares four things: what to match, how to verify, how to
build, and how to normalize its errors. The loop never parses a tool's
output itself — the entry's `errors` block reduces whatever the tool emits
to one uniform tuple fed back to the model.

```json
{
  "toolchain": [
    {
      "id": "ailang",
      "match": ["*.ailang"],
      "verify": {
        "cmd": ["analyzer.x", "{file}"],
        "timeout_ms": 5000
      },
      "build": {
        "cmd": ["ailang.x", "{file}", "-o", "{out}"],
        "produces": "{out}"
      },
      "errors": {
        "style": "structured",
        "line": "line", "col": "col",
        "severity": "level", "message": "message"
      }
    },
    {
      "id": "gcc",
      "match": ["*.c", "*.h"],
      "verify": { "cmd": ["gcc", "-fsyntax-only", "{file}"] },
      "build":  { "cmd": ["gcc", "{file}", "-o", "{out}"], "produces": "{out}" },
      "errors": {
        "style": "regex",
        "regex": "^([^:]+):(\\d+):(\\d+):\\s*(error|warning|note):\\s*(.+)$",
        "groups": { "file": 1, "line": 2, "col": 3, "severity": 4, "message": 5 }
      }
    }
  ]
}
```

Contract, regardless of `style`:

- **`match`** — glob list; first matching entry wins.
- **`verify`** — fast feedback path; runs on every emitted chunk before
  retry. Non-zero exit = compiler findings (feed back); timeout/crash =
  tool failure (feed back differently, don't loop on it).
- **`build`** — only when the model asks to compile, or after N clean
  verifies.
- **`errors`** — normalization. `structured` reads named fields straight
  off the tool's JSON/parse-tree output; `regex` extracts them from
  line-oriented text. Both reduce to the same tuple.

The uniform tuple fed back to the model:

```
[tool:line:col] severity: message
```

AILang's analyzer already emits `structured`, so its entry skips regex
entirely; gcc/clang/rustc/tsc use `regex`. Adding a language is one row in
this table — no code. (Field names in the `structured` block are
illustrative; they map to whatever the analyzer actually prints.)

Weak models recover from a concrete parse/type error far better than they
avoid producing one. The same trick applies to tool-call JSON: validate
against the schema, and on violation, hand the schema error back as the
tool result instead of crashing. This turns "model emits garbage" from a
hard failure into a self-correcting loop. Cheapest, most transformative
change in the list.

### 5.2 Convention projection — kill the guesswork

Today a model has to *infer* `chat:<proj>:<thread>:turn:<N>`,
`project_map`, and the seq-counter dance. Frontier models figure it out;
weak models don't. Fix: at prompt-build time, inject a deterministic header
with the active keys, session id, project, thread, and seq counter. The
model is *handed* the conventions rather than rediscovering them. No new
primitives — just a template rendered into the context.

### 5.3 One-step horizon — plan as mailbox state

The single biggest lever for weak agents: stop asking the model to hold the
plan in context. Move the plan into the mailbox (`plan` object: goal,
ordered steps, current index). The harness projects a status line —
goal / done / next — and the model only emits *the next action*. Its
planning horizon shrinks from "the whole task" to "one step." Same
principle as Section 3 ("state lives in the mailbox, not the loop's
stack"), now applied to *planning* instead of *session history*.

### 5.4 Capability profile — the scaffolding dial

The objection to all of the above is that it is precisely the scaffolding
the 3MB elegance deliberately removed. The answer: make it **conditional**.
One small profile object per provider — same spirit as Section 2's "one
integer, not N rules":

```json
{
  "tool_discipline": "strict",
  "follows_grammar": false,
  "planning": "harness",
  "context_window": 8192,
  "retries": 3
}
```

The harness reads the profile and turns layers on/off. **Frontier model →
raw path, zero overhead, elegance intact. Weak model → full deterministic
wrappers.** This is the move that lets HAL close the gap *without* becoming
the blob — the scaffolding only exists when the model needs it.

### 5.5 Bounded recovery + escalation

Every tool error becomes a structured result → bounded retries with
escalating context → hard fallback (a deterministic default, or escalate to
a stronger backend). This kills the death spiral where a weak model emits
an error, then doubles down on the error.

---

**What this actually does:** it doesn't make a weak model smarter. It
reduces the *required intelligence per step* — the environment absorbs the
bookkeeping, the conventions, the plan, and the error recovery, leaving the
model a much narrower job. Same bet as the rest of HAL, pushed one layer
further down the stack.

**What it costs:** it is the deterministic layer from Section 4's tradeoff,
but gated behind the capability profile so the frontier path stays lean.
HAL stops being *only* a platform demonstrator for strong models and
becomes a *spectrum* — raw for the strong, scaffolded for the weak, same
contracts throughout.

**Action:** build 5.1 first — a data-driven toolchain registry plus one
generic `run-verifier -> feed-errors-back -> retry` loop, with `analyzer.x`
as the first registered entry rather than a hardcoded path. Then 5.2 (a
template, not a feature), then 5.3 (mailbox object + status projection).
5.4 is the one knob that keeps all of it from becoming mandatory. 5.5
falls out of 5.1's retry bound.
