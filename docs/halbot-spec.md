# HalBot — Web-API Agent for the Grok Bot VM

Status: **Draft v0.1** — design source of truth, code not yet written.
Companion to: `HalCode9000.md`, `docs/architecture.md`, `docs/DESIGN_PGMEM.md`, `API.md`.

---

## 1. Decision

Three directions were on the table for the Grok Bot integration:

1. **Computer-use tool** — expose HalCode9000's tools as a single computer-use
   call for the Grok Bot model (thin, but throws away the agent loop + memory).
2. **Self-hosted always-on agent** — run the full HalCode9000 agent as a
   persistent process in the Grok Bot VM, driven over an API.
3. **Connector model** — register HalCode9000 as a Grok Bot "connector" and let
   xAI's runtime own the loop.

**Chosen: (2), with (3) as the integration surface.** HalBot is the full
HalCode9000 agent running in the VM, exposed as a **web API** (`--serve` mode),
so the Grok Bot (or any HTTP client) drives it the same way the desktop drives
HalCodeGTK today. The Grok Bot side is wired up as a connector/skill that points
at the VM-local endpoint.

> "API via web, like halcode for the bot" — HalBot is to the Grok Bot what
> HalCode9000 is to the desktop: the same agent loop, tools, providers, and
> memory, behind a network API instead of a TUI/GTK front end.

**The shape of everything else is fine.** No rewrite of the agent loop, tool
dispatch, providers, or memory. This spec only adds (a) an HTTP surface and
(b) coordinator/worker task semantics on top of the existing sub-agent path.

---

## 2. Terminology

| Term | Meaning |
|---|---|
| **HalBot** | `HalCode9000.x` running in `--serve` mode (web API). |
| **Coordinator** | The HalBot main loop: decomposes work, assigns tasks, collects results. |
| **Worker** | A headless `HalCode9000.x --agent <provider>:<model>` child that runs one task and parks a result. |
| **Task** | A discrete work unit with a lifecycle (create → assign → run → park → end). |
| **Drive assignment** | See §5 — the semantics behind "coordinator/workers". |

---

## 3. Reuse map (what HalBot is made of)

| HalBot concern | Existing component | Change |
|---|---|---|
| Agent loop (turn, tools, history) | `HalCode9000.ailang` `CC_RunTurn()` | none |
| Web API surface | **NEW** `--serve` mode (mirrors `--mcp`) | add |
| Tool execution | `IPCDispatch.ailang` + 26 `cc_*_ipc.x` | none |
| Model access | `backends/Backend.ailang` + `providers/*.json` | none (`xai.json` already present) |
| Persistent memory | Pgmem (Postgres) + Relmem (symbolic graph) | none |
| Sub-agent spawn | `cc_agent_ipc.x` (Agent tool) | small: provider/model from task routing |
| Task records | Pgmem `op=task_*` + `hc_tasks` (`DESIGN_PGMEM.md`) | wire |
| HTTP server | `Library.HTTPServer` (or `Library.Socket`) | add |
| HTTP client (model API, callbacks) | `Library.HTTP` | none |

HalBot compiles to the same binary; `--serve` is a fourth mode alongside
`--host`, `--agent`, and `--mcp`.

---

## 4. Web API (`--serve` mode)

Launch:

```
./HalCode9000.x --serve [--port 8080] [--bind 127.0.0.1] [--token <t>] [provider[:model]]
```

Transport: HTTP/1.1 + Server-Sent Events (SSE). Auth: `Authorization: Bearer <token>`.

### Endpoints

| Method | Path | Purpose |
|---|---|---|
| GET | `/healthz` | liveness (200 `{"ok":true}`) |
| GET | `/v1/tools` | tool schemas (introspection for the driver) |
| POST | `/v1/chat` | create session + first message; returns `session_id` + SSE stream |
| POST | `/v1/sessions/{id}/messages` | append a message and run a turn |
| GET | `/v1/sessions/{id}/stream` | subscribe to an in-flight turn's SSE |
| GET | `/v1/sessions/{id}` | session snapshot (history + task list) |
| POST | `/v1/tasks` | enqueue a task (coordinator surface) |
| GET | `/v1/tasks/{id}` | task status/result |

### Example — `POST /v1/chat`

Request:

```json
{
  "message": "Review backends/Backend.ailang for the backend-kind dispatch bug",
  "system_prompt": "optional override",
  "provider": "xai",
  "model": "grok-3",
  "approval": "auto"
}
```

Response (`Content-Type: text/event-stream`):

```
event: session
data: {"session_id":"sess_01J...","model":"grok-3"}

event: text_delta
data: {"text":"I'll start by reading the file."}

event: tool_call
data: {"tool":"Read","id":"toolu_01...","args":{"path":"/home/bob/HalCodeGTK/backends/Backend.ailang"}}

event: tool_result
data: {"tool":"Read","id":"toolu_01...","ok":true,"truncated":false}

event: task_assigned
data: {"task_id":"t_17","title":"review backend-kind dispatch","assigned_to":"grok-3-mini","provider":"xai"}

event: task_complete
data: {"task_id":"t_17","result_key":"halbot:result:t_17","status":"complete"}

event: turn_done
data: {"stop_reason":"end_turn"}
```

The SSE emitter is a small new module (`Serve.ailang`); the inbound SSE parser
(`Library.SSE`) is reused unchanged. The model stream inside the loop is already
handled by `Backend.OnEvent`.

### Concurrency model

- One coordinator loop per HalBot process.
- Per-session turns are serialized (matching the single-threaded agent loop);
  a `POST .../messages` to a busy session returns `409` with a retry hint.
- Parallelism comes from **worker fan-out** (§5), not concurrent turns in one
  session. Multiple sessions are allowed; they share the coordinator queue.

---

## 5. Drive assignment semantics (coordinator / workers)

"Coordinator/workers" and "drive assignment" describe the same thing. The
former is the polite name; the latter is the semantics we actually want:

> A single controller assigns discrete work units to stateless executors,
> deterministically, without negotiation. Workers never self-select work; the
> controller drives the whole pipeline, like a disk controller mapping logical
> blocks to physical sectors.

Concretely:

1. **Decompose** — coordinator splits the goal into tasks (`kind`, `effort`).
2. **Enqueue** — each task becomes a record (`hc_tasks` / Pgmem `task_create`).
3. **Route** — assign provider+model by `tier`/`strengths`/`effort`
   (`DESIGN_PGMEM.md` routing; coordinator stays on flagship, workers use the
   cheapest capable model).
4. **Assign & run** — spawn `--agent` worker(s). Workers are leaf executors:
   pull context from Pgmem, run one turn, park the result, call `task_end`.
5. **Collect** — coordinator `pickup`s the `result_key`; the coordinator never
   reads a worker's raw transcript, only the parked result.

Hard rules (already enforced by the Agent tool):

- **One layer deep** — workers cannot spawn workers.
- **Stateless workers** — a worker owns one task; no shared mutable state except
  Pgmem/Relmem (the "disk" in the analogy).
- **Push assignment** — coordinator assigns; there is no pull/claim queue.
- **Bounded run** — worker is `timeout`-bounded (currently 300 s in
  `cc_agent_ipc`); coordinator records failure and may reassign.

### Task record (reuse `hc_tasks` from `DESIGN_PGMEM.md`)

```
id, project_id, session_id, parent_id, title, kind, effort, min_tier,
assigned_to, provider, status(pending|running|complete|failed),
result_key, input_tokens, output_tokens, cost_usd, started_at, ended_at
```

The only addition over `DESIGN_PGMEM.md` is a `queue_order`/priority hint for
the drive-assignment loop.

### Parallel fan-out

The coordinator may spawn N workers in one turn (existing Agent tool pattern),
then wait and collect. This is where the drive semantics pay off: N tasks are
assigned in one batch, results land in N Pgmem keys, and the coordinator
reassembles them in task order.

---

## 6. Grok Bot integration

Grok Bot = xAI's hosted agent product (Cursor/Anysphere cloud), per-member
persistent VM (browser + fs + terminal), connectors, skills/routines,
approvals/Auto Review, bot-to-bot coordination.

HalBot appears to Grok Bot as **one connector** (or one skill) whose action is
"talk to the local HalBot API". Mapping:

| Grok Bot concept | HalBot equivalent |
|---|---|
| Connector | `--serve` HTTP endpoint registered as a connector action |
| Skill / routine | `skills/<domain>/SKILL.md` + an optional `cc_*_ipc.x` tool |
| Tool | one of the 26 `cc_*_ipc.x` tools, surfaced via `/v1/tools` |
| Approval / Auto Review | HalBot permission layer (§7) with an approval callback |
| Bot-to-bot coordination | HalBot coordinator → Grok Bot API as a peer, or Grok Bot polls `/v1/tasks` |

### Two supported topologies

**A. Grok Bot drives HalBot (default).** Grok Bot POSTs to `/v1/chat`, consumes
SSE, and surfaces approvals in its UI. HalBot is the executor of record.

**B. HalBot is the always-on coordinator.** HalBot runs its own task queue and
reaches out to Grok Bot (or other bots) as peers via the Grok Bot API. This is
the "self-hosted always-on agent" flavor.

The spec targets **A first** (smaller surface, matches "API via web"), with B
enabled by §5's coordinator loop already being the owner of the queue.

### Approval / Auto Review boundary

Auto-approved (read-only): Read, Head, LS, Grep, Find, Stat, Wc, Du, Diff,
Git read, Relmem query, Pgmem pickup/search, Skills read, Vision.

Require approval (mutating/expensive): Write, Edit, Undo, Bash, Git write,
Olympus commit, Pgmem park, MCP, Ailang compile, Packager build — unless the
session was opened with `"approval": "allowlist"` and the tool is listed.

Approval flows (order of preference):

1. **Callback** — HalBot POSTs an approval request to the configured Grok Bot
   webhook; blocks the tool until `approved|denied` returns (with timeout).
2. **Inline** — HalBot emits an `approval_required` SSE event and waits for the
   driver to POST `/v1/sessions/{id}/approvals`.
3. **Auto Review** — Grok Bot's Auto Review consumes the same `approval_required`
   event and applies its policy without a human.

---

## 7. Security / isolation (VM boundary)

- HalBot binds `127.0.0.1` by default; `--bind 0.0.0.0` is opt-in (remote drivers).
- Bearer token required; generated and stored in `~/.halcode/keys.env`.
- Worker sandboxing (landlock/chroot) is a **Phase 5** item; until then workers
  inherit the VM's permissions and the WSL2 RULES command blocklist.
- Secrets live in env/`keys.env`, never in history or parked results.
- Per-session token budget and tool-call caps mirror the existing WSL2 chain limit.

---

## 8. Open questions — resolved (xAI docs dive)

1. **Connector loopback — NO loopback.** Grok rejects `localhost` and private-IP
   URLs for custom MCP connectors; the server must be reachable over the public
   internet. Grok -> HalBot therefore needs a **tunnel** (ngrok — Cloudflare
   quick tunnels do not support SSE). Deferred until core tech is hardened.
2. **VM reachability — not relevant yet.** Grok Bot's computer runs in Cursor's
   cloud, not our VM. Remote/web/phone access is explicitly deferred until the
   core is secured and hardened; `--bind`/TLS decisions move to a later phase.
3. **Approvals — inline + Auto-review, no webhook.** Grok Bot approval is
   **Allow once / Deny / Always allow** cards in conversation, plus **Auto
   Review** rules ("Ask first" vs "Allow automatically"; model-based; "Ask
   first" wins). No outbound approval webhook exists. HalBot's approval surface
   is therefore its own API (inline card + callback), with Auto Review as a
   model-based pre-flight gate.
4. **Worker model — fixed, configurable.** Worker provider/model was hardcoded
   (`--agent deepseek`). Now: per-call `model` arg on the Agent tool ->
   `HALCODE_WORKER` env var -> built-in `deepseek`. `grok-3`/`grok-3-mini` are
   retired; `providers/xai.json` now lists grok-4.6 / grok-4.5 / grok-4.3 /
   grok-4.20-{reasoning,non-reasoning,multi-agent} / grok-code-fast-1 (default
   `grok-4.6`).
5. **TLS — loopback + bearer for Phase 1.** Remote access is deferred; TLS is a
   later hardening item, not Phase 1.

## 8b. Worker configuration (implemented)

- `cc_tools/cc_agent_ipc.ailang`: added optional `model` field; worker spec
  precedence is `model` arg -> `HALCODE_WORKER` -> `deepseek`.
- `HalCode9000.ailang`: added `CC_ResolveWorkerSpec()` (`HALCODE_WORKER` ->
  `deepseek`); MCP mode now uses it instead of hardcoded `"deepseek"`; Grok
  default bumped `grok-3` -> `grok-4.6`.
- `providers/xai.json`: refreshed to current xAI model list.
- **Build note:** `cc_agent_ipc.ailang` compiles cleanly. `HalCode9000.ailang`
  currently fails with a pre-existing error unrelated to these edits
  (`ANSICanvas.SetPixel`/`ANSICanvas_SGR` have 8 inputs > SysV 6-register
  limit); it blocks rebuilding `HalCode9000.x` and must be fixed separately.

---

## 9. Phase plan

| Phase | Deliverable |
|---|---|
| 0 | This spec (done) |
| 1 | `--serve` mode: `/healthz`, `/v1/chat` + SSE, single session, reused agent loop |
| 2 | Sessions: `/v1/sessions/*`, `/v1/tools`, auth |
| 3 | Task queue: `/v1/tasks`, coordinator routing, `hc_tasks` wiring |
| 4 | Grok Bot connector + approval callback |
| 5 | Sandboxing, multi-session/multi-bot coordination, TLS |

---

*Copyright 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.*
