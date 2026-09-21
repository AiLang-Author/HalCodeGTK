# HalBot — Open-Ended Web-API Agent (any backend drives it)

Status: **Draft v0.2** — design source of truth; Phase 1 code in progress.
Companion to: `HalCode9000.md`, `docs/architecture.md`, `docs/DESIGN_PGMEM.md`, `API.md`.

---

## 1. Decision

HalBot is **HalCode9000 running as a headless web-API agent** (`--serve` mode).
It is not tied to any one hosted bot product. **Any API backend can provide the
bot service** — a hosted bot (Grok, Claude, Cursor, ChatGPT, a custom skill), a
script, a GTK shell, or a phone — by speaking the same `/v1/chat` SSE surface
the desktop uses today.

What actually differs between one "bot" and another is only two things:

1. **Prompts** — the system/user prompt it is given (and, optionally, the
   provider+model routed per request).
2. **The local computer** — the machine HalBot is running on.

HalBot **owns its own machine**: the VM it boots in. On that machine it can
install packages, write and modify files, run builds, and generally
self-modify — **locally to itself**, never on the host. That is the point: the
bot has a disposable workspace it is free to change, and the agent loop's tools
(Bash, Write, Edit, Ailang compile, Packager, Git, …) already target that VM's
filesystem.

> "API via web, like halcode for the bot" — HalBot is to any driver what
> HalCode9000 is to the desktop: the same agent loop, tools, providers, and
> memory, behind a network API instead of a TUI/GTK front end.

**The shape of everything else is fine.** No rewrite of the agent loop, tool
dispatch, providers, or memory. This spec only adds (a) an HTTP surface and
(b) coordinator/worker task semantics on top of the existing sub-agent path.

### Provider-agnostic from the start

HalBot reuses the existing provider abstraction. `backends/Backend.ailang`
dispatches by `SelectedProvider.kind` (Anthropic/OpenAI/Gemini), and
`CC_SetupAgentProvider()` already maps a `provider[:model]` spec onto the
matching key (`ANTHROPIC_API_KEY`, `OPENAI_API_KEY`, `XAI_API_KEY`,
`DEEPSEEK_API_KEY`, `GEMINI_API_KEY`, or a local endpoint). So the same
`--serve` process can run behind any backend with no code change — the driver
just supplies (or the config defaults) the provider/model.

---

## 2. Terminology

| Term | Meaning |
|---|---|
| **HalBot** | `HalCode9000.x` running in `--serve` mode (web API). |
| **Driver** | Any HTTP client that talks to HalBot: hosted bot product, script, GTK shell, phone. |
| **Coordinator** | The HalBot main loop: decomposes work, assigns tasks, collects results. |
| **Worker** | A headless `HalCode9000.x --agent <provider>:<model>` child that runs one task and parks a result. |
| **Task** | A discrete work unit with a lifecycle (create → assign → run → park → end). |
| **Drive assignment** | See §5 — the semantics behind "coordinator/workers". |
| **The bot's machine** | The VM HalBot boots in — its own filesystem, owned and mutable locally. |

---

## 3. Reuse map (what HalBot is made of)

| HalBot concern | Existing component | Change |
|---|---|---|
| Agent loop (turn, tools, history) | `HalCode9000.ailang` `CC_RunTurn()` | none (reused via `--agent` child in Phase 1) |
| Web API surface | **NEW** `--serve` mode (mirrors `--mcp`) | add |
| Tool execution | `IPCDispatch.ailang` + 26 `cc_*_ipc.x` | none |
| Model access (any provider) | `backends/Backend.ailang` + `providers/*.json` + `CC_SetupAgentProvider()` | none |
| Persistent memory | Pgmem (Postgres) + Relmem (symbolic graph) | none |
| Sub-agent spawn | `cc_agent_ipc.x` (Agent tool) | small: provider/model from task routing |
| Task records | Pgmem `op=task_*` + `hc_tasks` (`DESIGN_PGMEM.md`) | wire |
| HTTP server | `Library.Socket` (raw syscalls) | add |
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

Request (provider/model optional — HalBot defaults per config):

```json
{
  "message": "Install ripgrep on this machine, then write a README noting it's done",
  "system_prompt": "optional override",
  "provider": "openai",
  "model": "gpt-4o",
  "approval": "auto"
}
```

Response (`Content-Type: text/event-stream`):

```
event: session
data: {"session_id":"sess_01J...","model":"gpt-4o"}

event: text_delta
data: {"text":"I'll start by checking the package manager."}

event: tool_call
data: {"tool":"Bash","id":"toolu_01...","args":{"command":"apt-get install -y ripgrep"}}

event: tool_result
data: {"tool":"Bash","id":"toolu_01...","ok":true,"truncated":false}

event: task_assigned
data: {"task_id":"t_17","title":"install ripgrep","assigned_to":"gpt-4o-mini","provider":"openai"}

event: task_complete
data: {"task_id":"t_17","result_key":"halbot:result:t_17","status":"complete"}

event: turn_done
data: {"stop_reason":"end_turn"}
```

The SSE emitter is a small new module (Phase 1 inlines it in `HalCode9000.ailang`);
the inbound SSE parser (`Library.SSE`) is reused unchanged for model streams.
The model stream inside the loop is already handled by `Backend.OnEvent`.

### Phase 1 scope (current)

- `--serve` boots tools + AgentProxy (same as `--host`), stays up, and serves HTTP.
- `GET /healthz` → 200.
- `POST /v1/chat` → runs the **existing agent loop** by spawning a `--agent`
  child for the requested `provider[:model]`, then relays its result as SSE
  (`session` → `text_delta` → `turn_done`). Single session, serialized turns.

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

## 6. Driver integration (any API backend)

HalBot is a generic HTTP+SSE backend. **Any driver** can use it; there is no
Grok-specific coupling. A driver integration is just:

1. A connector/skill/script that POSTs to `POST /v1/chat` (or the task queue).
2. A consumer that reads the SSE stream and renders deltas / tool calls /
   approvals in its own UI.

Mapping for a hosted-bot-style driver (Grok/Claude/Cursor/ChatGPT all fit this):

| Driver concept | HalBot equivalent |
|---|---|
| Connector | `--serve` HTTP endpoint registered as a connector action |
| Skill / routine | `skills/<domain>/SKILL.md` + an optional `cc_*_ipc.x` tool |
| Tool | one of the 26 `cc_*_ipc.x` tools, surfaced via `/v1/tools` |
| Approval / Auto Review | HalBot permission layer (§7) with an approval callback |
| Bot-to-bot coordination | HalBot coordinator → driver API as a peer, or driver polls `/v1/tasks` |

### Two supported topologies

**A. Driver drives HalBot (default).** The driver POSTs to `/v1/chat`, consumes
SSE, and surfaces approvals in its UI. HalBot is the executor of record.

**B. HalBot is the always-on coordinator.** HalBot runs its own task queue and
reaches out to other bots/drivers as peers. This is the "self-hosted always-on
agent" flavor.

The spec targets **A first** (smaller surface, matches "API via web"), with B
enabled by §5's coordinator loop already being the owner of the queue.

### Approval / Auto Review boundary

Auto-approved (read-only): Read, Head, LS, Grep, Find, Stat, Wc, Du, Diff,
Git read, Relmem query, Pgmem pickup/search, Skills read, Vision.

Require approval (mutating/expensive): Write, Edit, Undo, Bash, Git write,
Olympus commit, Pgmem park, MCP, Ailang compile, Packager build — unless the
session was opened with `"approval": "allowlist"` and the tool is listed.

Approval flows (order of preference):

1. **Callback** — HalBot POSTs an approval request to the configured driver
   webhook; blocks the tool until `approved|denied` returns (with timeout).
2. **Inline** — HalBot emits an `approval_required` SSE event and waits for the
   driver to POST `/v1/sessions/{id}/approvals`.
3. **Auto Review** — the driver's model-based review consumes the same
   `approval_required` event and applies its policy without a human.

---

## 7. Security / isolation (the bot's own machine)

- HalBot binds `127.0.0.1` by default; `--bind 0.0.0.0` is opt-in (remote drivers).
- Bearer token required; generated and stored in `~/.halcode/keys.env`.
- **The VM is the bot's machine.** Every tool operates on the VM's own
  filesystem. Install/modify/build operations (Bash, Write, Edit, Ailang
  compile, Packager, Git) are first-class and intentionally unrestricted *on
  that machine* — the bot is meant to be able to change its own environment.
- The boundary that matters is **VM ↔ host**. HalBot never escapes the VM:
  worker sandboxing (landlock/chroot) is a **Phase 5** hardening item; until
  then workers inherit the VM's permissions and the WSL2 RULES command
  blocklist applies on WSL hosts.
- Secrets live in env/`keys.env`, never in history or parked results.
- Per-session token budget and tool-call caps mirror the existing WSL2 chain limit.

---

## 8. Open questions — resolved (API integration dive)

1. **Connector loopback — NO loopback for hosted bots.** Hosted bot products
   reject `localhost`/private-IP URLs for custom connectors; the server must be
   reachable over the public internet. Remote drivers therefore need a
   **tunnel** (ngrok — Cloudflare quick tunnels do not support SSE). Deferred
   until core tech is hardened.
2. **VM reachability — not relevant yet.** A hosted bot's computer runs in its
   own cloud VM, not our VM. Remote/web/phone access is explicitly deferred
   until the core is secured and hardened; `--bind`/TLS decisions move to a
   later phase. HalBot's own machine (the VM it runs in) is local-first.
3. **Approvals — inline + Auto-review, no webhook required.** Hosted-bot
   approval is card-style (Allow once / Deny / Always allow) plus model-based
   Auto Review ("Ask first" vs "Allow automatically"; "Ask first" wins).
   HalBot's approval surface is therefore its own API (inline card + callback),
   with Auto Review as a model-based pre-flight gate.
4. **Worker model — fixed, configurable.** Worker provider/model was hardcoded
   (`--agent deepseek`). Now: per-call `model` arg on the Agent tool ->
   `HALCODE_WORKER` env var -> built-in `deepseek`. Any provider in
   `providers/*.json` is routable.
5. **TLS — loopback + bearer for Phase 1.** Remote access is deferred; TLS is a
   later hardening item, not Phase 1.

## 8b. Worker configuration (implemented)

- `cc_tools/cc_agent_ipc.ailang`: added optional `model` field; worker spec
  precedence is `model` arg -> `HALCODE_WORKER` -> `deepseek`.
- `HalCode9000.ailang`: added `CC_ResolveWorkerSpec()` (`HALCODE_WORKER` ->
  `deepseek`); MCP mode now uses it instead of hardcoded `"deepseek"`.
- `providers/xai.json`: refreshed to current xAI model list.
- **Build note (resolved):** the prior `ANSICanvas.SetPixel` 8-input > 6-register
  compile error was fixed in commit `94ac3ad` ("drop ANSICanvas import"). This
  section previously claimed it still blocked the build; that note is now stale
  and the main binary builds cleanly (596812 bytes).

---

## 9. Phase plan

| Phase | Deliverable |
|---|---|
| 0 | This spec (done, reframed v0.2) |
| 1 | `--serve` mode: `/healthz`, `/v1/chat` + SSE, single session, reused agent loop |
| 2 | Sessions: `/v1/sessions/*`, `/v1/tools`, auth |
| 3 | Task queue: `/v1/tasks`, coordinator routing, `hc_tasks` wiring |
| 4 | Driver connector (any backend) + approval callback |
| 5 | Sandboxing (VM↔host), multi-session/multi-bot coordination, TLS |

---

*Copyright 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.*
