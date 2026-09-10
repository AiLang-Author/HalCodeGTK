---
name: pgmem
description: Using the Pgmem tool (Postgres-backed memory). Covers all ops, correct usage patterns, and anti-patterns. Load this when working on any task that touches Pgmem or cross-agent context sharing.
---

# Pgmem — Postgres Memory Tool

Socket: `@halcode/Pgmem`  
Single-threaded daemon — one call at a time. Never hold the socket open across a wait.

---

## Session Lifecycle

Always start a session at the top of a new main-loop turn and end it on completion.

```
Pgmem op=session_start  role=main                          → session_id
Pgmem op=session_start  role=sub:reviewer  parent_id=<id>  → session_id
Pgmem op=session_end    session_id=<id>    auto_compact=1
```

---

## Parking and Retrieving Context

### Park (write)
```
Pgmem op=park  key=<label>  kind=<kind>  content=<text>
               [scope=session|project|persistent]
               [parent_key=<key>]
               [session_id=<id>]
```

**Kinds**: `decision`, `finding`, `plan`, `code`, `summary`, `note`, `todo`, `error`  
**Scope**: `session` (default, ephemeral), `project` (persists across sessions), `persistent` (never auto-compacted)

### Retrieve — RULE: use `search` for queries, `pickup` only for exact known keys

```
Pgmem op=search   query=<text>   [kind=...]  [scope=...]  [session_id=...]
Pgmem op=pickup   key=<label>    [session_id=<id>]
Pgmem op=tree     [session_id=<id>]  [scope=project]
```

**Do not use `pickup` as a general-purpose read** — it requires an exact key match and holds the socket while executing. Use `search` for anything discovery-oriented.

---

## Compaction

Atomically replaces a set of rows with a single summary row. Use to retire a completed work plan.

```
Pgmem op=compact  keys=<comma-separated-keys>  summary=<text>  kind=summary
                  [scope=persistent]
```

---

## Task Tracking

Create a task before spawning an Agent tool call — the task_id is mandatory for the agent daemon to call `task_end`.

```
Pgmem op=task_create  title=<...>  kind=<...>  effort=<low|medium|high>
                      [min_tier=1]  [parent_id=<id>]  [session_id=<id>]
                      → task_id

Pgmem op=task_start   task_id=<id>  assigned_to=<model_id>  provider=<name>
Pgmem op=task_end     task_id=<id>  status=<complete|failed>  [result_key=<key>]
                      input_tokens=<n>  output_tokens=<n>  cost_usd=<f>

Pgmem op=task_list    [session_id=<id>]  [status=pending]  [parent_id=<id>]
Pgmem op=task_get     task_id=<id>
```

---

## Anti-Patterns

- **Don't poll `op=pickup` in a loop** — while a pickup call is in-flight the socket is held, which blocks sub-agent daemons from calling `task_end`. Use `Sleep seconds=90` once, then check with `op=search` or `op=task_get`.
- **Don't use Pgmem for symbol queries** — use the Relmem tool directly (`op=symbols`, `op=where`, `op=query`, `op=callers`). Relmem owns `hc_files`/`hc_symbols`/`hc_symbol_edges` in the same postgres DB.
- **Don't park then immediately pickup** — park writes are async-friendly but the socket is still single-threaded. Do all parks first, then reads.
