# System packages installed for omni-video-generator

| Date | Package | Version | Source | Why |
|---|---|---|---|---|

Nothing was installed. Qt 6.12 (base, declarative, multimedia, svg, wayland),
qmake6, ffmpeg 9.0.2, flock (util-linux), nvidia-smi and uv were already on the
system. The Python environment lives in `.venv/` (built with
`scripts/setup_venv.sh`), not in system packages.

## Installed later

| date | package | version | source | why |
|---|---|---|---|---|
| 2026-11-17 | omni-video-generator | 0.1.0-1 | built locally from `pkgbuild/` (`makepkg -f`, then `pkexec pacman -U`) | the app itself, so it is in the app launcher |
