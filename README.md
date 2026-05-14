# deadbeef-bpmkey

Background BPM and musical-key detection plugin for [DeaDBeeF](https://deadbeef.sourceforge.io/). Analyzes every track in your playlists, writes `bpm` and `key` tags to the database (and optionally to file tags), and emits track-info-changed events so columns refresh live.

- BPM via [aubio](https://aubio.org/)
- Key via [libKeyFinder](https://github.com/mixxxdj/libKeyFinder)
- Multi-threaded worker pool, deduplicated URI queue, incremental scan, persistent tag write-back

## Build

Dependencies (Arch package names in parentheses):

- DeaDBeeF headers (`deadbeef`)
- aubio (`aubio`) — pkg-config name `aubio`
- libKeyFinder (`libkeyfinder`) — pkg-config name `libkeyfinder`
- A C and C++ compiler, GNU make, pkg-config

```sh
make
make install PREFIX=$HOME/.local      # user install -> ~/.local/lib/deadbeef/
# or
sudo make install                      # system install -> /usr/local/lib/deadbeef/
```

## Usage

Restart DeaDBeeF. The plugin starts a worker pool and queues every track that's missing `bpm` or `key` tags.

In **View → Design Mode → right-click column header → Add column**, set:

- Title: `BPM`, format: `%bpm%`
- Title: `Key`, format: `%key%`

### CLI

```sh
deadbeef --plugin=bpmkey status    # progress + queue
deadbeef --plugin=bpmkey scan      # incremental (only missing tags)
deadbeef --plugin=bpmkey rescan    # force re-analyze everything
deadbeef --plugin=bpmkey scandir /path/to/dir   # ad-hoc folder
```

### Configuration

In Edit → Preferences → Plugins → BPM and Key Detector, or in `deadbeef.conf`:

| Key | Default | Description |
| --- | --- | --- |
| `bpmkey.threads` | `1` | Worker threads (cap 32). One is usually enough; FFTW key analysis is serialized internally. |
| `bpmkey.write_tags` | `1` | Persist `bpm`/`key` to file tags via the decoder's `write_metadata`. |
| `bpmkey.skip_existing` | `1` | Skip tracks that already have both tags. |
| `bpmkey.scan_on_start` | `1` | Enqueue all missing tracks on plugin load. |
| `bpmkey.reactive` | `1` | Re-scan when playlists change. |

## Output format

- **BPM**: numeric, one decimal place (e.g. `123.5`).
- **Key**: a short tag using sharps for some, flats for others (KeyFinder's convention): `C`, `Cm`, `Db`, `Dbm`, `D`, `Dm`, `Eb`, `Ebm`, `E`, `Em`, `F`, `Fm`, `Gb`, `Gbm`, `G`, `Gm`, `Ab`, `Abm`, `A`, `Am`, `Bb`, `Bbm`, `B`, `Bm`. Major = bare letter; minor = letter + `m`. `F#` is reported as `Gb`, `C#` as `Db`, etc.

## Accuracy expectations

These are inherent limits of aubio's beat tracker and libKeyFinder's key model, not bugs in the plugin. See [TEST_REPORT.md](TEST_REPORT.md) for the methodology and results.

**BPM**: On a 20-track sample of pop/rock/electronic with well-documented canonical BPMs, **60% landed within ±3 BPM** and **75% within ±10 BPM**. Failures concentrate on ballads, syncopated grooves, and reggae backbeats where aubio locks to a sub-pattern.

**Key**: On a 15-track sample of solo classical (piano + cello — the hardest case), **40% matched the tonic exactly** but **all 15 fell on a Camelot-adjacent key** (relative major/minor, dominant, or enharmonic). libKeyFinder is trained for harmonic pop/EDM and is more accurate on material with drums and bass.

Treat detected tags as a useful first pass for sorting/DJ workflows, not as authoritative ground truth.

### Optional: better BPM via ML

If aubio's accuracy isn't good enough on parts of your library, [`tools/bpmkey-ml-rescan.py`](tools/) is a standalone batch tagger using `madmom` or `beat-this`. It writes the `BPM` tag directly to your files; the plugin's `skip_existing=1` leaves them alone afterward. **Opt-in** — not loaded by DeaDBeeF and not a dependency of the AUR package. See [tools/README.md](tools/README.md).

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
