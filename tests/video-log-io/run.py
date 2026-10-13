"""Exercise actual encoder logging functions with controlled libc I/O outcomes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

FUNCTIONS = ('venc_output_log_buffers', 'venc_extradata_log_buffers', 'venc_input_log_buffers')
RELATIVE = Path('mm-video-v4l2/vidc/venc/src/video_encoder_device_v4l2.cpp')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = (args.source / RELATIVE).read_text()
    extracted = []
    checks = []
    for name in FUNCTIONS:
        match = re.search(r'^int venc_dev::' + name + r'\([\s\S]*?^}\n', source, re.M)
        assert match, name
        body = match.group()
        assert body.count('{') == body.count('}'), name
        extracted.append(body)
        checks.append({'function': name, 'line': source[:match.start()].count('\n') + 1, 'sha256': hashlib.sha256(body.encode()).hexdigest()})
    directory = Path(__file__).resolve().parent
    fixture = args.output / 'logging.cpp'
    fixture.write_text((directory / 'prefix.cpp').read_text() + '\n'.join(extracted) + (directory / 'cases.cpp').read_text())
    receipt = {'status': 'incomplete', 'source': str(args.source / RELATIVE), 'source_sha256': hashlib.sha256(source.encode()).hexdigest(), 'functions': checks, 'runs': []}
    receipt_path = args.output / 'receipt.json'
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    for compiler in ('g++', 'clang++'):
        output = args.output / compiler.replace('+', 'p')
        flags = ['-std=c++11', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare', '-O1', '-g']
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
        command = [compiler, *flags, str(fixture), '-o', str(output)]
        compilation = subprocess.run(command, capture_output=True, check=False)
        (args.output / (output.name + '.compile.log')).write_bytes(compilation.stdout + compilation.stderr)
        compilation.check_returncode()
        run = subprocess.run([str(output)], capture_output=True, check=False, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        (args.output / (output.name + '.run.log')).write_bytes(run.stdout + run.stderr)
        receipt['runs'].append({'compiler': compiler, 'compile_command': command, 'compile_exit': compilation.returncode, 'run_command': [str(output)], 'run_exit': run.returncode, 'stdout': run.stdout.decode(), 'stderr_bytes': len(run.stderr)})
        receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
        print(compiler + ': ' + run.stdout.decode().strip() + f' exit={run.returncode}', flush=True)
    assert hashlib.sha256((args.source / RELATIVE).read_bytes()).hexdigest() == receipt['source_sha256']
    receipt['status'] = 'passed' if all(run['run_exit'] == 0 for run in receipt['runs']) else 'failed'
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    raise SystemExit(0 if receipt['status'] == 'passed' else 1)


if __name__ == '__main__':
    main()
