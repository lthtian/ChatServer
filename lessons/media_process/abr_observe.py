"""Use real HLS segment sizes with a controlled bandwidth trace to observe ABR.

This is a teaching simulation, not the Qt player's scheduling algorithm.
Network RTT, decoder cost and parallel audio downloads are deliberately absent.
"""
import argparse
import json
import pathlib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('bundle', type=pathlib.Path)
args = parser.parse_args()
catalog = json.loads((args.bundle / 'catalog.json').read_text(encoding='utf-8'))
variants = sorted(catalog['variants'], key=lambda v: v['bandwidth'])
segments = {}
for variant in variants:
    playlist = args.bundle / variant['playlist']
    rows = []
    duration = 0
    for line in playlist.read_text(encoding='utf-8').splitlines():
        if line.startswith('#EXTINF:'):
            duration = float(line[8:].split(',')[0])
        elif line and not line.startswith('#'):
            rows.append((duration, (playlist.parent / line).stat().st_size))
    segments[variant['id']] = rows

# Four buffered seconds initially; throughput changes at known segment indexes.
estimate, buffered, chosen, upgrade_votes = 2_500_000.0, 4.0, 0, 0
print('segment link_Mbps estimate_Mbps quality download_s buffer_s stall_s')
for index in range(min(18, min(len(s) for s in segments.values()))):
    bandwidth = 2_500_000 if index < 5 else 500_000 if index < 11 else 3_000_000
    eligible = [i for i, v in enumerate(variants) if v['bandwidth'] <= estimate * 0.7]
    candidate = eligible[-1] if eligible else 0
    if candidate < chosen or buffered < 2:
        chosen, upgrade_votes = min(candidate, chosen), 0
    elif candidate > chosen and buffered >= 4:
        upgrade_votes += 1
        if upgrade_votes >= 3:
            chosen, upgrade_votes = candidate, 0
    else:
        upgrade_votes = 0
    variant = variants[chosen]
    duration, size = segments[variant['id']][index]
    seconds = size * 8 / bandwidth
    stall = max(0, seconds - buffered)
    buffered = max(0, buffered - seconds) + duration
    estimate = estimate * 0.5 + (size * 8 / seconds) * 0.5
    print(f'{index:2d} {bandwidth/1e6:.2f} {estimate/1e6:.2f} {variant["id"]:4} '
          f'{seconds:.3f} {buffered:.3f} {stall:.3f}')
