# Optional tools

These companions are separate command-line programs. DeaDBeeF does not load
them, and the core `bpmkey` plugin does not require them.

## `bpmkey-test`

`bpmkey-test` is a standalone inspection harness for the plugin's analysis
chain. It builds with the same aubio parameters and `keyfinder_shim.cpp` used
by the plugin, decodes each input through `ffmpeg` to mono float32 at 44.1 kHz,
and writes tab-separated results.

```sh
make
./bpmkey-test /path/to/track.flac /path/to/another-track.ogg
```

Requirements: `aubio`, `libKeyFinder`, C and C++ compilers, GNU Make,
`pkg-config`, and `ffmpeg` on `PATH` when you run the harness.

The output columns are `path`, `bpm`, `key`, and `bpm_confidence`. A `-` means
that the harness did not produce that value for the input. The harness is used
by the repository's [test report](../TEST_REPORT.md); see that document for
the sample, parameters, and limitations.

## `bpmkey-ml-rescan.py`

`bpmkey-ml-rescan.py` is an opt-in batch BPM tagger. It decodes supported audio
files with `ffmpeg`, uses one selected ML backend to estimate BPM, and writes
the result with `mutagen`. It never runs automatically and does not change key
metadata.

The script accepts `flac`, `mp3`, `m4a`, `aac`, `ogg`, `opus`, `wav`, `wv`, and
`ape` paths. Directory arguments are traversed recursively. Existing BPM tags
are skipped unless you supply `--force`.

### Requirements

- Python 3
- `ffmpeg` on `PATH`
- `mutagen`
- one backend: `madmom` or `beat-this`

Install the dependencies in an isolated environment when possible. The exact
backend packages and their transitive requirements are maintained upstream.

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install mutagen madmom

# Or use the beat-this backend instead.
pip install mutagen beat-this torch
```

`beat-this` uses CUDA when its installed PyTorch runtime reports that a CUDA
device is available; otherwise the script requests CPU execution.

### Usage

```sh
# Process a directory, leaving existing BPM tags in place.
./bpmkey-ml-rescan.py --backend madmom /path/to/music

# Recalculate and overwrite BPM metadata for one file.
./bpmkey-ml-rescan.py --backend beat-this --force /path/to/track.flac

# Calculate without writing file metadata.
./bpmkey-ml-rescan.py --backend madmom --dry-run /path/to/music

# Set an accepted BPM range (defaults: 40–240).
./bpmkey-ml-rescan.py --backend madmom --min-bpm 60 --max-bpm 200 /path/to/music
```

`--dry-run` still decodes files and runs the selected estimator; it simply
leaves metadata unchanged. Without `--dry-run`, this tool writes file tags.
Back up your library if you need to preserve existing metadata exactly.

### Workflow with the plugin

When `bpmkey.skip_existing=1` (the default), the plugin skips only tracks that
already have **both** `bpm` and `key`. A track tagged only with BPM remains
eligible for plugin analysis so it can receive a key, and that analysis can
replace its BPM value. To preserve an ML BPM estimate under the default policy,
run this tool after the track already has a key.
