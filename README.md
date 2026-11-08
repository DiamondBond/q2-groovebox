# Groovebox

A groovebox for the Shanling Q2, as a card app: sixteen pads, each a sample, a synth or a live
909, 808, 303 or break; a step sequencer with parameter locks, ratchets, chance and songs;
punch-in effects, echo and reverb; and the DAC you already carry. One screen, six modes.

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
 ├────┼────┼────┼────┤ │ KEYS  │
 │  9 │ 10 │ 11 │ 12 │ │ WRITE │
 ├────┼────┼────┼────┤ │ SOUND │
 │  5 │  6 │  7 │  8 │ │  FX   │
 ├────┼────┼────┼────┤ │ PATT  │
 │  1 │  2 │  3 │  4 │ └───────┘
 └────┴────┴────┴────┘
```

Pad 1 is bottom-left. The pads stay where they are; the mode says what they mean.

| Control | Does |
| --- | --- |
| Play/Pause | Play / stop |
| Prev / Next | Previous / next pattern, at its end (at once when stopped). In WRITE, the page of steps |
| Centre tap | REC on / off |
| Centre hold + wheel | Volume (swing in PATT) |
| Centre hold + pad | PLAY: select a sound without playing it. PATT: copy the current pattern into that slot |
| Return tap | Undo the last recorded take or generated line; in PATT, take the last pattern off the song; in FX with nothing to undo, clear the pattern's effects |
| Return hold | Quit (the firmware's) |
| Wheel | The value the strip names; with a pad held, that pad's |
| Strip | Tap the play state, the tempo (the wheel turns it) or the volume. Tap the name: WRITE's parameter, KEYS's key or scale, PATT's export |
| A tab, tapped again | WRITE's or SOUND's second layer: `WRITE 2`, `SOUND 2` |

| Mode | Pads | Tap | Hold + wheel | Wheel |
| --- | --- | --- | --- | --- |
| PLAY | the kit's sixteen sounds | play (and record, quantised, with REC on while playing) | that sound's pitch | filter sweep, low-pass one way, high-pass the other |
| KEYS | the selected sound's notes in the key and scale, or a break's slices and tricks | play, until you lift; with REC on while playing it lands on the step, its pitch kept | | the key (tap the name for the scale) |
| WRITE | the selected sound's steps, sixty-four on four pages | toggle the step | lock the strip's parameter on that step (tap the name to cycle Level, Pitch, Decay, Cutoff, Nudge) | that parameter for the whole track; Nudge's wheel is the track length |
| SOUND | the sound's sixteen parameters | select | turn it | the selected one |
| FX | twenty-seven punch-in effects on two pages, then the bus | hold to run it; Centre hold + pad latches it until the next tap; with REC on it is recorded, quantised | on the bus page, that setting | the page |
| PATT | sixteen patterns | cue it for the pattern's end | hold one to add it to the song | tempo |

The effects are SLOOP's, time-synced to the tempo. Page 1: LOOP 4, LOOP 8, LOOP 16, LOOP 32,
STUTTER, REVERSE, STOP, HALF, LOW, HIGH, CRUSH, GATE, ECHO, KILL LOW, KILL MID, KILL HIGH. Page 2:
PHONE, ALIAS, WOBBLE, REWIND, OCT UP, FLANGER, AUTO-PAN, FREEZE, LOOP 8T, MONO, FADE OUT. Echo
and Reverb in SOUND are that sound's sends to a tempo delay and a reverb.

FX's third page is the bus. Its first tile sends Echo and Reverb to SLOOP's delay and reverb or to
X0X's, and Dist, Drive and Comp work on the whole mix with either.

### WRITE's second layer

Tap WRITE's tab again and the pads edit what a step does rather than whether it sounds. Tap the name
on the strip to cycle:

| Layer | Tap a step | Hold it and turn |
| --- | --- | --- |
| Ratchet | x2, x3, x4, then once | up to x8 |
| Chance | 75, 50, 25 percent, then always | in fives |
| Accent, Slide | on / off | |
| Rate | | the wheel alone also sets the track's rate, 1/32 to 1/4 with the triplets |

A step written on a later page brings the track out to that page. Tracks of different lengths and
rates drift against each other, and a pattern plays until its longest track ends.

### Kits, sounds and Load

SOUND's Load tile opens a picker over the pads: the wheel scrolls, Centre opens or picks, Return
backs out, and a tap on a row picks it. Each row but the kits and the phone gives the selected pad a
new sound:

- **Synth**: the engines ANALOG, LOFI, TRIO, WHEEL, NOISE and PHASE, and their presets.
- **909 drum**, **808 drum**: any of the sixteen sounds of those machines, modelled as it plays.
- **303 bass**: an acid voice with slide and accent.
- **Break**: a loop cut into eight slices and rearranged as it plays, in time with the tempo: a
  built-in groove or fill, or the pad's own sample.
- **From phone**: see below.

SOUND's tab, tapped again, shows a machine's own controls. The wheel on Preset walks a synth's
presets. On a synth or the 303, **New** writes a generated line into the current pattern in the key
and scale KEYS shows, its wheel setting the density, **Mutate** changes a few of its steps, and
Return takes the line back.

Then come the kits, which replace all sixteen sounds and keep your patterns and tiles. **default**,
**909**, **808** and **melodic** are samples, **909 Live** and **808 Live** the machines; a kit is
a folder `.groovebox/kits/<name>/` of WAVs named `NN Name.wav`, and the first sixteen fill the pads.

Last are the card's folders and audio files, in any format the Q2 plays. A picked file can be
chopped across the kit at its hits or into sixteen equal slices, become a break on the selected
pad, or go to that pad from one of fifteen start points, twenty seconds of it at most. Trim with
Start and End. Loaded sounds are kept with the project, and a kit change clears them.

### From your phone

**From phone** shows an address. With the Q2 and the phone on the same Wi-Fi (the phone's own
hotspot works), open it in the phone's browser and choose a recording, up to 64 MB. It is saved
to `Music/Groovebox/Drops`, and the Q2 goes straight to the choices above. The page exists only
while that screen is open, under the code in its address.

### Songs

In PATT, hold a pattern to add it to the song; the first one starts from the pattern playing. Pads
show their places in the song, and Return takes the last off. With two or more entries the patterns
play in turn and loop. A cued pattern plays once in between, and the song then carries on.

### Export

In PATT, tap the name on the strip. The current pattern, or the whole song, is rendered to `Music/Groovebox/1-<n>.wav`, 44.1 kHz 16-bit stereo, without the volume or the sweep. It plays
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
