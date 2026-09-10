# HalCode GTK shell

Two processes, same as CAD/Paint:

- `halcode_desk.x` — AILang kernel. AppDesk MDI, fonts, framebuffer.
- `halcode_shell_gtk` — native chrome + Cairo blit only.

GTK first. Haiku can run the Linux GTK binary on the Linux runtime bridge;
BeAPI chrome is later, not a HalCode port.

```
make -C shell
make -C shell run
```

Drag title bars, resize the grip, X closes a window. Click the Chat
input bar, type, Enter. Ctrl+Q or File → Quit stops both processes.

State dir `/tmp/halcode_app` (CAD contract):

| File | Meaning |
|---|---|
| `meta.bin` | int32 width, height, pitch |
| `frame.raw` | BGRA |
| `gen.txt` | generation; shell reloads on change |
| `ptr.txt` | `d`/`m`/`u` x y (mouse) |
| `cmd.txt` | `quit` |

Socket JSON (`@halcode/shell`) replaces `ptr.txt` when chat IME lands.
