---
name: relmem
description: Using the Relmem tool — Postgres-backed relational code/text symbol index. Covers indexing, symbol lookup, call-graph queries, and full-text search. Load when you need to find where a function is defined, what calls it, or what a file contains.
---

# Relmem — Relational Symbol Memory

Fire-and-forget daemon. Starts on demand, connects to postgres (`ailang_system`), serves one or more requests, then exits after 30s idle. HalCode9000 restarts it automatically on next use.

Postgres tables owned: `hc_files`, `hc_symbols`, `hc_symbol_edges` (shares `ailang_system` DB with Pgmem which owns `hc_context`, `hc_tasks` etc.)

---

## When to use Relmem vs Pgmem

| Need | Tool |
|------|------|
| Where is function X defined? | Relmem `op=where` |
| What symbols does file F contain? | Relmem `op=symbols` |
| What calls function X? | Relmem `op=callers` |
| What does function X call? | Relmem `op=calls` |
| Full-text search across all symbols | Relmem `op=query` |
| Index a directory of source files | Relmem `op=index` |
| Park/retrieve working context | Pgmem |
| Track tasks / agent coordination | Pgmem |

---

## Indexing

Parse source files and store symbols into postgres. Incremental — skips files whose mtime hasn't changed.

```
Relmem op=index  path=<abs-dir>  [exts=py,ailang,sh,md]
```

- Never pass `/`, `/mnt`, `/mnt/c`, `/home` — blocked (unbounded walk).
- Pass the specific project root, e.g. `/mnt/c/Users/Sean/Documents/HalCode9000`.
- Default extensions: `ailang, py, sh, md, js, ts`.

---

## Symbol Queries

### List all symbols in a file
```
Relmem op=symbols  path=<rel-or-abs-path>
```
Returns: `[file: N symbols]` then one line per symbol — `kind  name  :line`.

### Find where a symbol is defined
```
Relmem op=where  symbol=<name>
```
Returns exact matches first, then partial matches. Case-insensitive.

### Full-text search across all symbols
```
Relmem op=query  keywords=<words>  [limit=15]
```
Uses postgres `tsv @@ plainto_tsquery` — searches symbol names and bodies.

### Call graph
```
Relmem op=callers  symbol=<name>  [limit=25]   → who calls this function
Relmem op=calls    symbol=<name>  [limit=25]   → what this function calls
```

---

## Maintenance

```
Relmem op=forget  path=<file>          → remove one file from index
Relmem op=drop                         → drop all indexed files for this project
Relmem op=status                       → file count + symbol count
```

---

## Workflow: indexing then querying

```
# 1. Index the codebase (first time or after major changes)
Relmem op=index path=/mnt/c/Users/Sean/Documents/HalCode9000

# 2. Find a symbol
Relmem op=where symbol=Walker_ProcessFile

# 3. Understand its call graph
Relmem op=callers symbol=Walker_ProcessFile
Relmem op=calls   symbol=Walker_ProcessFile

# 4. List all symbols in a file
Relmem op=symbols path=cc_tools/cc_relmem_ipc.ailang
```

---

## Anti-patterns

- **Don't index broad paths** — `/mnt/c` hangs permanently. Always scope to a specific directory.
- **Don't call `op=index` before every query** — incremental skip means re-indexing unchanged files is a no-op, but it still walks the whole tree. Only re-index when files have actually changed.
- **Don't use Pgmem for symbol ops** — Pgmem has no sym_* handlers. Relmem is the symbol tool.
- **Don't worry about the daemon being down** — HalCode restarts it automatically. Just call the tool.
