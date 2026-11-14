# Open questions for the user

Answer inline or in conversation. Each question names the plan item it
unblocks. Move answered items into the relevant doc (decisions, hardware, plan)
and delete them from here.

## Phase 0

1. ~~USB~~ — fixed 2026-11-13: the cable was bad. The board is on
   `/dev/ttyUSB0`.
2. ~~SD card~~ — not needed for synthesized noise (user, 2026-11-13).

## Shapes Phase 2 (output path, D2)

3. ~~Speaker~~ — answered 2026-11-13: Edifier R2000DB, stays on while a
   Bluetooth link is active. Recorded in D2.
4. **The current brown-noise MP3.** Can you share the file the Pixel plays? We
   can measure its spectrum and tune the synthesized "brown" preset to sound
   the same.

## Shapes Phase 3 (Home Assistant, D4) — DEFERRED

5. **How HA talks to the device** — postponed by the user until the board is
   communicating. The user's starting position: no broker and no web API.
   See D4 and the "Discussion before Phase 3" section of the plan.
6. **Network.** OK to give it a DHCP reservation on the RB5009 (via
   `~/dev/network`), on the IoT SSID like the other ESP32s?

## Shapes Phase 4 (schedule and buttons, D6)

7. **Schedule.** Is the window 21:00–09:00 every day, or does it differ on
   weekends? And confirm the time zone (it will be stored as a POSIX TZ
   string on the device).
8. **Manual override semantics.** Proposed: a manual on/off from a button or
   HA holds until the next schedule boundary, then the schedule takes over
   again. A nap runs N minutes (default 2 h?), then reverts. A noise-type change
   from a button lasts until the next scheduled start. Does that match what you
   want?
9. **Buttons.** Probably 3–5 keys will be usable (it depends on the board
   variant; see `docs/hardware.md`). Proposed: Play = on/off (nap if pressed
   outside the schedule), Mode = next noise type, Vol−/Vol+. Anything else you
   want on a button? For example a "sleep timer: off in 30 min" or a "louder
   for tonight only".
10. **Noise types.** Beyond brown, pink and white, which do you want? For
    example "deep brown", or brown with slow random swells ("ocean-like").
    Any recorded sounds (rain, fan) for the SD-card phase?
