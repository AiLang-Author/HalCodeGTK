---
name: agent-orchestration
description: Spawning and managing HalCode9000 sub-agents via the Agent tool. Covers the mandatory task_id pattern, safe wait strategy, deadlock avoidance, and result retrieval. Load this before any multi-agent workflow.
---

# Agent Orchestration

The `Agent` tool spawns a DeepSeek-v4-Flash worker via `HalCode9000.x --agent`. The worker has its own history, no terminal UI, and runs on the same backend pool. It reads context from Pgmem and parks results back to Pgmem.

---

## Mandatory Pattern

```
# 1. Create a task FIRST — task_id is required
Pgmem op=task_create  title="review auth.ailang"  kind=review  effort=low
→ task_id = "abc123"

# 2. Park context the sub-agent will need
Pgmem op=park  key="agent-input-abc123"  kind=code  content=<file contents>

# 3. Spawn the agent
Agent  task_id="abc123"
       task_prompt="Read the context at agent-input-abc123 and review it for XSHash collisions. Park your findings at agent-result-abc123."
       context_key="agent-input-abc123"
       result_key="agent-result-abc123"

# 4. Wait — ONLY with the Sleep tool, not Bash sleep
Sleep  seconds=90

# 5. Check ONCE — use search or task_get, never loop
Pgmem op=task_get  task_id="abc123"
# If still pending, Sleep once more then check again
```

---

## Rules

**task_id is mandatory.** Omitting it leaves the task stuck in `pending` forever — the agent daemon calls `task_end` using the task_id, so without it the task record is never closed.

**Sleep tool only.** `Sleep seconds=90` (the HalCode9000 Sleep tool) is the only safe wait. `Bash sleep` is blocked in the system prompt. Polling Pgmem in a loop causes a deadlock: each `pickup` call holds the single-threaded socket, which prevents the agent daemon from calling `task_end`.

**One layer deep.** Sub-agents cannot spawn their own sub-agents (fork bomb prevention). Design workflows so the parent orchestrates and sub-agents only do leaf work.

**Parallel fan-out.** You can spawn multiple agents before waiting:
```
Agent task_id="t1"  task_prompt="..."  context_key="ctx1"  result_key="res1"
Agent task_id="t2"  task_prompt="..."  context_key="ctx2"  result_key="res2"
Sleep seconds=90
Pgmem op=task_get  task_id="t1"
Pgmem op=task_get  task_id="t2"
```

---

## Retrieving Results

Use `op=search` not `op=pickup` for results unless you know the exact key:
```
Pgmem op=search  query="agent result review auth"  session_id=<current>
# or if you set a known result_key:
Pgmem op=pickup  key="agent-result-abc123"
```

---

## What Sub-Agents Can Do

- All cc_tools except Agent (no recursion)
- Read, Write, Edit, Grep, Find, Bash, Git, Pgmem, Relmem, Olympus
- They share the same `@halcore/Pgmem` socket — single-threaded, so they queue naturally

## Context Sizing

Keep `context_key` content under 32KB (Pgmem's PARK_THRESHOLD). Larger content is truncated with a warning. Split large inputs across multiple park keys and list them in the task_prompt.
