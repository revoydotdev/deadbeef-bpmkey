#!/usr/bin/env python3
"""
bpmkey-ml-rescan: batch BPM tagging with ML backends.

Companion to the deadbeef-bpmkey plugin. Reads audio files, runs a selectable
ML backend (madmom or beat-this) to estimate BPM, writes the result to the
file's BPM tag via mutagen. The plugin's `skip_existing=1` setting (default)
will then respect these tags and not overwrite them with aubio's estimate.

This script is NOT loaded into DeaDBeeF — it's a separate batch tool you run
when you want better BPM accuracy than aubio provides. Key detection is left
to the plugin (libKeyFinder); ML key models aren't a clear win here.

Examples:
  # Tag every flac under a directory using madmom
  bpmkey-ml-rescan.py --backend madmom /mnt/games/Music

  # Force re-tag (overwrite existing BPM) using beat-this
  bpmkey-ml-rescan.py --backend beat-this --force song.flac

  # Dry-run, print what would be written
  bpmkey-ml-rescan.py --backend madmom --dry-run track.mp3
"""

from __future__ import annotations
import argparse
import os
import subprocess
import sys
from pathlib import Path

AUDIO_EXTS = {'.flac', '.mp3', '.m4a', '.aac', '.ogg', '.opus', '.wav', '.wv', '.ape'}
SR_MADMOM = 44100
SR_BEAT_THIS = 22050


def decode_to_mono_f32(path: Path, sr: int) -> 'numpy.ndarray':
    """Decode any ffmpeg-readable file to mono float32 PCM at the target rate."""
    import numpy as np
    cmd = [
        'ffmpeg', '-nostdin', '-v', 'error',
        '-i', str(path),
        '-f', 'f32le', '-ac', '1', '-ar', str(sr),
        '-',
    ]
    result = subprocess.run(cmd, capture_output=True, check=True)
    return np.frombuffer(result.stdout, dtype=np.float32)


# ---- backends ----

class MadmomBackend:
    name = 'madmom'
    sr = SR_MADMOM

    def __init__(self):
        # madmom is heavy to import; defer until first use.
        from madmom.features.beats import RNNBeatProcessor, DBNBeatTrackingProcessor
        from madmom.features.tempo import TempoEstimationProcessor
        self._rnn = RNNBeatProcessor()
        self._tempo = TempoEstimationProcessor(fps=100)

    def bpm(self, samples) -> float:
        activations = self._rnn(samples)
        tempi = self._tempo(activations)
        # Returns array of (bpm, strength); take the strongest.
        if len(tempi) == 0:
            raise RuntimeError('no tempo detected')
        return float(tempi[0][0])


class BeatThisBackend:
    name = 'beat-this'
    sr = SR_BEAT_THIS

    def __init__(self):
        # beat_this exposes a File2Beats helper; we do raw arrays instead.
        from beat_this.inference import Audio2Beats
        # Auto-pick GPU if available, else CPU.
        try:
            import torch
            device = 'cuda' if torch.cuda.is_available() else 'cpu'
        except ImportError:
            device = 'cpu'
        self._infer = Audio2Beats(checkpoint_path='final0', device=device, dbn=False)

    def bpm(self, samples) -> float:
        beats, _downbeats = self._infer(samples, self.sr)
        if len(beats) < 2:
            raise RuntimeError('fewer than 2 beats detected')
        # Median inter-beat interval → robust to occasional misses
        import numpy as np
        intervals = np.diff(beats)
        return float(60.0 / np.median(intervals))


BACKENDS = {'madmom': MadmomBackend, 'beat-this': BeatThisBackend}


# ---- tag I/O ----

def read_bpm_tag(path: Path) -> str | None:
    """Return existing BPM tag string, or None."""
    from mutagen import File as MFile
    try:
        f = MFile(path)
    except Exception:
        return None
    if f is None or f.tags is None:
        return None
    # Try common spellings
    for k in ('BPM', 'bpm', 'TBPM', 'TMPO', '----:com.apple.iTunes:BPM'):
        v = f.tags.get(k) if hasattr(f.tags, 'get') else None
        if v:
            return str(v[0]) if hasattr(v, '__iter__') and not isinstance(v, str) else str(v)
    return None


def write_bpm_tag(path: Path, bpm: float) -> None:
    """Write BPM to the file's tags. Format-agnostic via mutagen.File."""
    from mutagen import File as MFile
    from mutagen.id3 import TBPM
    f = MFile(path)
    if f is None:
        raise RuntimeError(f'mutagen could not open {path}')
    bpm_str = f'{bpm:.1f}'
    suffix = path.suffix.lower()
    if suffix == '.mp3':
        # ID3v2 frame
        if f.tags is None:
            f.add_tags()
        f.tags.add(TBPM(encoding=3, text=bpm_str))
    elif suffix in ('.m4a', '.aac', '.mp4'):
        # MP4 atoms: 'tmpo' is integer; also store freeform BPM for fractional
        f['tmpo'] = [int(round(bpm))]
        f['----:com.apple.iTunes:BPM'] = [bpm_str.encode('utf-8')]
    else:
        # FLAC, Ogg, Opus, WavPack → Vorbis comments / APEv2
        f['BPM'] = bpm_str
    f.save()


# ---- driver ----

def gather_files(targets: list[Path]) -> list[Path]:
    out: list[Path] = []
    for t in targets:
        if t.is_file():
            if t.suffix.lower() in AUDIO_EXTS:
                out.append(t)
        elif t.is_dir():
            for p in sorted(t.rglob('*')):
                if p.is_file() and p.suffix.lower() in AUDIO_EXTS:
                    out.append(p)
        else:
            print(f'skip: {t} not found', file=sys.stderr)
    return out


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description='Batch ML BPM tagger for deadbeef-bpmkey.')
    p.add_argument('--backend', choices=BACKENDS.keys(), required=True)
    p.add_argument('--force', action='store_true', help='overwrite existing BPM tag')
    p.add_argument('--dry-run', action='store_true', help='compute but do not write')
    p.add_argument('--min-bpm', type=float, default=40.0)
    p.add_argument('--max-bpm', type=float, default=240.0)
    p.add_argument('paths', nargs='+', type=Path)
    args = p.parse_args(argv)

    files = gather_files(args.paths)
    if not files:
        print('no audio files found', file=sys.stderr)
        return 1
    print(f'queued {len(files)} files (backend={args.backend})', file=sys.stderr)

    try:
        backend = BACKENDS[args.backend]()
    except ImportError as e:
        sys.stderr.write(
            f'ERROR: backend "{args.backend}" not installed: {e}\n'
            f'  pip install madmom        # for --backend madmom\n'
            f'  pip install beat-this     # for --backend beat-this\n'
        )
        return 2

    n_ok = n_skip = n_fail = 0
    for i, path in enumerate(files, 1):
        try:
            if not args.force:
                existing = read_bpm_tag(path)
                if existing and existing.strip() not in ('', '0', '0.0'):
                    print(f'[{i}/{len(files)}] skip (has BPM={existing}): {path}')
                    n_skip += 1
                    continue
            samples = decode_to_mono_f32(path, backend.sr)
            if len(samples) < backend.sr:
                print(f'[{i}/{len(files)}] skip (too short): {path}', file=sys.stderr)
                n_fail += 1
                continue
            bpm = backend.bpm(samples)
            if not (args.min_bpm <= bpm <= args.max_bpm):
                print(f'[{i}/{len(files)}] skip (BPM {bpm:.1f} out of range): {path}',
                      file=sys.stderr)
                n_fail += 1
                continue
            if args.dry_run:
                print(f'[{i}/{len(files)}] would write BPM={bpm:.1f}: {path}')
            else:
                write_bpm_tag(path, bpm)
                print(f'[{i}/{len(files)}] BPM={bpm:.1f}: {path}')
            n_ok += 1
        except subprocess.CalledProcessError as e:
            print(f'[{i}/{len(files)}] ffmpeg failed: {path}: {e.stderr.decode()[:200]}',
                  file=sys.stderr)
            n_fail += 1
        except Exception as e:
            print(f'[{i}/{len(files)}] {type(e).__name__}: {e}: {path}', file=sys.stderr)
            n_fail += 1

    print(f'done: ok={n_ok} skip={n_skip} fail={n_fail}', file=sys.stderr)
    return 0 if n_fail == 0 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
