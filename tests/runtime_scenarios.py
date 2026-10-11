#!/usr/bin/env python3
"""Exercise the real host, arcade ghost and recorded replay paths."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

SCENARIOS = {
    'arcade-time-trial': (500, {'OPENCMR2_PROBE_CURSORS': '0:2,88:2,29:0',
                              'OPENCMR2_PROBE_EXPECT_CARS': '2',
                              'OPENCMR2_PROBE_EXPECT_MODE': '7'}),
    'multiplayer-host': (500, {'OPENCMR2_PROBE_CURSORS': '0:3,33:2,60:0,53:1,61:6,19:2,62:3',
                             'OPENCMR2_PROBE_EXPECT_MODE': '8',
                             'OPENCMR2_PROBE_SESSION_NAME': 'TEST'}),
    'recorded-replay': (1000, {'OPENCMR2_PROBE_REPLAY_AT_TICK': '500'}),
}
REPLAY = re.compile(r'probe: replay tick=(\d+) playing=(-?\d+) lane=(-?\d+) frame=(-?\d+) length=(-?\d+)')


def verify_replay(log):
    samples = [tuple(map(int, match.groups())) for match in REPLAY.finditer(log)]
    playback = [s for s in samples if s[0] >= 500 and s[1]]
    finished = [s for s in samples if s[0] > 500 and not s[1]]
    # frame counts compressed control packets, not 25 Hz simulation steps.
    cursors = [(s[2], s[3]) for s in playback]
    if len(set(cursors)) < 3 or cursors != sorted(cursors):
        raise ValueError(f'replay did not advance continuously: {playback}')
    if not finished or finished[0][2] < 1 or finished[0][0] <= playback[-1][0]:
        raise ValueError(f'replay did not finish normally: {samples}')
    return {'distinct_packet_cursors': len(set(cursors)),
            'last_playing_tick': playback[-1][0], 'first_finished_tick': finished[0][0]}


def verify(probe, data, output):
    report = {}
    for name, (count, settings) in SCENARIOS.items():
        run = output / name
        run.mkdir(parents=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith('OPENCMR2_PROBE_')}
        env.update(OPENCMR2_HEADLESS='1', SDL_VIDEODRIVER='dummy',
                   SDL_AUDIODRIVER='dummy', XDG_DATA_HOME=str(run / 'user'))
        env.update(settings)
        log_path = run / 'run.log'
        with log_path.open('w') as log:
            subprocess.run([str(probe), str(data), str(run / 'trace.jsonl'), '30', str(count)],
                           env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        log = log_path.read_text()
        if not re.search(rf'probe: {count} ticks, [1-9][0-9]* driving ticks, .*; complete', log):
            raise ValueError(f'{name}: no completed race: {log_path}')
        records = [json.loads(line) for line in (run / 'trace.jsonl').read_text().splitlines()]
        if len(records) != count or records[-1]['tick'] != count - 1:
            raise ValueError(f'{name}: incomplete trace')
        report[name] = {'ticks': count, 'driving_ticks': records[-1]['driving_tick']}
        if name == 'recorded-replay':
            report[name]['replay'] = verify_replay(log)
        print(f'{name}: PASS ({count} ticks)', flush=True)
    (output / 'acceptance.json').write_text(json.dumps(report, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
        verify(args.probe.resolve(), args.data.resolve(), args.output.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix='opencmr2-runtime-') as directory:
            verify(args.probe.resolve(), args.data.resolve(), Path(directory))


if __name__ == '__main__':
    main()
