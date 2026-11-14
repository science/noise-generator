# Open questions for the user

Answer inline or in conversation. Each question names the plan item it
unblocks. Move answered items into the relevant doc (decisions, hardware, plan)
and delete them from here.

## Blocking Phase 0

1. **USB port.** The board did not show up on USB on 2026-11-13. Please move
   the cable to the micro-USB port labelled **UART** (the other one is
   power-only), and check the cable carries data. Then Claude can run the
   probe. (`docs/runbooks/bring-up.md` step 1.)
2. **SD card wipe.** Is it OK to reformat the 64 GB card as FAT32? This erases
   it. It needs a USB card reader on bambam, plus your PIN for `sudo`.

## Shapes Phase 2 (output path, D2)

3. **Which speaker is it** (make/model)? Specifically:
   - Does it have a 3.5 mm AUX input? If so, a cable from the board's
     headphone jack is the most reliable option of all.
   - Does it turn itself off when idle or silent? If it does, can it be woken
     over Bluetooth, or does it need a button press?
   - Is it on a smart plug (like the kitchen speaker on
     `switch.kitchen_plugs_socket_1`)?
   - Is it ever used with anything besides the Pixel 3?
4. **The current brown-noise MP3.** Can you share the file the Pixel plays? We
   can measure its spectrum and tune the synthesized "brown" preset to sound
   the same.

## Shapes Phase 3 (Home Assistant, D4)

5. **Mosquitto (an MQTT broker) in the HA stack.** OK to add a `mosquitto`
   service to `~/dev/home-assistant/docker-compose.yml`, plus the HA MQTT
   integration? It is the standard way for custom firmware to show up as HA
   entities. The alternative is HTTP polling with hand-written YAML.
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
