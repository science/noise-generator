# Bedroom noise generator — implementation plan

Version 2, 2026-11-13. It replaces the first plan, written before the board was
on the bench. Design reasons live in `docs/decisions.md` (D1–D8), and board
facts in `docs/hardware.md`. **Questions for you are at the end, each with a
space for your answer.**

---

## Summary (read this first)

### The goal

Retire the Pixel 3 that plays brown noise to the bedroom Edifier R2000DB over
Bluetooth, and the Kasa plug that exists only to cycle its battery. The
replacement is an ESP32-Audio-Kit board. It:

- generates the noise itself;
- streams it to the R2000DB over Bluetooth;
- runs its own nightly schedule;
- has physical buttons for on/off, tone and volume;
- updates over Wi-Fi.

Home Assistant's role is deliberately undecided (see Q6).

### What already works (verified on the bench, 2026-11-13)

- **The board is identified.** ESP32 rev 3.1, 4 MB flash, 4 MB PSRAM, ES8388
  codec on the "v1" wiring, and all six user keys. The factory firmware is
  backed up, and restoring it has been proven.
- **Noise synthesis.** White, pink, brown and deep brown, all at matched
  loudness, with independent left and right channels. Each colour's spectral
  slope is verified by tests.
- **Volume handling.** A volume curve, fades, and a ceiling that can never be
  exceeded. Every start fades in from silence.
- **Bluetooth streaming.** The ESP32 streams to a speaker as an A2DP source
  ("source" = the sender, the role the Pixel plays today). Tested against
  two desk speakers, a TP-Link and the Edifier R1280DBs. It holds a steady
  44,100 samples per second with flat memory use.
- **Your listening verdict:** *deep brown* is closest to what you want ("like
  a jet airplane cabin"). White and pink are never needed.

### Key concepts

- **Synthesized noise, no files.** The ESP32 computes noise live: random
  numbers shaped by small digital filters. It costs under 1% of the CPU, it
  never repeats or has a loop seam, and it needs no SD card. Firmware lives
  in the module's built-in flash.
- **Noise "colour" is tone balance.**
  - White has equal energy at every frequency, so it sounds hissy.
  - Brown falls 6 dB per octave, so it is bass-heavy, like a waterfall.
  - Deep brown also rolls the treble off more steeply. That is the "jet cabin"
    sound.
  - Grey is shaped so that every pitch sounds equally loud to human ears.
    That means less of the 2–5 kHz range ears are most sensitive to, and more
    bass than white. It is brighter than brown, but not hissy.
- **The tone ladder (new).** Instead of a noise-type switcher, there is one
  dial with two buttons, *darker* and *brighter*. It steps through a ladder
  that runs from grey at the bright end, through brown, to progressively
  deeper browns. The default step is today's deep brown. Every step plays at
  the same loudness, and changing steps glides over a fraction of a second,
  so there is no click. White and pink leave the device's controls entirely.
- **Gain** is the one place volume happens. Volume 0–100 maps onto a decibel
  curve, so steps sound even. There is a ceiling, and fade-in/fade-out on
  every start and stop. We never use the Bluetooth library's own volume
  command, because it changes the *speaker's* volume setting.
- **The device owns the schedule.** It keeps time itself, set by internet
  time (NTP) and stored with a timezone rule. The nightly on/off therefore
  never depends on Wi-Fi, Home Assistant, or t480. That matters because t480
  reboots every night at 03:00.
- **The speaker link supervisor (new).** Bench testing showed the Bluetooth
  library gives up after 3 reconnect attempts. It then only reconnects if the
  speaker happens to appear in a scan, which took about 2 minutes. The real
  firmware will retry forever, with a gentle backoff. Bluetooth link loss in
  the night must heal itself.
- **Keeping the speaker awake.** The R2000DB stays on while a Bluetooth link
  is active. So the plan keeps the link up even while the noise is "off" (see
  Q9 for the power trade-off).
- **Wi-Fi and Bluetooth share one radio.** The ESP32 time-shares the 2.4 GHz
  radio between them. Espressif says this works; the Bluetooth library's
  author says it does not. An overnight test decides. If it fails, Wi-Fi can
  be switched on only when needed, for a daily time sync and update window
  (see Q7).
- **OTA** (over-the-air update) means new firmware arrives over Wi-Fi, with
  no USB cable. If a new image fails its self-test, the chip automatically
  rolls back to the previous one. (That rollback support is confirmed
  present in the toolchain.)
- **Flash budget.** Bluetooth audio plus Wi-Fi fill about 85% of the space
  for one firmware image. Our code is small, so it fits, but every addition
  is checked.
- **Bedroom safety.** It boots silent and always fades in. Volume has a
  ceiling. No lights at night. Every flash or sound test needs your OK.

### Decided vs open

- **Decided:**
  - PlatformIO + Arduino firmware;
  - synthesized noise, no SD card;
  - Bluetooth to the R2000DB as the main output;
  - device-owned schedule;
  - a tone ladder instead of noise types;
  - the bedroom safety rules.
- **Open (questions below):**
  - the button map;
  - nap and stop behaviour;
  - schedule details;
  - what Home Assistant should do;
  - Wi-Fi always-on or on-demand;
  - OTA push or pull;
  - what "off" means for the speaker.

---

## Design

### Sound engine: the tone ladder

- One continuous family, from grey (brightest) → brown → deep brown →
  deeper browns. Proposed: about 9 steps (Q1).
  - **Brown family:** the existing brown filter (a 6 dB/octave slope with a
    140 Hz corner), plus a second low-pass filter. Lowering that filter's
    corner frequency step by step makes the sound darker. Today's deep brown
    has it at 500 Hz. Darker steps go lower, brighter steps go higher.
  - **Grey:** pink noise with a bass boost and a dip around 2–5 kHz. This
    approximates an inverted equal-loudness curve (ISO 226), the standard
    chart of how loud each pitch sounds to people.
  - **Between grey and brown:** a blend of the two, so the ladder has no big
    jump in the middle.
- Every step is normalised to the same RMS level, calculated in closed form,
  as `NoiseSynth` already does.
- A step change crossfades over about 200 ms, so you never hear a click.
- Tests (red/green, on the host):
  - Each step is darker than the one before. Measured by spectral centroid,
    the "centre of mass" of the spectrum, which falls step by step.
  - All steps play at equal RMS.
  - Grey shows its dip in the 2–5 kHz range.
  - Crossfades produce no discontinuity.
  - The existing brown and deep brown curves still pass, so nothing drifts.

### Buttons (proposal; Q2)

Six keys, numbered left to right as printed on the board. RST and BOOT are
not usable as buttons (`docs/hardware.md`).

- **KEY1 — play / stop.** Outside scheduled hours it starts a nap timer. Inside
  scheduled hours it stops until a chosen time (Q3).
- **KEY2 — darker.** One step down the tone ladder.
- **KEY3 — brighter.** One step up the tone ladder.
- **KEY4 — nap length / timer.** Each press cycles 30 min → 1 h → 2 h (Q2, Q3).
- **KEY5 — volume down**, 5% per step.
- **KEY6 — volume up**, 5% per step, never above the ceiling.
- **Long-press KEY1 (5 s) — pair with a new speaker.** Scans for nearby
  Bluetooth audio devices and picks the strongest. Only audio devices count,
  and there is a minimum signal strength. A quiet chime confirms. Your idea;
  details are in Phase 3.

Tone and volume changes are saved, so they survive a reboot. The saves are
batched, so holding a button doesn't wear out the flash.

### States and schedule (Q3, Q4, Q5)

The device is always in exactly one of these states:

- scheduled-on;
- scheduled-off;
- manual-on or manual-off (each held until a defined point, Q3);
- nap (N minutes, then back to whatever the schedule says).

- The schedule starts as 21:00 → 09:00 every day, with a timezone rule so
  daylight saving is automatic.
- At the scheduled start, it fades in over a configurable time (proposed
  30 s). At the scheduled end, it fades out (proposed 60 s).
- If the time has never been synced since power-up, the device does nothing
  surprising. It keeps its last state and keeps retrying NTP.

### Speaker link supervisor

- On boot, connect to the stored speaker address. If that fails, retry
  indefinitely: every 5 s at first, backing off to every 30 s. Fall back to
  discovery (scanning) between retries, so a speaker that was powered off is
  found as soon as it is back.
- While the schedule says "off": keep the link up and stream silence, so the
  R2000DB stays on. Or drop the link and let it sleep (Q9). Either way,
  verify that the R2000DB wakes for the nightly start.
- Log every connect and disconnect with a timestamp. These counts feed the
  overnight soak.

### Network: Wi-Fi, time, updates (Q6–Q8)

Wi-Fi is needed for at least two things, whatever Home Assistant does:

- **The clock (NTP).** The ESP32's clock drifts less than a second a day
  while powered, so a sync once a day is plenty.
- **OTA updates.**

Two ways to run it:

- **Always-on Wi-Fi.** Simplest, and needed for live control from Home
  Assistant. It depends on the overnight coexistence soak passing.
- **On-demand Wi-Fi.** Wi-Fi is up at boot, once a day in the afternoon (time
  sync + update check), and when you hold a button. Otherwise the radio
  belongs to Bluetooth. This sidesteps the coexistence risk entirely, but
  there is no live remote control.

OTA comes in two styles (Q8):

- **Push:** run a command on bambam, and it sends the image to the device.
  This is how the hot-tub and ESPHome devices work.
- **Pull:** the device checks a URL daily and installs anything newer.

Either way, Wi-Fi credentials will be kept in the device's own storage, not
compiled into the image, so a firmware file can never leak the Wi-Fi
password.

### Home Assistant (deferred; Q6)

You don't think your requirements call for a broker or a web API. The
options, from least to most integration:

- none, with the device fully standalone;
- HA can see status (playing, connected, firmware version);
- HA can also start, stop and nap;
- HA can also change the schedule, volume and tone.

The answer to Q6 decides whether we need MQTT, a small HTTP interface, or
nothing at all. Nothing in Phases 1–4 depends on it.

### Parked (not planned unless something changes)

- SD-card audio files. You don't need them; the synthesizer covers grey and
  brown.
- The analog headphone-jack output. It is the fallback if Bluetooth fails
  the overnight soak. The R2000DB has analog inputs.
- White and pink as user-facing choices. The code stays, because the tests
  use it as a reference, but no button reaches it.

---

## Phases

Each phase ends with `./scripts/test.sh` green and a commit. Pure logic is
written test-first on the host. Hardware steps are verified on the device
with your OK.

### Phase 0 — Bring-up — DONE (2026-11-13)

Board identified, factory firmware backed up and the restore proven, keys
mapped. Details: `docs/hardware.md`.

### Phase 1 — Sound engine: the tone ladder

1. `ToneLadder` / extended `NoiseSynth`, written test-first (see "Sound
   engine" above).
2. `bttest` gets KEY2 darker / KEY3 brighter, replacing the type switcher.
   Listening session at your desk with the R1280DBs. Pick the default step,
   and check the steps feel evenly spaced.
3. Measure CPU time per audio callback on the device, and keep a margin.

**Done when** you have picked a default sound, and the ladder has no audible
clicks or jumps.

### Phase 2 — Speaker link supervisor + the R2000DB

1. Never-give-up reconnect with backoff and scan fallback. A test on the
   desk speaker: power-cycle it 10 times at random intervals, and it must
   recover every time within 60 s of the speaker coming back.
2. The "off" policy (Q9): silence streaming vs dropping the link. Leave the
   R2000DB connected-but-silent for 1 hour and confirm it stays awake.
3. Bedroom trial, in daytime: turn off the Pixel's Bluetooth, pair the
   ESP32 with the R2000DB, and confirm the speaker wakes and plays after a
   simulated "night start".

**Done when** the R2000DB plays our noise and recovers from power-cycles on
its own.

### Phase 3 — Controller, buttons, schedule (no network yet)

1. `Schedule` (test-first): windows that cross midnight, "next boundary",
   daylight saving transitions, a disabled schedule.
2. `Controller` (test-first): the state machine (Q3), driven by events from
   buttons, the schedule and timers.
3. `Buttons` (test-first): debounce, short vs long press.
4. NVS persistence: tone, volume, schedule, speaker address.
5. The pairing long-press (your idea). It filters by Bluetooth device class
   (audio renderers only) and uses an RSSI threshold. Chime to confirm; a
   second long press within 10 s undoes it.
6. Until Phase 4 adds NTP, the clock is set by hand over serial.

**Done when** the device runs a full simulated night (with the clock sped up
in tests) and the buttons behave as agreed.

### Phase 4 — Wi-Fi, time, OTA + the coexistence decision

1. A DHCP reservation for the board, made in `~/dev/network` (asked for at
   the time). Then Wi-Fi with credentials stored on the device, NTP with a timezone rule, and
   OTA (push or pull, Q8) with automatic rollback.
2. **The overnight soak (go/no-go).** Bluetooth streaming to the R2000DB with
   Wi-Fi connected, for one full night. The pass criteria are in D2:
   - no audible dropouts;
   - no disconnects that fail to heal;
   - Wi-Fi stays reachable;
   - memory stays flat.
3. If it fails, switch to on-demand Wi-Fi (Q7).

**Done when** time and updates work without a USB cable, and the soak
result is recorded in `docs/decisions.md`.

### Phase 5 — Home Assistant (shape decided by Q6)

Possibly empty. The scope is set after we discuss Q6.

### Phase 6 — Cutover

A week of real nights, with the Pixel kept as a fallback. Then retire the
Pixel 3, its Tasker profiles, and the `pixel3-charger` plug automations.
That cleanup happens in `~/dev/home-assistant`, with your OK.

---

## Questions for you

Write your answer under each question, after "Your answer:". Short answers
are fine. Leave one blank if you want to discuss it instead.

**Q1. The tone ladder.** Proposal: about 9 steps. Grey at the bright end,
two grey/brown blends, brown, then five progressively deeper browns.
Today's "deep brown" is the default, in the middle of the dark half. White
and pink are removed from the controls. Does that shape sound right? Do you
want more or fewer steps?

> Your answer: 9 steps seems fine. Selection of step should be firmware stored and survive reboots, so whatever is chosen "sticks" unless the firmware is fully reset. (Ideally OTA updates don't destroy this config file - not sure if that's possible but it seems like it should be?)

**Q2. Button map.** Proposal:

- KEY1 play/stop (long press = pair with a speaker);
- KEY2 darker;
- KEY3 brighter;
- KEY4 nap length;
- KEY5 volume down;
- KEY6 volume up.

Any changes? Is there something you'd rather have on KEY4? For example:
"off in 30 minutes", "louder for tonight only", or nothing.

> Your answer: I don't need KEY4. KEY1 start would have an auto-timeout at 2 hours, except where the system is programmed to perform on the nightly schedule (9pm to 9am). 


**Q3. Play/stop and nap behaviour.** Proposal:

- **Pressed during scheduled hours (night):** stop. The noise stays off until
  the next scheduled start, so the next night still starts normally.
- **Pressed outside scheduled hours (day):** start a nap. It plays for the
  nap length (default 2 h; KEY4 changes it), then fades out.
- **Pressed again during a nap:** stop.

Is that what you want? In particular: should a night-time stop last until
tomorrow night, or just for a while (e.g. 30 min) before resuming?

> Your answer: With KEY4 deleted from functionality, here's my proposed scheduled behavior:
State cases:
1. Inside 9pm-9am autoplay window:
  1. KEY1 pressed (STOP): noise generator stops until next autoplay window start event
  2. KEY1 pressed (START): noise generator resumes playing, per its regularly scheduled window behavior
2. Outside 9pm-9am autoplay window:
  1. KEY pressed (STOP): noise generator stops until next autoplay window start event
  2. KEY pressed (START): noise generator starts playing for 2 hours
3. If START press (2 hour noise duration) outside autoplay window extends past the autoplay window start, then State Case #1 takes over and noise generation continues until autoplay window exits. In short State Case #1 should have precedence authority over State Case 2. 

**Q4. The schedule.** Is it 21:00–09:00 every day, or different on weekends?
Which timezone (e.g. America/Los_Angeles)? Fade-in at the start (proposed
30 s) and fade-out at the end (proposed 60 s): OK?

> Your answer: US/Pacific (LA/America) TZ. 9pm to 9am is the autoplay window. Fade in/out should be 5 second ramp (both for autoplay and button press)

**Q5. Volume at the start of each night.** Should it always start at a fixed
default you set, or at whatever level it was last left at? And what maximum
should the ceiling be, as a rough fraction of the R2000DB's comfortable
level? This can be set by ear during the bedroom trial.

> Your answer: Volume level should be persisted as durably as possible, just like noise tone selection (ideally surving reboots and OTA updates)

**Q6. Home Assistant.** What should HA be able to see or do? Pick a level
(or describe your own):

- (a) nothing, fully standalone;
- (b) see status only: playing, speaker connected, firmware version;
- (c) also start, stop and nap;
- (d) also change the schedule, volume and tone.

> Your answer: Maybe nothing for now? I had imagined we'd use it for OTA updates and getting alerts/logs for device health or stuff like that, which is why I suggested it join the HA network as a device. But I don't need it to be controlled from HA. 

**Q7. Wi-Fi strategy.** Always-on Wi-Fi (needed for live HA control, if the
overnight soak passes)? Or on-demand Wi-Fi (boot + a daily afternoon window
for time sync and updates; no live control, no coexistence risk)? If you
picked (a) or (b) in Q6, on-demand is probably enough.

> Your answer: I think on-demand Wifi is fine. Similar to the ~/dev/sleep-monitor project -- maybe use wifi to send logs/state periodically to HA, plus time sync (if not NTP directly). Oh, it might be nice if vacation mode could block the noise generator and unblock when I get back. That's bonus but would make sense and also be fine with "phone home every 3 hours" type HA interaction, and no HA interation is needed while noise is being streamed to b/t speakers, if that will disrupt the sound at all.

**Q8. OTA style.** Push from bambam (you or Claude run a command, the device
installs it)? Or pull (the device checks a URL daily and installs anything
newer, for example from this repo's GitHub releases)? Push is simpler and
more deliberate. Pull works even when you aren't at a computer, but needs a
release step.

> Your answer: Push is fine for now. This is hopefully a rare event that we push updates after the first few shakeout period

**Q9. What "off" means for the speaker.** During the day, should the ESP32
keep the Bluetooth link up and stream silence? Then the R2000DB stays on all
day (a few watts) and is always ready, including for naps. The alternative
is to drop the link and let the R2000DB go to standby. Then it must wake when
the ESP32 reconnects at night, and we'd have to test that it reliably does.
What does the Pixel do today: is the speaker on all day?

> Your answer: The pixel maintains b/t contact at all times. And I think that might be important b/c the edifier might, iirc, switch to opt or aux input if the b/t stays disconnected for too long. I'm not sure if pixel 3 is sending empty noise or just hanging onto the b/t link (assuming there's a difference: my b/t stack knowledge is fuzzy at this layer)

**Q10. Bedroom trial timing.** Phase 2 needs a daytime session in the
bedroom: Pixel Bluetooth off, ESP32 paired with the R2000DB, a few minutes
of noise at low volume. When works for you? Should the Pixel then stay
unpaired, or go back to normal until cutover?

> Your answer: We can probably make this all work tonight before I go to bed, and then pixel 3 retires tonight and we try this system then.

**Q11. Bench test notes.** Did you hear any clicks when switching noise type,
or when stepping the volume? Did KEY1's fade-out/fade-in sound smooth? In
your Bluetooth off/on test, did you toggle the speaker twice? The log shows
a second disconnect at 18:56:24 that took about 2 minutes to recover.

> Your answer: No clicks at all - all sounds were clean: vol up/down, fade in/out, noise tone switch were all excellent. Regarding bench test: yes, I switched off b/t for a short period and the system reconnected, then I switched off for a long period (2 min) and when I turned it back on, the device reconnected well after only 5 second delay. 

---

## Follow-up questions (round 2, 2026-11-13)

These come from reviewing your answers above. Same format as before.

**F1. Tonight's scope.** To retire the Pixel tonight, I'd build a minimum
"first night" firmware and leave the rest for over-the-air updates later.
Proposed for tonight:

- deep brown by default;
- KEY2/KEY3 stepping through a **brown-only** ladder, from lighter brown
  to deeper (grey is added in a later update, once we can tune it by ear);
- KEY1 start/stop with your state rules (Q3);
- KEY5/KEY6 volume;
- 5 s fades;
- tone and volume saved across reboots;
- the 21:00–09:00 Pacific schedule;
- Bluetooth link kept up at all times, with a reconnect that never gives
  up;
- Wi-Fi only briefly at boot, to set the clock.

Not tonight: grey, phone-home to Home Assistant, vacation mode, the
pairing button. Is that the right cut?

> Your answer:

**F2. The R2000DB's Bluetooth name.** For the first connection I'd have the
ESP32 look for the speaker by name, then remember its address. What name
does the Pixel show for it in its Bluetooth settings (e.g. "EDIFIER
R2000DB")? If you can see its address there instead, that works too.

> Your answer:

**F3. Bedroom power.** The board needs a 5 V USB supply of at least 1 A, on
an **always-on** outlet. Please don't use the Kasa "Tablet charger" plug the
Pixel uses. That plug is switched off for 6–9 hours at a time by the Pixel's
own Tasker profile. Once the Pixel retires, nothing will turn it back on, so
it could be left off. What will you plug it into?

> Your answer:

**F4. Wi-Fi credentials.** OK to copy the IoT Wi-Fi network name and
password from `~/dev/home-assistant/esphome/secrets.yaml` into this repo's
gitignored `firmware/.env`? They'd be compiled into the firmware. That's
fine with push updates, because the firmware file never leaves your
machines. (If we ever switch to pulling updates from GitHub, they move into
the device's own storage first.)

> Your answer:

**F5. START near the end of the night window.** By your rules, pressing
START at 08:50 (inside the window, after a stop) plays until 09:00, so only
10 minutes. Is that what you want? Or should START always give at least 2
hours (i.e. play until 09:00 or 2 hours from now, whichever is later)?

> Your answer:

**F6. Vacation mode and manual START.** When vacation mode blocks the
nightly autoplay, should KEY1 START still work, e.g. for a house sitter?
(Proposed: yes. Vacation mode only blocks the *automatic* start.)

> Your answer:

**F7. How the device talks to Home Assistant (for later).** The simplest
match for "phone home every few hours" is the sleep-monitor pattern. The
device makes ordinary web requests to HA's REST API with an access token:
it posts its health and logs, and reads `binary_sensor.vacation_active`.
No broker, and no server on the device.

**My concern:** an HA long-lived token has full access to everything HA
controls (climate, hot tub, locks if any). It would sit on a board in the
bedroom whose flash anyone with a USB cable can read. Mitigations:

- create a dedicated HA user for the device, so the token can be revoked on
  its own and its actions show up under that user's name;
- keep the token out of the firmware file.

HA has no read-only tokens, so this limits the damage rather than
removing it. OK with a dedicated user + token? Or would you rather avoid a
token on the device at all? Without one it could still post health data via
an HA webhook, but it could not read vacation mode.

> Your answer:

**F8. When updates can be pushed.** With on-demand Wi-Fi, the device only
listens for an update while Wi-Fi is up. Proposal: Wi-Fi plus the update
listener stay up for 10 minutes after every boot. To push an update in the
daytime, press RST, then run the push command within 10 minutes.
Alternatives:

- holding the now-unused KEY4 for 5 s opens the same 10-minute window
  without a reboot;
- or the window also opens during each phone-home check.

Preference?

> Your answer:

**F9. The Pixel after tonight.** I'd keep it charged, with Bluetooth off, in
a drawer for about a week as a fallback, rather than wiping or repurposing
it straight away. The HA-side cleanup (the `pixel3-charger` plug automations
and Tasker mappings) happens later in the HA repo. OK?

> Your answer:
