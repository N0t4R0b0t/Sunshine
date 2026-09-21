# Browser streaming client: unattended end-to-end tests

Runs headless Chrome against Sunshine's real browser session layer and checks what the browser
actually decodes. No person, display or monitor is involved.

```
ninja -C cmake-build-webrtc datachannel   # once, builds the WebRTC library the host links against
npm install                                # once
node run.mjs                               # exit status 0 only if every check passed
```

Options: `--only=<text>` runs matching scenarios, `--chrome=<path>` picks a browser (it must decode
H.264, so Playwright's bundled Chromium is not used), `--host=<path>` picks a host binary,
`--verbose` shows the host's log. Results are also written to `build/report.json`.

## What it tests

The host (`e2e_host.cpp`) is Sunshine's real `src/webrtc/session.cpp` and `media.cpp`, with the desktop
capture replaced by ffmpeg encoding a test pattern to real H.264. So the SDP negotiation, RTP
packetization, loss recovery and the decoded picture are all genuine.

| Scenario | Checks |
| --- | --- |
| basic | Chrome decodes 30+ frames at the right size and 20+ fps, negotiates the best profile it offered, decodes a keyframe, shows real picture content, sees under 2% loss, and the data channel echoes. Run with and without mDNS candidate hiding. |
| closing the tab | The host stops the stream with no request from the browser. |
| hanging up | `DELETE` returns 200, the stream stops, no sessions remain. |
| dying stream | When the host stream ends on its own, the browser sees its connection end. |
| no H.264 | A VP8-only browser is refused with a clear reason, and nothing is left running. |
| limits | The fifth simultaneous session is refused. |

## Tier 2: a real Sunshine

`run_sunshine.mjs` starts the actual `sunshine` binary on a private virtual display (Xvfb), with a
video test pattern playing on it, and streams that display to headless Chrome. This is the part the
fake host cannot cover: real screen capture, the real software encoder, display preparation, and the
media tap in `stream.cpp`.

```
ninja -C cmake-build-webrtc -j4 sunshine   # a full link; heavy on memory, see below
node run_sunshine.mjs
```

It runs the scenarios that need nothing special from the host. It also checks that the browser shows
real screen content rather than a blank frame, by sampling the decoded picture. `--no-content` leaves
the display empty on purpose, to show that check failing.

Nothing touches the real desktop. Sunshine gets its own X server, a private home and runtime
directory (so it finds no session bus and none of your settings), a PulseAudio address that goes
nowhere with audio streaming off, and random ports. Everything it starts is stopped on exit.

Use `-j4` or lower and a memory cap for the build. A full parallel link has crashed this machine before:

```
systemd-run --user --scope -p MemoryMax=14G -p MemorySwapMax=6G nice -n 10 ninja -C cmake-build-webrtc -j4 sunshine
```

## What neither tier tests

Audio, real input injection (the harness has no access to `/dev/uinput`), hardware encoders, and
several simultaneous real sessions.

## Checking that the tests can fail

`build.sh` takes `MEDIA_SRC` and `OUT`, so a deliberately broken copy of `media.cpp` can be built
elsewhere and run with `--host`, without editing real source:

```
MEDIA_SRC=/tmp/broken_media.cpp OUT=/tmp/broken ./build.sh
node run.mjs --only=basic --host=/tmp/broken/e2e_host
```

`shim/` holds stand-ins for `logging.h` and `crypto.h` used only by this host. They are never part of
the Sunshine build.
