# Groovebox

A groovebox for the Shanling Q2, as a card app: sixteen pads, a step sequencer with parameter
locks, a sampler, and the DAC you already carry. One screen, five modes, no menus.

Install it from **Settings → System → App Store** on Q2 Pod. It then appears under Games and as
the **Groovebox** choice of the Shortcut setting.

## The stage

```
 strip:  ▶/■  REC●  120  P3   [what the wheel turns]   meter  volume
 ┌────┬────┬────┬────┐ ┌───────┐
 │ 13 │ 14 │ 15 │ 16 │ │ PLAY  │
 ├────┼────┼────┼────┤ ├───────┤
 │  9 │ 10 │ 11 │ 12 │ │ WRITE │
 ├────┼────┼────┼────┤ ├───────┤
 │  5 │  6 │  7 │  8 │ │ SOUND │
 ├────┼────┼────┼────┤ ├───────┤
 │  1 │  2 │  3 │  4 │ │  FX   │
 └────┴────┴────┴────┘ ├───────┤
                       │ PATT  │
                       └───────┘
```

Pad 1 is bottom-left. The pads stay where they are; the mode says what they mean.

| Control | Does |
| --- | --- |
| Play/Pause | Play / stop |
| Prev / Next | Previous / next pattern, on the next bar (at once when stopped) |
| Centre tap | REC on / off |
| Centre hold + wheel | Volume (swing in PATT) |
| Centre hold + pad | PLAY: select a sound without playing it. PATT: copy the current pattern into that slot |
| Return tap | Undo the last recorded take; in PATT, release a chain |
| Return hold | Quit (the firmware's) |
| Wheel | The value the strip names; with a pad held, that pad's |
| Strip | Tap the play state, the tempo (the wheel turns it) or the volume |

| Mode | Pads | Tap | Hold + wheel | Wheel |
| --- | --- | --- | --- | --- |
| PLAY | the kit's sixteen sounds | play (and record, quantised, with REC on while playing) | that sound's pitch | filter sweep, low-pass one way, high-pass the other |
| WRITE | the selected sound's sixteen steps | toggle the step | lock the strip's parameter on that step (tap the name to cycle Level, Pitch, Decay, Cutoff, Nudge) | that parameter for the whole track; Nudge's wheel is the track length |
| SOUND | the sound's sixteen parameters | select | turn it | the selected one |
| FX | punch-in effects | coming | | |
| PATT | sixteen patterns | cue it for the next bar | hold one to chain it with the current pattern (Return releases the chain) | tempo |

Projects live in `.groovebox/projects/` on the card and survive an update; Remove deletes them.

## Building

The device binary links the Q2's own glibc 2.28, libm, libpthread and libasound through a sysroot
made by Rockbox's `tools/shanlingq2/mksysroot.sh` from the stock firmware zip; `make
SYSROOT=path` builds `dist/.groovebox/` and `dist/groovebox.zip`. `make host` builds an SDL2
window (mouse = finger, scroll = wheel, Enter = Centre, Backspace = Return, Space = Play, `[` `]` =
Prev/Next), and `--script FILE OUTDIR` replays input lines and writes frames. `make check` runs
the asserts.

## Credits

See [CREDITS.md](CREDITS.md). GPL-3.0-only.
