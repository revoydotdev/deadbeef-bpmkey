# tools/

Optional, opt-in companions to the `bpmkey` plugin. **None of these are loaded by DeaDBeeF.** They're standalone CLIs you run yourself.

## `bpmkey-test`

Standalone accuracy tester. Links the same `keyfinder_shim.cpp` and the same analyzer parameters the plugin uses, decodes via ffmpeg, prints TSV results. Used to produce [`../TEST_REPORT.md`](../TEST_REPORT.md).

```sh
make            # builds ./bpmkey-test
./bpmkey-test file1.flac file2.ogg ...
```

Deps: aubio, libkeyfinder, gcc/g++.

## `bpmkey-ml-rescan.py`

Batch BPM tagger using ML backends. Run this when aubio's accuracy isn't good enough on a corner of your library (ballads, syncopated grooves, half-time). Writes the `BPM` tag directly to the file via mutagen; the plugin's `skip_existing=1` will then leave those tags alone on subsequent scans.

**This script is opt-in. Nothing in the AUR package or the plugin depends on it.** Install the ML backend yourself.

### Backends

| Backend | Accuracy (rough) | Install size | GPU? |
| --- | --- | --- | --- |
| `madmom` | ~85% within ±3 BPM | ~200 MB | CPU only |
| `beat-this` | ~90%+ within ±3 BPM | ~2 GB (with CUDA) | optional GPU |

### Install

```sh
# madmom — CPU-only, lighter
pip install --user madmom mutagen
# Note: madmom pins old numpy; consider a venv:
python -m venv ~/.venvs/bpmkey-ml && source ~/.venvs/bpmkey-ml/bin/activate
pip install madmom mutagen

# beat-this — bigger, optional GPU
pip install --user beat-this mutagen torch
```

Plus `ffmpeg` on your `$PATH` (you have it already if you're running DeaDBeeF).

### Usage

```sh
# Tag everything under a directory using madmom (skips files that already have BPM)
./bpmkey-ml-rescan.py --backend madmom /mnt/games/Music

# Force overwrite using beat-this
./bpmkey-ml-rescan.py --backend beat-this --force song.flac

# Preview without writing
./bpmkey-ml-rescan.py --backend madmom --dry-run /mnt/games/Music/Daft\ Punk

# Multiple files / dirs accepted
./bpmkey-ml-rescan.py --backend madmom album1/ album2/ track.flac
```

### Why no key detection here?

ML key models exist (Korzeniowski/Widmer is the well-known one) but the accuracy delta over libKeyFinder on real-world libraries is small relative to the added install weight. The plugin's `key` tag from libKeyFinder is good enough; the ML investment pays off mainly for BPM.

### Workflow

```
Add new tracks to library
        │
        ▼
DeaDBeeF starts, plugin enqueues new tracks
        │
        ▼
aubio + libKeyFinder analyze (fast, "good enough")
        │
        ▼
(later, when you want better BPM)
        │
        ▼
bpmkey-ml-rescan.py --backend ... /path
        │
        ▼  (writes BPM tag directly to files)
        │
DeaDBeeF reads tags; plugin's skip_existing=1 leaves them alone
```
