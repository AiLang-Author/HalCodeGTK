# HalCodeGTK

GTK / AppDesk front end for HalCode9000. **This is the GUI fork** — same
agent, tools, history, backends, and config as HalCode9000. The ANSI TUI
(`UI.ailang` raw-mode) stays in `~/HalCode9000`. Here `UI.ailang` is a
headless stub so the agent still compiles; pixels come from AppDesk + a
thin GTK blit host.

```
make -C shell run          # AppDesk MDI + chat input (stub reply)
./setup.sh                 # same keys/providers as the TUI tree
./HalCode9000.x --agent    # headless agent (no TUI)
```

Haiku: run the Linux GTK binary on the Linux runtime bridge. BeAPI chrome
is a later twin host, not a rewrite of the agent.

Chat Enter is still a local stub until the desk process talks to
`HalCode9000.x`. Config/providers/keys are this tree’s `setup.sh` /
`~/.halcode/` — same as the TUI install.
