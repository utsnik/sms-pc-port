#!/usr/bin/env python3
"""Require a real cache hit with the selected private Windows compiler.

The probe runs on a server of its own with a local disk cache: CI's server
stores into the GitHub Actions cache, whose writes fail when the repository's
cache is busy or full, and that is not what this checks.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time

# away from sccache's default 4226, which the build's own server holds
PROBE_PORT = '4400'


def stats(cache, env):
    return json.loads(subprocess.check_output(
        [cache, '--show-stats', '--stats-format', 'json'], text=True, env=env))


def hits(report):
    return sum(report['stats']['cache_hits']['counts'].values())


def main():
    cache = os.environ['SMS_COMPILER_CACHE']
    compiler = os.environ.get('CXX') or str(
        pathlib.Path(os.environ['MSYS2_ROOT']) / 'mingw64/bin/g++.exe')
    command = [cache, compiler]
    if os.environ.get('SMS_WINDOWS_32_CROSS') == '1':
        command = [sys.executable, str(pathlib.Path(__file__).resolve().parents[1] /
                   'msys_cross_compile.py'), '--cache', cache, compiler]
    with tempfile.TemporaryDirectory(prefix='SMS cache check with spaces ') as folder:
        folder = pathlib.Path(folder)
        env = {k: v for k, v in os.environ.items() if not k.startswith('SCCACHE_')}
        env['SCCACHE_DIR'] = str(folder / 'cache')
        env['SCCACHE_SERVER_PORT'] = PROBE_PORT
        subprocess.run([cache, '--start-server'], check=True, env=env)
        try:
            print('Probe cache:', stats(cache, env)['cache_location'])
            source = folder / 'cache probe.cpp'
            header = folder / 'forced header.h'
            output = folder / 'probe.obj'
            header.write_text('#define PROBE_VALUE 42\n')
            source.write_text('int cache_probe() { return PROBE_VALUE; }\n')
            args = ['-include', str(header), '-c', str(source), '-o', str(output)]
            # Use the same drive and working directory as the launcher game build.
            subprocess.run([*command, *args], check=True, env=env)
            # sccache stores a miss after the compile returns
            deadline = time.monotonic() + 30
            while True:
                report = stats(cache, env)
                written = report['stats']['cache_writes']
                if written or report['stats']['cache_write_errors']:
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError('The first compilation was never written to the compiler cache')
                time.sleep(0.2)
            if not written:
                raise RuntimeError('The compiler cache could not store the first compilation')
            before = hits(report)
            output.unlink()
            subprocess.run([*command, *args], check=True, env=env)
            if not output.is_file() or hits(stats(cache, env)) <= before:
                raise RuntimeError('Repeated compilation did not produce a compiler cache hit')
        finally:
            subprocess.run([cache, '--stop-server'], env=env)
    print('Verified compiler cache hit with private Windows tools')


if __name__ == '__main__':
    main()
