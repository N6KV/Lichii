# lichii-chimera

A windowless Lichii frontend for [Chimera](https://github.com/N6KV), a multi-system emulator
frontend. Chimera starts `lichii-chimera` as a child process and drives it over stdin/stdout:
it sends controller input once per frame and receives each frame's picture and audio, which it
shows inside its own window.

## What this fork changes

| File | Change |
|---|---|
| `src/lichii_chimera/` | New frontend (this folder) |
| `src/audio_core/sink/null_sink.h` | An optional capture callback: the null sink hands the samples it plays to the frontend instead of discarding them |
| `CMakeLists.txt`, `src/CMakeLists.txt` | `LICHII_CHIMERA` option (on by default, off on Android) |
| `.github/workflows/release-binaries.yml` | Builds and packages `lichii-chimera`; publishes `lichii-chimera-<platform>.zip`; only the standard macOS arm64, Windows x86-64 and Linux x86-64 builds |

Everything else is upstream Lichii, unchanged.

## How it works

- **Video:** Vulkan on a headless surface (`VK_EXT_headless_surface`, as Lichii's GPU trace
  replay uses it). Each presented frame is composited into a CPU buffer through the renderer's
  screenshot path and sent as RGBA8 (default 1280 × 720, `lichii_output` option).
- **Audio:** the null sink plays in real time; its samples go to Chimera with the frames.
- **Input:** Lichii's virtual gamepad (the one the Android touch overlay uses), players 1 and 2.
- **Pause:** when Chimera stops asking for frames for 150 ms, emulation is paused, and resumed with
  the next command.
- **Profile:** all Lichii folders (keys, NAND with firmware and saves, config, cache) live below
  `--save-dir`, apart from a regular Lichii installation.

## Usage

```text
lichii-chimera --core lichii --rom <game> --save-dir <profile folder>
               [--option lichii_output=1280x720] [--option lichii_docked=1]
```

The protocol is described in `chimera_protocol.h`. Keys, firmware and games are not included;
Chimera copies the user's own `prod.keys`, `title.keys` and firmware into the profile folder.

## License

GPL-3.0-or-later, like Lichii.
