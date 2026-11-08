# Turbo Term

A pseudo-3D terminal racing game written in C. OutRun-style: carve the
curves, dodge traffic, chase the distance — all in your console with ANSI
colors.

![genre](https://img.shields.io/badge/genre-racing-blue) ![lang](https://img.shields.io/badge/language-C-green) ![license](https://img.shields.io/badge/license-MIT-lightgrey)

## Features

- Pseudo-3D road projection (segment engine, curves, rumble strips, lane markers)
- Traffic to dodge with collision detection
- Speed-sensitive steering and centrifugal force on curves
- Score, speedometer, title/game-over screens
- ~60 FPS in any modern terminal (Windows Terminal, cmd, Linux)

## Controls

| Key | Action |
|---|---|
| `Up` / `W` | Accelerate |
| `Down` / `S` | Brake |
| `Left` / `Right` or `A` / `D` | Steer |
| `R` | Restart (after wreck) |
| `Q` | Quit |

## Build

```sh
gcc -O2 -o turboterm turboterm.c -lm
```

Windows binary included in [Releases](../../releases). Any C99 compiler
works — no dependencies beyond libc and libm.

## How it works

The track is a loop of 400 segments (200 units each). Each frame the
camera projects segment endpoints into screen space; road polygons are
rasterized row-by-row into a character buffer with ANSI color styles.
Curves accumulate as a lateral offset that shifts the camera projection.
Enemy cars live at absolute track positions, get recycled ahead of the
player, and collide via lateral overlap + z-proximity checks.
