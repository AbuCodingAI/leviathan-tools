# LeviathanOS Tools

Native tools of LeviathanOS — **v4.3** — **binaries *and* full source**.
Lightweight tools for a lightweight OS (old hardware, especially old Intel Macs).

- Prebuilt binaries in [`bin/`](bin/) — for x86-64 Linux, just run them.
- Complete source in [`src/`](src/) — build it yourself with `make`.

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

## Build from source

The GUI tools need the usual dev headers (GTK3/GTK4, WebKitGTK, X11, zstd):

```sh
make            # builds everything into bin/
make dev        # or build a single tool
```

Source lives in [`src/`](src/), shared bits in [`third_party/`](third_party/),
build rules in the [`Makefile`](Makefile).

## License

GPLv3, with an Additional Permission waiving attribution — see [`LICENSE`](LICENSE).

The **complete corresponding source** for every binary in `bin/` is included in
this repository under `src/` — so the GPL is fully satisfied: binaries and the
source that builds them, together, in one place.
