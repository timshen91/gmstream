# gmstream

Stream SteamOS Game Mode to a web browser with low latency.

gmstream is a small Wayland compositor that SteamOS's own gamescope runs
nested inside. It hardware-encodes each frame gamescope produces (VA-API:
H.264, HEVC or AV1) the moment it arrives and sends it over WebTransport to a
web client that decodes it with WebCodecs. Audio (Opus), keyboard, mouse,
cursor and gamepads (virtual Xbox 360 pads with rumble) go over the same
connection. It is not Moonlight/GameStream compatible and needs no Sunshine
or Polaris.

- The client picks the resolution and refresh rate; changing them does not
  restart Game Mode.
- About 7–8 ms from capture to display (Chrome on a Mac, Wi-Fi, 60 Hz).
- One mostly static binary runs directly on SteamOS: no container or packages
  at run time, nothing written to the read-only root.

Status: works end to end on one SteamOS 3.9 machine (Strix Halo). Showing
Game Mode on the device's own screen while nobody streams is not done yet.

## Layout

| Path | What |
|---|---|
| `core/` | C: wlroots compositor, VA-API/FFmpeg encoder, PipeWire audio, uinput gamepads |
| `src/` | Rust: WebTransport sessions, HTTPS server for the web client |
| `web/` | The web client (embedded in the binary) |
| `examples/probe.rs` | Command-line test client |
| `build/` | Build image with the static C dependencies |
| `session/` | systemd units and the Game Mode session script fork |
| `gamescope/` | Patches for SteamOS's gamescope and a container to build it |

## Docs

- [DESIGN.md](DESIGN.md): design, Game Mode integration, the gamescope
  patches, build and test workflow, known issues.
- [PROTOCOL.md](PROTOCOL.md): the wire format.

## Building

Builds run in containers on a Linux build box (see `build/Containerfile`,
`gamescope/Containerfile` and DESIGN.md §7). Copy `local.env.example` to
`local.env`, set your build box and device, then run `./dev-ship.sh`
(gmstream) or `gamescope/ship.sh` (patched gamescope).

## License

MIT (see [LICENSE](LICENSE)). The patches in `gamescope/` modify Valve's
gamescope (BSD-2-Clause). Binaries statically link FFmpeg, libva, wlroots and
other libraries under their own licenses.
