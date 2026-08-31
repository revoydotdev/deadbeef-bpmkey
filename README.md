<div align="center">

# bpmkey

### BPM and musical-key analysis for DeaDBeeF

[DeaDBeeF](https://deadbeef.sourceforge.io/) plugin · [aubio](https://aubio.org/) tempo analysis · [libKeyFinder](https://github.com/mixxxdj/libKeyFinder) key analysis · [GPL-3.0-or-later](LICENSE)

</div>

`bpmkey` is a small native plugin that analyzes the tracks in your DeaDBeeF
playlists in the background and adds `bpm` and `key` metadata. It is meant to
make a music library easier to sort, browse, and prepare—not to replace a
careful musical judgment.

| It provides | How it works |
| --- | --- |
| Background analysis | A configurable worker pool queues distinct track URIs across the current playlists. |
| BPM metadata | [aubio](https://aubio.org/)'s tempo tracker produces a one-decimal BPM value when it has a valid result. |
| Key metadata | [libKeyFinder](https://github.com/mixxxdj/libKeyFinder) estimates a major or minor key from decoded audio. |
| Live library updates | The plugin updates DeaDBeeF track metadata, notifies the player so columns refresh, and can ask the track decoder to write the new metadata back to the file. |

## Install

### Requirements

You need a DeaDBeeF installation with development headers, plus:

- `aubio` (pkg-config package: `aubio`)
- `libKeyFinder` (pkg-config package: `libkeyfinder`)
- a C compiler, a C++ compiler, GNU Make, and `pkg-config`

On Arch-based systems, the relevant package names are typically `deadbeef`,
`aubio`, and `libkeyfinder`. Other distributions may split DeaDBeeF's runtime
and development headers into separate packages.

Check that the analysis libraries are visible to the build, then compile:

```sh
pkg-config --modversion aubio libkeyfinder
make
```

Install for the current user or system-wide:

```sh
# User installation: ~/.local/lib/deadbeef/bpmkey.so
make install PREFIX="$HOME/.local"

# System installation: /usr/local/lib/deadbeef/bpmkey.so
sudo make install
```

To remove the same installation later, repeat the prefix you used:

```sh
make uninstall PREFIX="$HOME/.local"
```

Restart DeaDBeeF after installing the shared object. The plugin identifies
itself as **BPM and Key Detector** in the Plugins preferences.

## First scan

With the defaults, bpmkey starts its workers when DeaDBeeF loads plugins and
queues tracks that are missing either `bpm` or `key`. It scans the main track
list of every playlist, while avoiding duplicate URIs during that pass.

Add visible columns in **View → Design Mode**: right-click a column header,
choose **Add column**, then use these title-format strings:

| Column | Format |
| --- | --- |
| BPM | `%bpm%` |
| Key | `%key%` |

The command-line interface can inspect or start a scan from a DeaDBeeF
process:

```sh
deadbeef --plugin=bpmkey status              # queue, completed count, and workers
deadbeef --plugin=bpmkey scan                # analyze tracks missing a tag
deadbeef --plugin=bpmkey rescan              # force analysis of all playlist tracks
deadbeef --plugin=bpmkey scandir /path/to/dir # add a "bpmkey-scan" playlist and force a scan
```

`scandir` changes the playlist collection by creating a playlist named
`bpmkey-scan` and importing the supplied directory. Use `status` when you only
want to inspect progress.

## Configuration

Configure the plugin in **Edit → Preferences → Plugins → BPM and Key
Detector**, or set the following keys in `deadbeef.conf`.

| Key | Default | Effect |
| --- | ---: | --- |
| `bpmkey.threads` | `1` | Number of workers, clamped to 1–32. Audio decoding and BPM analysis can run concurrently; key analysis is serialized because of libKeyFinder/FFTW plan creation. |
| `bpmkey.write_tags` | `1` | Ask the decoder to persist successfully detected `bpm` and/or `key` metadata to the source file. Set to `0` for DeaDBeeF metadata only. |
| `bpmkey.skip_existing` | `1` | Skip a track during ordinary scans when it already has both tags. |
| `bpmkey.scan_on_start` | `1` | Queue an ordinary scan after DeaDBeeF has loaded plugins. |
| `bpmkey.reactive` | `1` | Queue an ordinary scan when a playlist changes. |

## Tag format and limits

- **BPM** is stored with one decimal place, for example `123.5`.
- **Key** uses the spellings returned by the plugin's libKeyFinder mapping:
  `C`, `Cm`, `Db`, `Dbm`, `D`, `Dm`, `Eb`, `Ebm`, `E`, `Em`, `F`, `Fm`, `Gb`,
  `Gbm`, `G`, `Gm`, `Ab`, `Abm`, `A`, `Am`, `Bb`, `Bbm`, `B`, and `Bm`.
  A bare note is major; `m` is minor. Enharmonic spellings such as `F#` and
  `C#` are represented as `Gb` and `Db`.
- Tracks shorter than roughly one second, tracks unsupported by an available
  DeaDBeeF decoder, or tracks for which an analyzer cannot produce a valid
  value can remain partially or wholly untagged.
- File write-back depends on the selected DeaDBeeF decoder supporting
  `write_metadata`; retain backups if you do not want audio-file tags changed.

Tempo and key estimation are fallible, especially for material with unstable
tempo, sparse percussion, or ambiguous harmony. Treat the results as useful
starting metadata rather than canonical facts. The included
[test report](TEST_REPORT.md) records the project's limited, reproducible
sample and its methodology; it is evidence for that sample, not a general
accuracy guarantee.

## Optional ML BPM companion

[`tools/bpmkey-ml-rescan.py`](tools/bpmkey-ml-rescan.py) is an opt-in batch
tagger for cases where you want to try a separate ML BPM estimator. It is not
loaded by DeaDBeeF, is not required to build this plugin, and writes `BPM`
metadata directly to the files it processes. See [the tools guide](tools/README.md)
for its dependencies, supported backends, and usage. The plugin's ordinary
scan skips only tracks that already have **both** `bpm` and `key`; run the
companion after a key is present if you do not want a later plugin scan to
replace its BPM value.

## Build and developer checks

```sh
make                # build bpmkey.so
make -C tools       # build the standalone analyzer harness
python3 -m py_compile tools/bpmkey-ml-rescan.py
python3 tools/bpmkey-ml-rescan.py --help
```

The standalone harness accepts audio-file paths and prints tab-separated
measurements:

```sh
./tools/bpmkey-test /path/to/track.flac
```

It additionally requires `ffmpeg` to decode its input. See
[TEST_REPORT.md](TEST_REPORT.md) for the exact analysis parameters and sample
results.

## License

This project is licensed under the [GNU General Public License, version 3 or
later](LICENSE). DeaDBeeF, aubio, libKeyFinder, and their respective names are
their upstream projects' property; this repository is an independent plugin
that uses their published interfaces and libraries.
