# LeviathanOS Tools

Prebuilt binaries of LeviathanOS's own native tools — **v4.3**.
Lightweight tools for a lightweight OS (old hardware, especially old Intel Macs).

## What's in `bin/`

| Tool | What it is |
|------|-----------|
| `infinitas` | Web browser (GTK4/WebKit) — persistent login, password manager, ad-block, tab groups |
| `architect` | Code editor / IDE — LSP, integrated terminal, fuzzy find, `.dev` build/run |
| `dada` | Security center — firewall, ClamAV, real-time Guardian, disk/fsck |
| `leviathan-dashboard` | **Helm** — system center: hardware, live thermals + fan control, services |
| `citron` | ptrace debugger — source-level, hardware watchpoints, conditional breakpoints |
| `dev` | `.dev` bytecode toolchain — JIT/AOT to C, zstd archives, W^X |
| `scribe` / `tabula` / `swipe` / `appbase` | Office suite — writer / spreadsheet / slides / database |
| `olaunch` | Spotlight launcher — apps, inline calculator, unit conversions |
| `churro-*` | The Churro desktop — dock, launcher, tray, keybinds, workspaces |
| `leviathan` | Package-manager config tool |

## Run

```sh
chmod +x bin/*
./bin/dev --help
```

These are built for **x86-64 Linux** (GTK3/GTK4 + WebKitGTK userspace assumed).

## License

GPLv3, with an Additional Permission waiving attribution — see [`LICENSE`](LICENSE).

### Corresponding source (GPLv3 §6)

These are GPLv3 binaries. The **complete corresponding source** is available on
request, and is being published at `github.com/AbuCodingAI/LeviathanOS`
(source ships next). This notice is the written offer required by the GPL.
