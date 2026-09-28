# GroovixBox V2

Hardware step sequencer: an 8×8 RGB pad grid, function buttons R1–R8 on the right and track
buttons B1–B8 below. One codebase targets a Teensy 4.x, iPad, Mac and PC. The UI, storage,
communication and sequencer engine are separate modules behind swappable interfaces; see
[ARCHITECTURE.md](ARCHITECTURE.md). The SDL2 simulator is the first platform. The user manual is
in [docs/manual/](docs/manual/index.html), one page per mode, plus three tutorials that build a
track without any paging: [first beat](docs/manual/tutorial-beat.html),
[melody](docs/manual/tutorial-melody.html), [patterns and scenes](docs/manual/tutorial-live.html).

```
core/        portable C++11: engine/ ui/ storage/ comm/ app/
platform/    interface implementations: common/ (files, MIDI log), sdl/ (simulator),
             audio/ (the internal instruments)
tools/       gx_render: renders the internal instruments to a WAV, with no audio device
tests/       host tests using in-memory fakes of every interface
cmake/       layer-rule and MCU checks
```

## Surface

32 tracks, each with 32 patterns of up to 256 steps, shown 8 at a time (4 pages of tracks,
4 pages of patterns, 8 pages of steps). New patterns are 32 steps long. 8 pages
of 64 project slots and 8 pages of 64 preset slots (512 each). The track and pattern counts
are the simulator's limits; MCU builds use smaller ones (see Build).

| Button | Function |
|---|---|
| **R1 PROJECT** | Pads are project slots. Tap one to open it; the open project is saved first. Hold Shift and press B1–B8 to go to page 1–8; holding R1 instead does the same, also from other modes. |
| **R2 PATTERN** | Columns are tracks, rows are that track's patterns (top row first). Tap to choose the pattern a track plays; the tap also selects that column's track, which is how a track is picked here. Shift + a pad on the bottom row mutes that column's track, and while Shift is held that row shows the mutes (track colour = playing, dark = silent); a muted track's whole column dims. Mutes are session state, so they stay put as you flip between patterns and scenes. **The bottom row pages the grid**: B1–B4 are the track pages (tracks 1–8 … 25–32), B5–B8 the pattern pages (patterns 1–8 … 25–32), each lit green for the page on screen and blue for a page holding something. It takes no modifier and ignores Shift, so the pages stay reachable while Shift is held for the mutes. Shift therefore means one thing here — the bottom pad row is the mutes — and on the buttons it means what it means everywhere else, Shift + R2 for SCENES included. |
| **R3 NOTE** | Top 4 rows: the selected track's 32 steps. Bottom 4 rows: a keyboard of the project's scale (or the track's own scale, or two 4×4 blocks of drum pads on a drum track; see SCALE), starting on its root in the track's octave (2 by default) at bottom-left and rising one scale note per pad. Every row starts on the root, so a 7-note scale puts the octave root on the 8th pad and repeats it at the start of the row above; chromatic (the default, C2–G4) keeps 8 semitones per row with the roots on a diagonal. Tap an empty step to switch it on; pressing or holding a lit step shows its notes and never switches it off (only Clear + step removes a step, as on Circuit Tracks). Hold note keys and press a step to give it that chord (up to 4 notes); or hold steps and tap keys to add a note, or take it out if the step already has it (as on Circuit Tracks). **A track can play a chord from one key** (SCALE mode, TRIAD or 7TH): every key then gives its note with the rest of the scale stacked in thirds on top, and Shift + a key plays the single note. Holding a step lights all of its notes. Hold R3 and press B1–B8 for another page of 32 steps, with the page LEDs on B1–B8 and the playing page blinking. Shift + a step makes it the pattern's last step, shown red. Shift + the leftmost/rightmost key moves the keyboard down/up an octave, per track and saved. Note pads play their note at the velocity the pad was hit, and while playing + recording write it at the playhead - **the hit sets the step's velocity**, so a part played in keeps its accents (velocity belongs to the step, so the last hit into one speaks for its chord; a surface that can't tell how hard, such as the simulator's mouse, leaves the step's velocity alone). Hold Shift and press B1–B4 to change the track page. A track can show a **piano roll** instead (SCALE mode, bottom-row pad 1): columns are 8 steps and rows 8 notes of the track's scale, lowest at the bottom; tap a pad to add or remove that note (holding a pad just plays it), Shift + B5/B6 scroll the notes and Shift + B7/B8 the steps by 4, Shift + a bottom-row pad sets the last step, and hold R3 + B1–B8 jumps to a step page. |
| **R4 PARAMS** | Per-step velocity and gate. Top 4 rows: the current page of steps; tap one to select it, hold several to edit them together, and brightness shows each step's velocity. Rows 5–6: velocity in 16 levels (8–127), left to right and then along row 6. Rows 7–8: gate, one ramp of 16 lengths a pad each, shortest to longest — pads 1–5 are a sixth of a step up to five sixths, pad 6 is one step (the default), then 2, 3, 4, 5, 6, 8, 10, 12, 14 and 16 steps. One tap is that length; nothing is hidden behind a second tap. A pitch still sounding from a long gate stops before it plays again. **Hold Shift and rows 7–8 become the ratchets**: pad n fires the step n + 1 times across its own length (2 = 32nds, 4 = 64ths, 8 = a roll; the pads past 8 are dark). Each hit lasts its own gate or the space to the next, whichever is shorter, so a long gate rolls rather than blurs; the probability is rolled once, so a ratcheted step plays whole or not at all. Clear + step resets velocity, gate and the ratchet; Duplicate + step + step copies all three. Hold R4 and press B1–B8 for another step page; a tap of R4 stays here, and Shift + R4 opens PRESET mode. |
| **SHIFT + R2 SCENES** | Song sections — which tracks are silent and which pattern each track plays — reached the same way from every mode, Pattern mode included. Top 4 rows: 32 scene pads — grey empty, dim blue holds a scene, green playing. Bottom row: the 8 tracks of the current track page; tap to mute or unmute (track colour = playing, dark = silent). Tap a scene to launch it **at the top of the next bar** (16 steps, so the change lands in time however loosely you tap); the pad blinks green while it waits, tapping it again cancels, tapping another replaces it, and stopping forgets it. While playback is stopped a tap launches at once. A launch applies the scene's mutes and drops any solo. Hold R7 (Record) and tap a pad to capture what you are hearing; the scene pads turn red while it is held, and R7 on its own still toggles recording. Clear + pad erases, Duplicate + pad + pad copies. A scene stores the mutes **and each track's selected pattern**, so launching it puts the tracks back on those patterns (a scene saved before this leaves patterns alone). Scenes are saved with the project; the mutes themselves are session state. R2 returns to PATTERN mode. |
| **SHIFT + R8 SONG** | The scenes in the order they play, four bars a step. It is R8 and not R7 because the APC mini mk2's firmware keeps **Shift + R6 and Shift + R7** for its own Drum and Note pad modes and never passes them on; R8 alone is Play, so shift-play plays the song. Top 4 rows: the 32 scenes, as in SCENES. Bottom 4 rows: 32 song steps, each in its scene's colour (scenes 1–8 take the track colours, later banks the same dimmed); the step the song is on is white. Hold a step and tap a scene to put it there — while held, the scene it already plays lights in the palette. Tap a step on its own to go there and hear it. Clear + step empties it (a hole the song skips), Duplicate + step + step repeats a section. Tapping a scene launches it on the next bar, as in SCENES, while tapping a song step goes there at once. **R8 (Play) pressed in this mode plays the song**: it starts at the marker, holds each step's scene for 4 bars, skips empty steps and loops at the end; the step playing is bright white, and the marker dims when the song isn't running itself. Tapping a scene by hand takes over (the song stops advancing, the music carries on); stop and play again to hand it back. Play pressed in any other mode is the plain transport. Saved with the project. |
| **SHIFT + R3 SCALE** | Top two rows: a piano (C to C, black keys above white) to pick the root note. Bottom four rows: 24 scales, Major to Chromatic. The selected root and scale light green, and NOTE mode's keyboard then offers only notes of that scale. R3 returns to NOTE mode. The last two pads of the bottom row set the selected track's keyboard: pad 7 Drums (two 4×4 blocks of consecutive notes from C rising left to right and bottom to top, like drum machine pads: 36–51 on the left, 52–67 on the right at the default octave) and pad 8 Own scale. With Own scale on, the piano and scale list change only that track: its picks light in the track colour and the song's key stays as a faint green hint. It starts as a copy of the song's key each time it's turned on and is dropped when turned off. Pick Chromatic, root C, for every note from C. In use, Own scale lights in the track colour and Drums in amber; picking one replaces the other, and tapping it again follows the song's key. Saved per track. **Bottom-row pads 5 and 6 are TRIAD and 7TH**: with one lit (violet), a single key of the NOTE keyboard plays that many notes of the scale stacked in thirds from the key pressed, so the chord is always in key - no chord tables, and it follows whatever scale the track uses. One at a time; tapping the lit one goes back to one key, one note. Shift + a key plays the single note. **Chords, Drums and Piano roll are exclusive** - a drum track has no scale to build a chord from and the roll has no keyboard to play one on, so picking any one of the three drops the other two; Own scale is not in that group, since a chord is built from whatever scale the track uses. Per track, saved with the project. **Bottom-row pad 1 turns the selected track's piano roll on or off** (white when on). It sits alone at the left end of the row, three dark pads clear of the four on the right: those four say what the keys play, while the roll replaces the grid. |
| **SHIFT + R4 PRESET** | Pads pick the patch the selected track's gear plays (select the track with B1–B8 first). Tapping one sends **Bank Select + Program Change**: 8 pages of 64 reach the 512 patches those messages address. Both halves of Bank Select go out — MSB (CC 0) is the bank, LSB (CC 32) is zeroed — because a synth keeps the half it isn't sent, and then the Program Change, which is what makes it act on the bank. **A preset is a number, not something we store**, so every slot looks the same but the one in use, and Clear and Duplicate have nothing to act on here and do nothing. The simulator's status line shows the two numbers being sent (`B1:0`), since a bank is 128 patches — two pages — and the split is invisible on the pads. Hold Shift (or R4) and press B1–B8 to go to page 1–8; a tap of R4 that picked no page goes on to PARAMS, which is what R4 means elsewhere. |
| **SHIFT + R5 PROBABILITY** | The chance each step plays, with the same step selection: rows 5–6 are 16 levels from 6% to 100% (always), and brightness shows each step's probability; a chord plays whole or not at all. Hold R5 and tap a step to put it back to 100%, or press B1–B8 for another step page; Duplicate + step + step copies it. **Rows 7–8 are micro-timing**: how far off the grid the step plays, in ticks (a tick is a 24th of a step). Straight is the 9th pad — the first of row 8 — and the lane lights from there out to the value. The pads are rungs rather than a count: −1, −2, −3, −4, −6, −8, −10, −12 early and +1, +2, +3, +4, +6, +8, +12 late, so single ticks are available where feel lives and the ends reach half a step. **±8 are the triplet rungs** — a beat is 96 ticks and the grid sits at 0, 24, 48, 72, so every 8th- and 16th-triplet position is 8 ticks from a step. A nudged step plays off the grid without the grid moving: the playhead, the bar and everything hanging off it stay where they are. |
| **SHIFT + R1 GLOBAL** | Global settings. Top row: pick a setting; pad 1 is the tempo, pad 3 the swing and pad 4 the devices. **The last pad of the bottom row sends every fader and knob's CC again** at the value it last reported, so the gear matches the panel after a change of project; controls nothing has been heard from are skipped, so an untouched knob never slams a synth to zero (a MIDI Mix's own SEND ALL does the same for its 33 controls). The rows below show its value in the 3×5 font (the hundreds digit is 2 pads wide so three digits fit in 8 columns; digits alternate white and amber), and fader 1 sets it, 20–300 BPM across its travel. On the bottom row, pad 5 is up (+1 BPM) and pad 6 is down (−1 BPM); they dim at the limits. Pad 2 sets where the selected track plays: rows 3–4 are MIDI channels 1–16, row 5 the MIDI ports P1–P8 and rows 6–7 the internal instruments I1–I16, with the current ones in the track colour. A track uses a port **or** an instrument, so the row it isn't using dims; tapping its instrument again sends it back to its port. By default track n plays on channel n (starting again after 16) and each page of 8 tracks starts on its own port (tracks 1–8 on P1, 9–16 on P2 …), so 8 ports × 16 channels reach 128 instruments. **Pad 3 is swing**, 50 (straight) to 75 (every second step half a step late), on the same display, fader and up/down pads as the tempo. Swing moves when the notes play, not the grid. All of it is saved with the project, tempo and swing included. **Pad 5 is the arpeggiator**, per track: row 3 the mode (OFF, UP, DOWN, UP-DOWN, DOWN-UP, AS PLAYED, RANDOM), row 5 the rate (1/8 to 1/64T, all exact divisions of a step's 24 ticks) and row 7 how many octaves it climbs (1–4), each in the track colour. A step holding a chord then plays its notes one at a time instead of together, for as long as the step's gate lasts — so it pairs with SCALE mode's TRIAD and 7TH, which put the chord there in the first place. A step's ratchet is ignored while the arp is on. **Pad 4 is the devices page**: row 3 shows the control surfaces (PADS and MIXER, lit = here and answering), row 5 the MIDI ports P1–P8 (green = that port reaches something, so a 4×4 interface lights four), **row 7 the clock source** — INT (we drive and send clock), AUTO (follow an incoming clock while there is one, our own when there isn't; the default) and EXT (only ever follow, so Play waits without one). The lit pad is green while an outside clock is really driving and white while our own is, so AUTO shows which it settled on; Auto gives up half a second after the clock stops. While following, the tempo page shows the tempo measured from the incoming clock and its fader and +1/-1 pads do nothing rather than fight the master, and we still send one clock out for each one in. Start and Stop from outside drive the transport; Continue restarts like Start, since there is no song position yet. The clock source belongs to the instrument, not the project, so it is never saved with one. **The control surfaces and the keyboard reattach themselves** when they are unplugged and plugged back in: the ALSA sequencer has an announce port for gear appearing and disappearing, so nothing polls and the scan only happens when something has actually changed. The **output ports** are left alone instead — repatching mid-take is the player's call — and REFRESH lights up bright to offer the rewire, sending All Sound Off and All Notes Off on any port that has just come back so a note it was holding when the cable went is released. The first pad of the bottom row is REFRESH, which looks again for gear plugged in since the start and wires the ports to an interface that has appeared. Nothing scans on a timer — a scan walks every client of the MIDI system, which is not something to do behind the playhead's back — and a port already patched by hand is left alone. R1 goes to PROJECT mode. |
| **R5 CLEAR** | Hold and tap a pad to clear that project, pattern or step. **Clearing a project or preset doesn't delete the file**: it is moved to `trash/<date>_<time>/` inside the data folder (`trash/2026-09-22_23-32-31/project_07.gxb`), so a mis-tap can be undone by copying it back. Clears in the same second share a folder and never overwrite each other, and copying an empty slot over a full one is kept the same way. Nothing empties the trash. Clearing a step or a pattern is not kept. |
| **R6 DUPLICATE** | Hold, tap the source pad, then tap one or more destination pads. The source stays picked when you change page. |
| **R7 RECORD** | Record on/off. With playback running, note pads and **a MIDI keyboard** write at the playhead, velocity and all. **The playhead runs red while recording** (white otherwise), and a key held on the keyboard lights the pad that plays the same note. Track 1 is peach rather than red, so red means a marker - the last step, Record, the recording playhead - and never a track. |
| **R8 PLAY** | Play/stop. |
| **B1..B8** | Select a track on the current track page and return to NOTE mode, from any mode except GLOBAL settings, where they only move the selection so a channel, port or instrument can be given to one track after another (Shift + B1–B4 changes the track page there too) (e.g. tracks 9–16 after Shift + B2 in NOTE or PATTERN mode; flipping the page moves the selected track to the same position, so track 4 becomes 12). While R1 or R4 is held they pick a project/preset page instead, and their LEDs show the pages: green = shown, dim blue = has data, grey = empty. |
| **SHIFT** (bottom right) | Modifier, lit while held; see the modes above for what it does. |
| **FADERS 1–8** | One below each track button, positions 0–1023. Each sends a MIDI CC on its track's channel and port (see Control map); fader 1 sets the picked value in GLOBAL mode instead, and then sends nothing. |
| **MASTER FADER** | Below Shift. Sends a CC on the selected track's channel once one is assigned; unassigned by default. |
| **MIXER** | To the right of the grid, laid out like an Akai MIDI Mix: 8 strips, each with three knobs (K1–K3), a **MUTE** button, **A2** and a fader **F1**. A column on the right holds a button beside each knob row (**M1**–**M3**) and **SHIFT** beside the mute row, with the mixer's master fader below them. Strip n is track n of the current track page, so Shift + B1–B4 moves the mixer too. **The A2 row picks pages**: page 1–8 of whatever the open mode pages over — steps in NOTE, PARAMS and PROB, projects and presets in those, track pages everywhere else. **In PATTERN mode it mirrors the APC's bottom row** and splits the same way: A2 1–4 the track pages, A2 5–8 the pattern pages, showing the same eight pages the pads' own buttons show. The page you are on is green, a page holding something is brighter blue, and its LEDs stay lit, so page position is visible without holding anything (on the APC it only shows while SHIFT, R1 or R4 is held). It replaces Shift + B1–B8, which can select a track if Shift is released early. **An Akai MIDI Mix is the panel in hardware** (`platform/common/MidiMixSurface`, picked up automatically by the simulator): its MUTE row is the mute row (**lit = the track is silent**), **REC ARM is the A2 page row**, **SOLO is the mixer's Shift** — hold it and press MUTE to solo — and **BANK LEFT/RIGHT change the track page**, moving the strips and the pads together. The panel's LEDs follow the mute row, lit meaning the track is sounding; SOLO and SEND ALL have no MIDI-controllable LED. |
| **MIXER KNOBS AND FADERS** | Each sends a MIDI CC on its strip's track (see Control map): by default the faders send Volume (CC 7) and the knob rows Cutoff (74), Resonance (71) and Pan (10, centred). The mixer's master sends nothing until it is assigned. |
| **MIXER MUTE** | Tap to mute or unmute the strip's track: its steps stop playing at once, and notes it is holding are released. **SHIFT + MUTE solos** the track — only soloed tracks are heard, and several can be soloed at once; **tapping SHIFT (SOLO on a MIDI Mix) on its own drops every solo**, which is how you get back to hearing everything. Notes played by hand still sound, and mutes belong to the session, not the project, so they are not saved. LEDs: white = soloed, track colour = heard, half-lit = silenced by another track's solo, darkest = muted. The knobs, A2 and M1–M3 have no function yet. |

Project, pattern and preset slots: **green** = selected, **dim blue** = holds data, **grey** = empty.
Note mode: steps in the track colour, playhead white, held steps and their note green, last step red.

## Internal instruments

Tracks routed to I1–I16 play in `platform/audio` (`gx_audio`) instead of going out of a MIDI
port. The sequencer calls it from its own thread, the audio device calls it from the audio
thread, and the two meet only through a wait-free queue, so the audio side never allocates,
locks or waits.

**Each slot hosts one LV2 plugin.** `Lv2Host` scans the installed plugins with lilv at
start-up; each slot instantiates the plugin its config names, connects its ports, feeds it MIDI
through an atom sequence and runs it per block. Plugins that need work off the audio thread get
it — the worker extension runs on its own thread, which sfizz needs to load its SFZ files. A
slot with no plugin named falls back to a plain sine, which is what proved the path before
plugins arrived and what CPU measurements are taken against.

Building with hosting needs `liblilv-dev`; without it everything still builds and the slots
keep the stand-in tone.

Each slot's plugin, level and pan come from [config/instruments.conf](config/instruments.conf),
read from the config directory (`--config DIR`, `/mnt/usb1/config` by default) or named
outright with `--instruments FILE`:

```
I1         = http://sfztools.github.io/sfizz   # the plugin this slot hosts
I1.sfzfile = ~/samples/drums.sfz               # the plugin's own parameters
I1.volume  = -3
I1.gain    = 0.8                               # ours: 0..1
I1.pan     = -0.3                              # ours: -1 left .. 1 right
```

Anything that isn't `gain` or `pan` goes to the plugin — one of its **control ports** by
symbol, or a **patch parameter** by the last part of its URI, which is how a sampler is told
which file to play. A name the plugin doesn't have is reported when it loads, along with some
of the names it does have.

**Plugin settings are saved with the project.** When a project is saved, each loaded plugin's
LV2 state goes into a blob beside it (`instruments_007`), and opening the project puts it back
— so a song reopens with the sounds it was written with, wherever the config's defaults have
moved on. A slot that now hosts a different plugin than the project expects is left alone,
with a line saying so; swapping a plugin while it plays is a job for later. The config decides
*which* plugins are in the rack; the project decides how they were set.

Notes and CCs reach the plugin; **CC 7 and CC 10 are kept by the mixer** for the slot's level
and pan, so the mixer's faders and pan knobs work the same whatever the plugin does. At exit
each slot reports how many MIDI messages it received and how many blocks it ran — the first
thing to look at when a plugin stays silent.

The mixer moves both live without any extra wiring: its faders send **CC 7** (level) and its
third knob row **CC 10** (pan), which reach whichever slot the track is routed to. Panning is
constant power, so moving across the image doesn't change how loud a part sounds.

`AudioBackend` is where a device goes, and there are three:

| Backend | |
|---|---|
| `alsa` | Straight to an ALSA device with our own `SCHED_FIFO` thread. What the standalone instrument uses: no audio server in the path. The device paces it by blocking in `snd_pcm_writei`. |
| `jack` | JACK, or PipeWire answering the JACK API. JACK owns the thread, rate and block size — the easy path while developing. Needs a running server (`pipewire-jack` or `jackd`). |
| `null` | No device: renders on demand. Tests and `gx_render` use it. |

```sh
./build/groovix_sim --audio alsa                       # the default ALSA device
./build/groovix_sim --audio alsa --audio-device hw:0 --block 64
./build/groovix_sim --audio jack                       # needs a JACK server
```

Without `--audio` nothing opens and tracks on I1–I16 stay silent; MIDI tracks are unaffected.
If the device can't be opened the simulator says so and carries on. At 48 kHz a 128-frame
block is 2.7 ms; the simulator prints the rate, the block size and whether it got real-time
priority (it needs `rtprio` in `/etc/security/limits.d`).

```sh
./build/gx_render --out beat.wav --demo --seconds 4     # a short pattern on instrument I1
./build/gx_render --out song.wav --data /mnt/usb1/data --seconds 30
```

`gx_render` also reports how fast it rendered against the audio it produced ("12x realtime" is
about 8% of one core), which is how the instrument count gets sized on the target board.

## Control map

Every fader and knob can send a MIDI CC on its track's channel and port — the strip's track on
the current track page, or the selected track for the two master faders. Moving a control sends
it; the positions a surface reports at start-up are only recorded, so nothing is sent until you
move something.

The assignments live in a config file, [config/controls.conf](config/controls.conf). It is
looked for in the **config directory** — `--config DIR`, which defaults to `/mnt/usb1/config`
— or named outright with `--controls FILE`; without one the built-in defaults apply. Config
and data are separate so the rig's settings can sit in `/etc` or under version control while
projects churn somewhere writable. **The same file names the MIDI
device to send out of**, so a rig is described in one place rather than on the command line:

```
midiout     = MIDI4x4     # which interface P1..P8 go out of
p3          = Digitone    # or give one port a device of its own
mixfaderrow = 7           # all eight mixer faders: Volume
knobrow3    = 10, center  # the third knob row: Pan, centred
knob3.1     = 91          # just strip 1's third knob: reverb send
fader2      = 11          # a fader under the pads: Expression
master      = off         # sends nothing
```

`aconnect -o` lists everything you can send MIDI to, and **the quoted names are what these
lines match** — copy one as it stands. The name after `client` is the device; the indented
lines under it are its inputs, and those names usually carry the device's inside them:

```
client 40: 'MIDI4x4' [type=kernel,card=6]
    0 'MIDI4x4 Midi Out 1'          ->  p1 = MIDI4x4 Midi Out 1
    1 'MIDI4x4 Midi Out 2'          ->  p2 = MIDI4x4 Midi Out 2
```

The `0 1 2 3` on the left are ALSA's own port numbers; they count from zero and shift as gear
is plugged in, so nothing in the config uses them. `aconnect -l` shows the same list with what
is wired to what, including the sequencer's own `GroovixBox` ports while it runs, and
`amidi -l` lists the hardware by card when a device is plugged in but no ALSA client appeared.

`midiin` names the keyboard to listen to; with no line one is found by itself — a card-backed
device with outputs that is neither the loopback nor a surface we drive, the one with the
*fewest* inputs winning, since a keyboard has one and an interface has several. `midiout` names
the interface to wire P1..P8 to in order; with no line at all one is found by
itself — a card-backed device that is neither the loopback nor one of our own surfaces, with
the most inputs winning, so a 4×4 beats a keyboard that enumerates first. A rig whose only
device is a controller will still pick it, which is when naming one earns its keep. `p1`–`p8` give a port a device port of its own, which is how a USB
synth gets a port to itself: `p3 = MIDI4x4 Midi Out 2` (that port), `p3 = Digitone` (that
device's first input), `p3 = MIDI4x4:2` (its second input, counting the inputs it offers) or
`p3 = Digitone:MIDI 1` (that device's input whose name contains that). Everything matches on
part of the name, ignoring case; the colon forms are only for when a port's own name doesn't
tell two devices apart. Ports named this way are wired to exactly that; the rest are left for
`aconnect`.

Names are `fader1`–`fader8`, `faderrow`, `master`, `mixfader1`–`mixfader8`, `mixfaderrow`,
`mixmaster`, `knob<row>.<strip>` (rows 1–3, strips 1–8) and `knobrow1`–`knobrow3`. The sweep is
`full` (0–127 over the travel, the default) or `center`, which holds CC 64 in a small detent in
the middle for pan-like controls. `#` or `;` starts a comment.

## Build, run, test

Requires CMake ≥ 3.10 and SDL2 (`sudo apt install libsdl2-dev`, `brew install sdl2`).

```sh
cmake -S . -B build
cmake --build build
./build/groovix_sim                         # the simulator: the device in a window
./build/groovix                             # the instrument itself, no window (Linux + ALSA)
(cd build && ctest --output-on-failure)     # core tests + layer rules
cmake --build build --target check_mcu      # core freestanding, 32-bit (Teensy 4.x)
cmake --build build --target check_64bit    # core freestanding, 64-bit (Cortex-A53)
```

### Building for a Raspberry Pi

`groovix` needs **ALSA and nothing else** — no SDL, no plugin host — so the Pi build is small:

```sh
sudo apt install cmake g++ libasound2-dev          # on the Pi
cmake -S . -B build-pi -DGX_BUILD_SIMULATOR=OFF -DGX_BUILD_AUDIO=OFF \
      -DGX_NUM_TRACKS=32 -DGX_NUM_PATTERNS=32 -DGX_MAX_STEPS=256
cmake --build build-pi --target groovix
```

`GX_BUILD_SIMULATOR=OFF` drops SDL, `GX_BUILD_AUDIO=OFF` drops the instrument engine and its
JACK and LV2 dependencies; the capacities match the desktop so project files move between the
two. The binary links `libasound`, `libstdc++`, `libgcc_s` and `libc`, so a 64-bit Raspberry
Pi OS Lite image needs only `libasound2` to run it.

**Cross-compiling from a desktop** — for turning out an image repeatably, or when the Pi is
too slow to build on — uses [cmake/rpi-aarch64.cmake](cmake/rpi-aarch64.cmake):

```sh
sudo apt install crossbuild-essential-arm64
sudo dpkg --add-architecture arm64 && sudo apt update
sudo apt install libasound2-dev:arm64
cmake -S . -B build-rpi -DCMAKE_TOOLCHAIN_FILE=cmake/rpi-aarch64.cmake \
      -DGX_BUILD_SIMULATOR=OFF -DGX_BUILD_AUDIO=OFF \
      -DGX_NUM_TRACKS=32 -DGX_NUM_PATTERNS=32 -DGX_MAX_STEPS=256
cmake --build build-rpi --target groovix
scp build-rpi/groovix pi@raspberrypi:~/
```

It targets `-mcpu=cortex-a53`, which is both the Pi 3B+ and the Zero 2 W, and takes
`-DGX_SYSROOT=/opt/rpi-sysroot` when you have a Pi's filesystem copied over instead of the
`:arm64` packages.

### `groovix`: the instrument on a box with no screen

`groovix` is what runs on a Raspberry Pi in a case: the same core and the same platform code
as the simulator, without SDL. It finds the APC and the MIDI Mix, wires P1..P8 to an
interface, reads `controls.conf`, and plays. It takes the same options as the simulator where
they mean the same thing (`--data`, `--controls`, `--midi-out`, `--no-midi-out`, `--midi-log`,
`--allow-multiple`) plus `--no-surfaces` for a box with no controller attached. **SIGTERM is a
clean stop**: it saves the open project and puts the LEDs out, so `systemctl stop` leaves a
tidy instrument behind. [config/groovix.service](config/groovix.service) is a unit file to
copy; it grants the real-time priority the clock asks for.

Both surfaces and the ports are found once at startup and again whenever the **REFRESH** pad
on the devices page of [Global settings](docs/manual/global.html) is pressed — nothing polls,
so a scan never lands behind the playhead.

`gx_platform_linux` holds what the two builds share: `MidiRig` (the ports, the surfaces and
the devices page's answers), `RigConfig` (the config file) and `SingleInstance` (one
instrument per machine, since two would fight over the same APC).

### Where it keeps its files

A built GroovixBox is a Pi with a USB stick in it, and the stick is the whole of its storage.
Both binaries resolve the same two directories the same way, in `platform/common/RigPaths.cpp`:

| | Default |
|---|---|
| **data** — projects, presets, the last open project, the trash | `/mnt/usb1/data` |
| **config** — `controls.conf`, `instruments.conf` | `/mnt/usb1/config` |

`--data DIR` / `--config DIR` win; then `$GXBOX_DATA_DIR` / `$GXBOX_CONFIG_DIR`, which is how
the [settings page](settings/README.md) is pointed at the same pair; then the defaults. The
directories themselves don't have to exist — they're created — but the mount point above them
does.

**On a machine with no stick mounted**, a desktop running the simulator, each default falls
back to the per-user directory these lived in before (`$XDG_DATA_HOME/Groovix/GroovixBox`, else
`~/.local/share/...`) and **says on stderr that it did**. It is the saying that matters: a
silent fallback is what made a rig with a stick in it look like it was ignoring it.

A rig that mounts its storage somewhere else can move the defaults at build time rather than
patch the file: `-DGX_DEFAULT_DATA_DIR='"/var/lib/groovix/data"'`.

Both compile **only** `core/`, freestanding, with the capacities the hardware ships, and fail
if an object references `new`/`delete`, `malloc` or the exception and static-guard runtime.
That is how the hardware path is kept free of everything else, from the desktop.

They differ in the ABI, which is the point of having two. `check_mcu` is 32-bit
(`arm-none-eabi-g++`, Cortex-M7); **`check_64bit`** is 64-bit (`aarch64-none-elf-g++` or
another `aarch64` compiler, Cortex-A53), where pointers and `long` are 8 bytes, alignment and
padding differ, and the calling convention is another one again. Code that quietly needs a heap
or a static guard can slip past one and not the other.

`check_64bit` **does not fall back to a 32-bit compiler** when no `aarch64` one is installed.
Falling back would report success while testing the very thing the target exists to cover
differently; instead the target simply isn't there, and asking for it says so.

Built `-nostdinc`, the only header core needs beyond the compiler's freestanding set is
`<string.h>` (`memcpy`, `memset`, `memcmp`, `strlen`), which the platform supplies.

| Configure option | |
|---|---|
| `-DGX_BUILD_SIMULATOR=OFF` | core, platform and tests without SDL |
| `-DGX_BUILD_AUDIO=OFF` | leave out the internal instrument engine (`platform/audio`) and `gx_render`, for a MIDI-only build. Needs `-DGX_BUILD_SIMULATOR=OFF` too, since the simulator plays the instruments. |

An MCU build compiles `core/` plus its own platform directory, so the audio engine never
enters the picture — nothing in `core/` refers to it. All the core carries for instruments is
one byte per track saying where the track plays, and the routing table in `MidiEventSink`
(72 bytes at the MCU's 16 tracks).

Desktop builds use 32 tracks × 32 patterns × 256 steps — four pattern pages, the same shape as
the four track pages, so both halves of pattern mode's bottom row reach every page. A scene recalls one pattern per track and there are 32 scenes, so a
project cannot reference more than 32 patterns on a track anyway. To try other limits, configure with
e.g. `-DGX_NUM_TRACKS=16 -DGX_NUM_PATTERNS=16 -DGX_MAX_STEPS=64`. Those are the core's MCU
defaults, which `check_mcu` also uses.

## Simulator

Controls: click = press a pad or button (held while the mouse button is down), right-click =
latch a pad down until you right-click it again, drag a fader or roll the mouse wheel over it = move it (hold Ctrl for fine steps), drag a mixer knob up or down to turn it
(about 160 pixels covers its range), F1–F8 = R1–R8, Space = R8 (play), 1–8 = B1–B8,
Shift = SHIFT, Esc = quit. Use right-click latching for anything needing several pads at once,
such as holding a chord and pressing a step.

While Shift is held, the window relabels R1–R8 and B1–B8 with what they do with Shift in the
current mode (e.g. GLOBAL, SCALE, PARAMS, PROB; pattern or track pages; piano roll scrolling).
While a held R button turns B1–B8 into page buttons, they are labelled with the pages: PG1–PG8
holding Shift, R1 or R4 in PROJECT or PRESET mode, and the steps each page shows (S1-32, S33-64, …)
holding R3 in NOTE, R4 in PARAMS or R5 in PROBABILITY mode.

To help learn the settings modes, the window also prints on the pads what they do: note and
scale names and the keyboard pads in SCALE mode, and the settings, +1/−1 and channel numbers
in GLOBAL mode. The labels come from the core (`Mode::padLabel`); the APC can't show them.

Playback runs on its own clock thread, ticking every millisecond at real-time priority when
the system allows (the simulator prints which it got), so its timing doesn't depend on the
window's frame rate.

**Only one simulator runs at a time.** It takes two locks: one on its data directory, so two
of them can't overwrite each other's projects, and one for the machine's controllers, because
a second instance would subscribe to the same APC and MIDI Mix and answer every press
alongside the first. A second start refuses with the other one's pid; `--no-apc` skips the
controller lock and `--allow-multiple` skips both. The locks are files the kernel releases
when the process ends, so a crash leaves nothing to clean up.

**Pad colours go out in the device's palette mode**: a Note On whose channel is the brightness
(0x90 is 10%, 0x96 100%) and whose velocity picks one of its 128 fixed colours. Three bytes a
pad against eight inside an RGB SysEx, so a full repaint is 192 bytes rather than ~570. The
surface matches each UI colour by hue — always taking the brightest palette entry of that hue,
since the device multiplies the entry by the channel's brightness — and then picks the channel
nearest the brightness the UI asked for. Hue in the velocity, brightness in the channel.

**The APC's own pad modes.** Shift + R6 and Shift + R7 on the panel are DRUM and NOTE, handled
by the device's firmware: it takes the grid for itself and never passes those presses on, and
the same buttons bring it back, like any mode button. Nothing of ours may live there, which is
why the song sits on Shift + R8 and there are four pattern pages rather than eight — four of each
fits the bottom row, which is where Pattern mode puts them.

Hardware surfaces are picked up automatically when they are plugged in: an **APC mini mk2**
for the grid (client `GroovixBox Surface`) and an **Akai MIDI Mix** for the mixer (client
`GroovixBox Mixer`). Both are portable `platform/common` code over a `MidiPort`, so a hardware
build uses the same classes. `--no-apc` ignores both.

The sequencer's MIDI goes out of eight ALSA sequencer ports named **P1–P8**, one per MIDI
port, so a track's port decides which socket its notes leave by. Every port also carries
**MIDI clock** — one per engine tick, which is 6 ticks a step and 4 steps a beat, exactly the
24 per quarter note MIDI clock wants — bracketed by Start and Stop
(`MidiEventSink::setClockPorts` can keep a port quiet). They are created whether or
not anything is connected, so `aconnect` (or a patchbay) can wire them up at any time;
`--midi-out NAME` connects one device's inputs to P1, P2… in order. The control surface is a
separate client, `GroovixBox Surface`, which finds an APC mini mk2 by name.

Projects, presets and the last open project are kept in the **data directory**, which is
`/mnt/usb1/data` by default — the USB stick in a built rig, and the same pair of directories
the [settings page](settings/README.md) edits. The open project is saved on quit. Project
files are sparse — only what differs from a new project is written — so a four-bar beat is a
few hundred bytes. The format is at version 8; files from versions 1–7 still open, and what
they don't carry (scenes, the song, a scene's patterns, swing, micro-timing, the chord a key
plays, a step's ratchet and a track's arpeggiator) comes up empty, straight, off, once, or
"leave it alone".

| Option | |
|---|---|
| `--data DIR` | keep projects and presets in `DIR` instead (default `/mnt/usb1/data`, or `$GXBOX_DATA_DIR`) |
| `--config DIR` | read `controls.conf` and `instruments.conf` from `DIR`, so the rig's settings can live apart from its projects — in `/etc`, beside your dotfiles, or on read-only media (default `/mnt/usb1/config`, or `$GXBOX_CONFIG_DIR`) |
| `--controls FILE` | read the control map from `FILE`, naming it outright (default: `controls.conf` in the config directory) |
| `--instruments FILE` | read the instrument slots from `FILE`, naming it outright (default: `instruments.conf` in the config directory) |
| `--audio BACKEND` | `alsa`, `jack` or `none` (default): where instruments I1–I16 play |
| `--audio-device NAME` | ALSA device, e.g. `hw:0` (default `default`) |
| `--rate HZ` / `--block FRAMES` | what to ask the device for (default 48000, 128) |
| `--allow-multiple` | run beside another simulator (for headless screenshots); by default a second one refuses to start |
| `--midi-out DEVICE` | connect ports P1, P2… to the inputs of the first ALSA client whose name contains `DEVICE`, in the order it lists them (a 4×4 interface becomes P1–P4) |
| `--midi-log` | print the MIDI the sequencer sends. Bank Select and Program Change are spelled out rather than left as CC numbers, and the Program Change line names the preset pad the bank and program add up to:<br>`MIDI P1 ch1  bank MSB (CC  0)   1`<br>`MIDI P1 ch1  bank LSB (CC 32)   0`<br>`MIDI P1 ch1  program   0  preset 3.01 (bank 1, program 0)` |
| `--screenshot FILE.bmp` | render a few frames to a BMP and exit without saving (`SDL_VIDEODRIVER=offscreen` works headless) |
