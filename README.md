# Groovebox

A groovebox for the Shanling Q2, as a card app: sixteen pads, a step sequencer with parameter
locks, a sampler with punch-in effects, echo and reverb, and the DAC you already carry. One
screen, five modes, no menus.

It needs Q2 Pod 1.7.2 or later for its sound and media keys. Unzip `groovebox.zip` to the card's
root (it holds one folder, `.groovebox`). It then appears under Games and as the **Groovebox**
choice of the Shortcut setting.

Sound goes to whatever Q2 Pod is playing through: the headphone jack, Bluetooth or a USB DAC. On
USB it opens the first USB-Audio card at 44.1 kHz 16-bit. The volume starts where Q2 Pod's was.

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
| Return tap | Undo the last recorded take; in PATT, release a chain; in FX with nothing to undo, clear the pattern's effects |
| Return hold | Quit (the firmware's) |
| Wheel | The value the strip names; with a pad held, that pad's |
| Strip | Tap the play state, the tempo (the wheel turns it) or the volume. In PATT, tap the name to export |

| Mode | Pads | Tap | Hold + wheel | Wheel |
| --- | --- | --- | --- | --- |
| PLAY | the kit's sixteen sounds | play (and record, quantised, with REC on while playing) | that sound's pitch | filter sweep, low-pass one way, high-pass the other |
| WRITE | the selected sound's sixteen steps | toggle the step | lock the strip's parameter on that step (tap the name to cycle Level, Pitch, Decay, Cutoff, Nudge) | that parameter for the whole track; Nudge's wheel is the track length |
| SOUND | the sound's sixteen parameters | select | turn it | the selected one |
| FX | twenty-seven punch-in effects on two pages | hold to run it; Centre hold + pad latches it until the next tap; with REC on it is recorded, quantised | | the page |
| PATT | sixteen patterns | cue it for the next bar | hold one to chain it with the current pattern (Return releases the chain) | tempo |

The effects are SLOOP's, time-synced to the tempo. Page 1: LOOP 4, LOOP 8, LOOP 16, LOOP 32,
STUTTER, REVERSE, STOP, HALF, LOW, HIGH, CRUSH, GATE, ECHO, KILL LOW, KILL MID, KILL HIGH. Page 2:
PHONE, ALIAS, WOBBLE, REWIND, OCT UP, FLANGER, AUTO-PAN, FREEZE, LOOP 8T, MONO, FADE OUT. Echo
and Reverb in SOUND are that sound's sends to a tempo delay and a reverb.

### Kits and Load

SOUND's Load tile opens a picker over the pads: the wheel scrolls, Centre opens or picks, Return
backs out, and a tap on a row picks it. Its first rows choose a kit, which replaces the sounds and
keeps your patterns and tiles. **default**, **909**, **808** and **melodic** ship with the app; a
kit is a folder `.groovebox/kits/<name>/` of WAVs named `NN Name.wav`, and the first sixteen fill
the pads.

Below the kits are the card's folders and audio files, in any format the Q2 plays. A picked file
can be chopped into sixteen equal slices across the kit, or go to the selected pad from one of
fifteen start points, twenty seconds of it at most. Trim with Start and End. Loaded sounds are
kept with the project, and a kit change clears them.

### Export

In PATT, tap the name on the strip. The current pattern, or both patterns of a chain, is rendered
to `Music/Groovebox/1-<n>.wav`, 44.1 kHz 16-bit stereo, without the volume or the sweep. It plays
in the player and survives Remove; Load it to resample.

Projects live in `.groovebox/projects/` on the card and survive an update; Remove deletes them.

## Building

The device binary links the Q2's own glibc 2.28, libm, libpthread and libasound through a sysroot
made by Rockbox's `tools/shanlingq2/mksysroot.sh` from the stock firmware zip; `make
SYSROOT=path` builds `dist/.groovebox/` and `dist/groovebox.zip`. `make host` builds an SDL2
window (mouse = finger, scroll = wheel, Enter = Centre, Backspace = Return, Space = Play, `[` `]` =
Prev/Next), and `--script FILE OUTDIR` replays input lines and writes frames (`tests/beat.txt` walks
every mode; `GROOVEBOX_MEDIA` is the folder Load browses). `make check` runs the asserts, and
requires the lifted effects to match SLOOP's own output. `tools/upstream.sh` regenerates what comes
from upstream at the pinned commits: the tables, those hashes and the kits.

## Credits

See [CREDITS.md](CREDITS.md). GPL-3.0-only.
