# app_studio

The studio panel: an eight-state controller and the scene it renders on the
480×480 display. Portable C — it talks to ESP-GSP and to `mb_event_t`, never to
ESP-IDF drivers — so the same controller runs in the simulator through
`host/`.

## States

| State | Headline | Detail line | Extras |
|---|---|---|---|
| `IDLE` | `IDLE` | hold the AI button and speak | bear |
| `LISTENING` | `LISTENING` | listening… | bear |
| `BUILDING` | `BUILDING` | building your program… | bear |
| `AWAIT_MICROBIT` | `CONNECTING` | waiting for micro:bit… | bear; ritual text after 15 s |
| `FLASHING` | `FLASHING` | — | bear + five-segment progress |
| `VERIFYING` | `VERIFYING` | checking your program… | bear |
| `DONE` | — | — | trophy clip |
| `FAILED` | `FAILED` | the reason | — |

`CONNECTING` is the only screen label that differs from the state name: at 40 px
`AWAIT_MICROBIT` needs 336 px and the app column gives 288. `PAIRING` was
rejected as a wording — gate G2 measured `mode=1`, this flow never pairs.

## Event contract

`app_studio_apply()` takes `mb_event_t` from `components/mb_event`. The pipeline
reports; this component decides what the panel shows. `DONE` and `FAILED` return
to `IDLE` after 10 s and 8 s; `AWAIT_MICROBIT` promotes the ritual hint at 15 s
and never times out.

## Scene bindings

Every object the controller touches is named and declares `bind` +
`bind_target`; a name alone generates only `*_get_info` and the setters will not
link. `state`→`text`, `detail`→`text`, `bear`→`visible` (with `bear.y` declared
dynamic over 186..194 for the 600 ms bob), `success`→`visible`, `track1..5` and
`seg1..5`→`visible`. The flashing segment pulses at 500 ms; segments below it are
solid.

## Images

`assets/microbit-front.png`, `assets/bear.png`, `assets/trophy.gif` (34 frames,
80 ms, 172×240). GSP only draws raster contents when the compiled image cache has
a budget — packing with the default `image_cache_budget_bytes=0` produces a scene
where every image is invisible, in the simulator and on the device alike. Set the
budget in the build configuration.

The trophy is an animated raster: gspc packs its frames
(`animation_frame_peak_bytes=165.9 KiB`) but the ESP-GSP simulator composites
nothing for animated sources, so `DONE` renders empty in `project sim` while a
still PNG in the same slot draws. That state is verified on hardware.

## Simulation

```sh
python mosaico.py project sim --project projects/microbit_studio --headless   # PC backend
STUDIO_SIM_STATE=FLASHING python mosaico.py project sim ...                  # one screen
```

Passing `--frames`, `--fps` or `--dump-ppm` selects the scene-only preview and
skips the backend entirely (`gsp-sim/run.py`, `requests_sim_bridge()`).

Third-party notices: `NOTICE.md` beside this file.
