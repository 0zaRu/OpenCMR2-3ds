#!/usr/bin/env python3
"""Two independent game processes: LAN discovery, join, ready and stage load.

Accepts native probes or the Wine wrapper used by physics_parity.py. Both
processes use isolated profiles and the same real UDP protocol as LAN play.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile


def verify(host_probe, guest_probe, data, output):
    processes = []
    logs = []
    # Reserve two distinct ports until profiles are written.
    sockets = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in range(2)]
    try:
        for s in sockets:
            s.bind(('127.0.0.1', 0))
        host_port, guest_port = [s.getsockname()[1] for s in sockets]
        for role, port, probe in [('host', host_port, host_probe), ('guest', guest_port, guest_probe)]:
            run = output / role
            user = run / 'user'
            user.mkdir(parents=True)
            (user / 'opencmr2.ini').write_text(
                f'[net]\nport={port}\npeers=127.0.0.1:{host_port}\nbroadcast=0\ntailscale=off\n')
            env = {k: v for k, v in os.environ.items() if not k.startswith('OPENCMR2_PROBE_')}
            browser = 1 if role == 'host' else 0
            env.update(OPENCMR2_HEADLESS='1', SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
                       XDG_DATA_HOME=str(user), OPENCMR2_PROBE_LAN_ROLE=role,
                       OPENCMR2_PROBE_CURSORS=f'0:3,33:2,60:0,53:{browser},61:6,19:2,62:3',
                       OPENCMR2_PROBE_EXPECT_CARS='2', OPENCMR2_PROBE_EXPECT_MODE='8',
                       OPENCMR2_PROBE_SESSION_NAME='LAN TEST')
            logs.append((run / 'run.log').open('w'))
            sockets[len(processes)].close()
            processes.append(subprocess.Popen(
                [str(probe), str(data), str(run / 'trace.jsonl'), '30', '500'],
                env=env, stdout=logs[-1], stderr=subprocess.STDOUT))
        for role, process in zip(('host', 'guest'), processes):
            code = process.wait(timeout=60)
            if code:
                raise RuntimeError(f'{role} failed ({code}): {output / role / "run.log"}')
        report = {}
        for role in ('host', 'guest'):
            records = [json.loads(line) for line in (output / role / 'trace.jsonl').read_text().splitlines()]
            if len(records) != 500 or records[-1]['driving_tick'] <= 0:
                raise ValueError(f'{role}: no complete two-player race')
            remote_positions = {tuple(car['position']) for record in records if record['state'] == 8
                                for car in record['cars'] if car['id'] == 1}
            if len(remote_positions) < 50:
                raise ValueError(f'{role}: remote car did not advance')
            report[role] = {'ticks': len(records), 'driving_ticks': records[-1]['driving_tick'],
                            'distinct_remote_positions': len(remote_positions)}
        if 'probe: LAN guest ready' not in (output / 'host/run.log').read_text():
            raise ValueError('host started without receiving guest readiness')
        (output / 'acceptance.json').write_text(json.dumps(report, indent=2) + '\n')
        print('LAN game: host and guest joined and drove 500 ticks with two cars')
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.wait()
        for log in logs:
            log.close()
        for s in sockets:
            s.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host-probe', type=Path, required=True)
    parser.add_argument('--guest-probe', type=Path)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    host = args.host_probe.resolve()
    guest = (args.guest_probe or args.host_probe).resolve()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
        verify(host, guest, args.data.resolve(), args.output.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix='opencmr2-lan-') as directory:
            verify(host, guest, args.data.resolve(), Path(directory))


if __name__ == '__main__':
    main()
