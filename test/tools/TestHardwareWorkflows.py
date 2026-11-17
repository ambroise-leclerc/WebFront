#!/usr/bin/env python3
"""Fail closed for the audited hardware workflow control plane, without a YAML dependency.

This is a regression guard, not a GitHub access-control boundary: fork authors can
edit tests too. External workflow approvals must be reviewed before execution.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
# Keep control-plane YAML deliberately literal. Any change requires reviewing the
# security policy and updating this independent expectation, including new jobs.
CONTROL = """name: Jetson Orin
on:
  schedule:
    - cron: '17 1 * * 1'
  push:
    branches: [master]
  workflow_dispatch:
permissions:
  contents: read
concurrency:
  group: jetson-orin
  cancel-in-progress: false
jobs:
  jetson-orin:
    if: >-
      github.repository == 'ambroise-leclerc/WebFront' && !cancelled() &&
      (github.event_name == 'schedule' || (github.event_name == 'push' && github.ref == 'refs/heads/master') ||
      (github.event_name == 'workflow_dispatch' && github.actor == 'ambroise-leclerc' && github.ref_type == 'branch'))
    runs-on: [self-hosted, linux, arm64, jetson-orin]
    timeout-minutes: 180
    steps:
"""


VALIDATION_CONTROL = """  jetson-validation:
    if: >-
      github.repository == 'ambroise-leclerc/WebFront' && !cancelled() && github.event_name == 'workflow_dispatch' &&
      github.actor == 'ambroise-leclerc' && github.ref_type == 'branch' && inputs.jetson_orin
    runs-on: [self-hosted, linux, arm64, jetson-orin]
    timeout-minutes: 180
    concurrency:
      group: jetson-orin
      cancel-in-progress: false
    steps:
"""

HOSTED = re.compile(r'(?:ubuntu|windows|macos)-(?:latest|[0-9]+(?:\.[0-9]+)?)(?:-arm)?')


def check_hosted(name, body):
    # Generic Linux/ARM64 labels can select this runner without spelling
    # self-hosted. Permit only literal hosted labels and the audited static os
    # matrices; reject unfamiliar YAML rather than guessing how GitHub routes it.
    for line in body.splitlines():
        if line.lstrip().startswith('#'):
            continue
        if 'runs-on' in line:
            match = re.fullmatch(r"\s*['\"]?runs-on['\"]?:\s*(.*?)\s*", line)
            value = match[1].strip("'\"") if match else ''
            if HOSTED.fullmatch(value):
                continue
            if name == 'BuildAndTest.yml' and value == '${{ matrix.os }}':
                continue
            raise ValueError(f'{name}: unaudited runner selection: {line.strip()}')
        matrix = re.fullmatch(r'\s*(matrix|include|exclude):\s*(.*)', line)
        if matrix and matrix[2]:
            raise ValueError(f'{name}: dynamic or inline matrix requires a routing audit')
    for value in re.findall(r'\bos:\s*([^,}\n]+)', body):
        if not HOSTED.fullmatch(value.strip().strip("'\"")):
            raise ValueError(f'{name}: os matrix must contain literal hosted labels')


def check(workflows):
    hardware = workflows.get('JetsonOrin.yml', '')
    lines = [line for line in hardware.splitlines() if line.strip() and not line.lstrip().startswith('#')]
    prefix = CONTROL.splitlines()
    if lines[:len(prefix)] != prefix:
        raise ValueError('Jetson control plane changed: audit events, master ref, maintainer and runner labels')
    # Prevent a second job, trigger, document or duplicate key after the steps.
    if any(not line.startswith('      ') for line in lines[len(prefix):]):
        raise ValueError('Unexpected control-plane YAML after Jetson steps')
    for name, body in workflows.items():
        if name == 'BuildAndTest.yml':
            match = re.search(r'^  jetson-validation:\n(?:(?:[ ]{4,}[^\n]*|[ ]*)\n)*', body, re.M)
            if not match or not match[0].startswith(VALIDATION_CONTROL):
                raise ValueError('Manual Jetson validation must require maintainer dispatch')
            remainder = match[0][len(VALIDATION_CONTROL):].splitlines()
            if any(line.strip() and not line.startswith('      ') for line in remainder):
                raise ValueError('Unexpected job-level YAML after manual Jetson steps')
            body = body[:match.start()] + body[match.end():]
        if name != 'JetsonOrin.yml' and re.search(r'self-hosted|jetson-orin', body, re.I):
            raise ValueError(f'{name}: hardware runners are confined to JetsonOrin.yml')
        if name != 'JetsonOrin.yml':
            check_hosted(name, body)


class HardwareWorkflows(unittest.TestCase):
    def test_repository(self):
        check({p.name: p.read_text() for p in (ROOT / '.github/workflows').iterdir()
               if p.suffix in ('.yml', '.yaml')})

    def test_rejects_untrusted_routes(self):
        for old, new in [
            ('  workflow_dispatch:', '  pull_request:'),
            ('  workflow_dispatch:', '  pull_request_target:'),
            ('  workflow_dispatch:', '  workflow_call:'),
            ('branches: [master]', 'branches: [develop, master]'),
            ("github.ref == 'refs/heads/master'", 'true'),
            ("github.actor == 'ambroise-leclerc'", 'true'),
            ("github.repository == 'ambroise-leclerc/WebFront'", 'true'),
            ("github.ref_type == 'branch'", 'true'),
            ("github.event_name == 'push'", "github.event_name == 'pull_request'"),
            ('contents: read', 'contents: write'),
        ]:
            with self.subTest(change=new), self.assertRaises(ValueError):
                check({'JetsonOrin.yml': CONTROL.replace(old, new)})
        for suffix in ['on: [pull_request]\n', '  unsafe:\n    runs-on: self-hosted\n', '---\n']:
            with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                check({'JetsonOrin.yml': CONTROL + suffix})

    def test_rejects_weakened_manual_guard(self):
        body = (ROOT / '.github/workflows/BuildAndTest.yml').read_text()
        for old in ["github.event_name == 'workflow_dispatch'", "github.actor == 'ambroise-leclerc'", 'inputs.jetson_orin']:
            with self.subTest(guard=old), self.assertRaises(ValueError):
                check({'JetsonOrin.yml': CONTROL, 'BuildAndTest.yml': body.replace(old, 'true')})

    def test_rejects_hardware_in_other_workflows(self):
        for label in ['self-hosted', 'jetson-orin', 'SELF-HOSTED']:
            with self.subTest(label=label), self.assertRaises(ValueError):
                check({'JetsonOrin.yml': CONTROL,
                       'fork.yml': f'on: [pull_request]\njobs:\n  fork:\n    runs-on: [{label}]\n'})

    def test_rejects_duplicate_manual_job_keys(self):
        for key in ['if: true', 'permissions: write-all', 'runs-on: self-hosted']:
            with self.subTest(key=key), self.assertRaises(ValueError):
                check({'JetsonOrin.yml': CONTROL,
                       'BuildAndTest.yml': 'jobs:\n' + VALIDATION_CONTROL + '    ' + key + '\n'})

    def test_rejects_generic_and_dynamic_runner_routes(self):
        for route in ['Linux', 'ARM64', '[linux, arm64]', '${{ inputs.runner }}',
                      '\n      labels: Linux', '{labels: ARM64}']:
            with self.subTest(route=route), self.assertRaises(ValueError):
                check({'JetsonOrin.yml': CONTROL,
                       'fork.yml': 'on: [pull_request]\njobs:\n  fork:\n    runs-on: ' + route + '\n'})

    def test_rejects_untrusted_os_matrices(self):
        body = (ROOT / '.github/workflows/BuildAndTest.yml').read_text()
        for changed in [body.replace('os: ubuntu-26.04', 'os: Linux'),
                        body.replace('      matrix:\n', '      matrix: ${{ fromJSON(inputs.matrix) }}\n'),
                        body.replace('        include:\n', '        include: ${{ fromJSON(inputs.jobs) }}\n')]:
            with self.assertRaises(ValueError):
                check({'JetsonOrin.yml': CONTROL, 'BuildAndTest.yml': changed})


if __name__ == '__main__':
    unittest.main()
