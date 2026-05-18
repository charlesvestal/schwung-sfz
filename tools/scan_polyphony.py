#!/usr/bin/env python3
"""Predict per-preset polyphony default using the same heuristic the
plugin uses (max region stacking across the (key, vel) plane → polyphony
cap = floor(VOICE_TARGET / stacking), clamped to [POLY_FLOOR, POLY_CEIL]).

Walks an instruments directory tree and reports for each .sfz / .dspreset
what polyphony the plugin would set automatically.

Usage:
    # remote (Move):
    ssh move.local 'find /data/UserData/schwung/modules/sound_generators/sfz/instruments \
        -type f \( -name "*.sfz" -o -name "*.dspreset" \)' | \\
        python3 scan_polyphony.py --stdin-list move

    # local:
    python3 scan_polyphony.py /path/to/instruments
"""
import os
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from collections import defaultdict

VOICE_TARGET = 60
POLY_FLOOR   = 4
POLY_CEIL    = 14

# RR weighting: regions with seq_length=N share one slot — only 1 of N
# fires per note-on. We accumulate fractional contributions (1/N) into
# the grid; ceil at the end. seq_length=1 (or absent) contributes 1.0.

def recommend(stacking: int) -> int:
    if stacking < 1: stacking = 1
    r = VOICE_TARGET // stacking
    if r < POLY_FLOOR: r = POLY_FLOOR
    if r > POLY_CEIL:  r = POLY_CEIL
    return r

def grid_max(regions):
    """regions: list of (klo, khi, vlo, vhi, rr_key). rr_key is either None
    (always fires) or a hashable tag identifying the round-robin group
    (e.g. ('ds-groups', seqPosition) or ('sfz', seq_length, seq_position)).

    Per (key, vel) cell, only one rr_key value fires at a time per cycle.
    So effective concurrent voices at a cell =
        always_count(cell) + max_over_rr_keys(count_at_cell_for_that_key).
    Max across cells = worst-case simultaneous voice count per note-on."""
    if not regions: return 0
    always = [0] * (128 * 128)
    per_rr = {}  # rr_key -> grid

    def fill(grid, klo, khi, vlo, vhi):
        klo = max(0, klo); khi = min(127, khi)
        vlo = max(1, vlo); vhi = min(127, vhi)
        if klo > khi or vlo > vhi: return
        for k in range(klo, khi + 1):
            row = k * 128
            for v in range(vlo, vhi + 1):
                grid[row + v] += 1

    for klo, khi, vlo, vhi, rr in regions:
        if rr is None:
            fill(always, klo, khi, vlo, vhi)
        else:
            g = per_rr.setdefault(rr, [0] * (128 * 128))
            fill(g, klo, khi, vlo, vhi)

    if not per_rr:
        return max(always)
    max_eff = 0
    for i in range(128 * 128):
        rr_max = max(g[i] for g in per_rr.values())
        eff = always[i] + rr_max
        if eff > max_eff: max_eff = eff
    return max_eff

# --- SFZ parser (covers what the plugin uses: lokey/hikey/lovel/hivel,
#     key= shorthand, pitch_keycenter as single-key fallback). -----------
def parse_sfz_regions(path: Path):
    """Return list of (klo, khi, vlo, vhi) tuples by walking <global>/<group>/<region>
    inheritance. Defaults: lokey=0, hikey=127, lovel=0, hivel=127."""
    try:
        text = path.read_text(encoding='utf-8', errors='ignore')
    except Exception:
        return []
    # Strip comments
    text = re.sub(r'//.*', '', text)
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)

    # Split into header sections
    headers = re.split(r'<(global|group|region|master|control)>', text)
    # headers[0] is anything before the first header (often nothing)
    inherited = [{}]  # stack
    regions = []
    i = 1
    cur_master = {}
    while i < len(headers):
        kind = headers[i]
        body = headers[i+1] if i+1 < len(headers) else ''
        opcodes = dict(re.findall(r'(\w+)\s*=\s*("[^"]*"|\S+)', body))
        if kind == 'master':
            cur_master = opcodes
        elif kind == 'global':
            inherited = [opcodes]
        elif kind == 'group':
            inherited = [inherited[0] if inherited else {}, opcodes]
        elif kind == 'region':
            merged = {}
            for layer in inherited: merged.update(layer)
            merged.update(cur_master)
            merged.update(opcodes)
            trig = merged.get('trigger', 'attack').strip('"')
            if trig not in ('attack', 'first', 'legato'):
                # release-trigger / first-trigger-only-on-newest etc.
                # Don't fire on every note-on; skip.
                # ('legato' fires on note-on within a held context — close
                # enough to count as a typical attack for stacking purposes.)
                i += 2; continue
            klo, khi, vlo, vhi = region_bounds(merged)
            try: seq_length = max(1, int(float(merged.get('seq_length', '1').strip('"'))))
            except Exception: seq_length = 1
            try: seq_position = int(float(merged.get('seq_position', '1').strip('"')))
            except Exception: seq_position = 1
            try: group_id = int(float(merged.get('group', '0').strip('"')))
            except Exception: group_id = 0
            # RR identity: regions with same seq_length and group share
            # one cycle; their seq_position determines which fires.
            if seq_length > 1:
                rr_key = ('sfz', group_id, seq_length, seq_position)
            else:
                rr_key = None
            regions.append((klo, khi, vlo, vhi, rr_key))
        i += 2
    return regions

def region_bounds(opcodes):
    def to_pitch(s):
        # Accept MIDI number or note name like "c4", "f#3"
        s = s.strip('"').strip()
        try: return int(s)
        except ValueError: pass
        m = re.match(r'^([a-gA-G])([#b]?)(-?\d+)$', s)
        if not m: return None
        base = {'c':0,'d':2,'e':4,'f':5,'g':7,'a':9,'b':11}[m.group(1).lower()]
        if m.group(2) == '#': base += 1
        elif m.group(2) == 'b': base -= 1
        octave = int(m.group(3))
        return base + (octave + 1) * 12
    def int_or(s, default):
        try: return int(float(s.strip('"')))
        except Exception: return default

    def pitch_or(opcode_val, default):
        """SFZ keys can be MIDI numbers or note names (c4, F#3, Ab2)."""
        if opcode_val is None: return default
        s = opcode_val.strip('"').strip()
        try: return int(float(s))
        except ValueError: pass
        p = to_pitch(s)
        return p if p is not None else default
    if 'key' in opcodes:
        k = pitch_or(opcodes['key'], 60); klo = khi = k
    else:
        klo = pitch_or(opcodes.get('lokey'), 0)
        khi = pitch_or(opcodes.get('hikey'), 127)
    vlo = int_or(opcodes.get('lovel', '0'), 0)
    vhi = int_or(opcodes.get('hivel', '127'), 127)
    return klo, khi, vlo, vhi

# --- dspreset parser ----------------------------------------------------
def parse_dspreset_regions(path: Path):
    try:
        root = ET.parse(path).getroot()
    except Exception:
        return []
    regions = []
    # DS round-robin: <groups seqMode="round_robin"> wraps groups with
    # seqPosition="N" attributes. Per note-on, only groups whose
    # seqPosition matches the current cycle position fire. We tag each
    # region with its seqPosition so grid_max can take the per-cell
    # max across positions instead of summing them.
    groups_attrs = {}
    gw = root.find('groups')
    if gw is not None:
        groups_attrs = dict(gw.attrib)
    wrapper_rr = (groups_attrs.get('seqMode') == 'round_robin')
    for grp in (root.iter('group')):
        gattr = {**groups_attrs, **dict(grp.attrib)}
        # Groups with trigger != "attack"/"first" don't fire on regular
        # note-on (legato, release-trigger samples, etc.). Skip them so
        # the heuristic counts only what plays on every note-on.
        trig = gattr.get('trigger', 'attack')
        if trig not in ('attack', 'first', 'always'):
            continue
        # A group participates in RR if the wrapper says so AND it has
        # a seqPosition. Otherwise it's "always-on" and contributes to
        # the constant baseline.
        if wrapper_rr and 'seqPosition' in gattr:
            try: pos = int(float(gattr['seqPosition']))
            except Exception: pos = None
            rr_key = ('ds-rr', pos) if pos is not None else None
        else:
            rr_key = None
        for samp in grp.iter('sample'):
            sattr = {**gattr, **dict(samp.attrib)}
            def gi(k, default):
                v = sattr.get(k)
                if v is None: return default
                try: return int(float(v))
                except Exception: return default
            root_note = gi('rootNote', None)
            lo = gi('loNote', root_note if root_note is not None else 0)
            hi = gi('hiNote', root_note if root_note is not None else 127)
            vlo = gi('loVel', 0)
            vhi = gi('hiVel', 127)
            regions.append((lo, hi, vlo, vhi, rr_key))
    return regions

# --- main ---------------------------------------------------------------
def find_files(root):
    if str(root).startswith('move:'):
        # ssh-side find
        import subprocess
        rpath = str(root)[5:]
        out = subprocess.check_output([
            'ssh', 'move.local',
            f'find "{rpath}" -type f \\( -name "*.sfz" -o -name "*.dspreset" \\) | sort'
        ]).decode().strip().split('\n')
        return [f for f in out if f]
    return [str(p) for p in sorted(Path(root).rglob('*'))
            if p.suffix.lower() in ('.sfz', '.dspreset')]

def fetch_remote_text(path):
    import subprocess
    return subprocess.check_output(['ssh', 'move.local', f'cat "{path}"']).decode()

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    arg = sys.argv[1]
    files = find_files(arg)
    print(f"# scanning {len(files)} files")
    print(f"# heuristic: voices = floor({VOICE_TARGET}/stacking), clamped [{POLY_FLOOR}, {POLY_CEIL}]")
    print()
    print(f"{'stacking':>8}  {'voices':>6}  path")
    print('-' * 80)
    per_lib_max = defaultdict(int)
    rows = []
    for f in files:
        try:
            if arg.startswith('move:'):
                # pull text via ssh, parse from string
                import tempfile
                txt = fetch_remote_text(f)
                tmp = Path('/tmp/_scan_one')
                tmp.write_text(txt)
                p = tmp
            else:
                p = Path(f)
            if p.suffix.lower() == '.dspreset':
                regs = parse_dspreset_regions(p)
            else:
                regs = parse_sfz_regions(p)
            st = grid_max(regs)
            rec = recommend(st)
            rows.append((st, rec, f))
            # library = first dir under instruments/
            lib = Path(f).parts[-2] if '/' in f else 'root'
            per_lib_max[lib] = max(per_lib_max[lib], st)
        except Exception as e:
            print(f"# err {f}: {e}", file=sys.stderr)
    # sort by stacking descending
    rows.sort(key=lambda r: (-r[0], r[2]))
    for st, rec, f in rows:
        # shorten path for display
        disp = f.replace('/data/UserData/schwung/modules/sound_generators/sfz/instruments/', '')
        print(f"{st:>8}  {rec:>6}  {disp}")

if __name__ == '__main__':
    main()
