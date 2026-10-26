"""Sample a command's process family, retaining its exit status and a JSON resource report (stdlib only)."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def ps_seconds(text):
    """Parse ps's [[dd-]hh:]mm:ss[.ss] cumulative CPU time."""
    days, _, clock = text.rpartition('-')
    seconds = 0.0
    for part in clock.split(':'):
        seconds = seconds * 60 + float(part)
    return seconds + float(days or 0) * 86400


def snapshot(root=None):
    """pid -> (parent, resident bytes, cumulative CPU seconds). Sampling can miss short-lived children."""
    result = {}
    if sys.platform.startswith('linux'):
        ticks = os.sysconf('SC_CLK_TCK')
        page = os.sysconf('SC_PAGE_SIZE')
        for directory in Path('/proc').glob('[0-9]*'):
            try:
                stat = (directory / 'stat').read_text().rsplit(')', 1)[1].split()
                result[int(directory.name)] = (int(stat[1]), int(stat[21]) * page,
                                                (int(stat[11]) + int(stat[12])) / ticks)
            except (OSError, ValueError, IndexError):
                pass
    elif sys.platform == 'darwin':
        output = subprocess.check_output(['ps', '-axo', 'pid=,ppid=,rss=,time='], text=True)
        for line in output.splitlines():
            pid, parent, rss, cpu = line.split()
            seconds = ps_seconds(cpu)
            result[int(pid)] = (int(parent), int(rss) * 1024, seconds)
    elif sys.platform == 'win32':
        import ctypes as c
        from ctypes import wintypes as w
        class Entry(c.Structure):
            _fields_ = [('size', w.DWORD), ('usage', w.DWORD), ('pid', w.DWORD), ('heap', c.c_size_t),
                        ('module', w.DWORD), ('threads', w.DWORD), ('parent', w.DWORD),
                        ('priority', w.LONG), ('flags', w.DWORD), ('name', w.WCHAR * 260)]
        class Memory(c.Structure):
            _fields_ = [('size', w.DWORD), ('faults', w.DWORD)] + [(name, c.c_size_t) for name in
                ('peakWorking', 'working', 'peakPaged', 'paged', 'peakNonpaged', 'nonpaged', 'pagefile', 'peakPagefile')]
        kernel = c.WinDLL('kernel32', use_last_error=True)
        kernel.CreateToolhelp32Snapshot.restype = w.HANDLE
        kernel.OpenProcess.restype = w.HANDLE
        kernel.CloseHandle.argtypes = [w.HANDLE]
        kernel.Process32FirstW.argtypes = [w.HANDLE, c.POINTER(Entry)]
        kernel.Process32NextW.argtypes = [w.HANDLE, c.POINTER(Entry)]
        kernel.GetProcessTimes.argtypes = [w.HANDLE] + [c.POINTER(w.FILETIME)] * 4
        psapi = c.WinDLL('psapi')
        psapi.GetProcessMemoryInfo.argtypes = [w.HANDLE, c.POINTER(Memory), w.DWORD]
        handle = kernel.CreateToolhelp32Snapshot(2, 0)
        entry = Entry(size=c.sizeof(Entry))
        parents = {}
        more = kernel.Process32FirstW(handle, c.byref(entry))
        while more:
            parents[entry.pid] = entry.parent
            more = kernel.Process32NextW(handle, c.byref(entry))
        # Opening every process on a loaded runner can outlast a short command: only query the root's descendants.
        wanted = set(parents)
        if root is not None:
            wanted = {root}
            while True:
                children = {pid for pid, parent in parents.items() if parent in wanted}
                if children <= wanted:
                    break
                wanted |= children
        for pid in wanted & set(parents):
            process = kernel.OpenProcess(0x1000 | 0x10, False, pid)
            if process:
                memory = Memory(size=c.sizeof(Memory))
                times = [w.FILETIME() for _ in range(4)]
                if psapi.GetProcessMemoryInfo(process, c.byref(memory), memory.size) and kernel.GetProcessTimes(process, *map(c.byref, times)):
                    cpu = sum((item.dwHighDateTime << 32) + item.dwLowDateTime for item in times[2:]) / 10_000_000
                    result[pid] = (parents[pid], memory.working, cpu)
                kernel.CloseHandle(process)
        kernel.CloseHandle(handle)
    else:
        raise RuntimeError('Unsupported resource sampler platform: ' + sys.platform)
    return result


def measure(command, output, interval=0.25):
    started = time.monotonic()
    process = subprocess.Popen(command)
    observed = {}
    peak_memory = peak_count = 0
    while process.poll() is None:
        processes = snapshot(process.pid)
        family = {process.pid}
        while True:
            children = {pid for pid, (parent, _, _) in processes.items() if parent in family}
            if children <= family:
                break
            family.update(children)
        active = {pid: processes[pid] for pid in family if pid in processes}
        peak_memory = max(peak_memory, sum(item[1] for item in active.values()))
        peak_count = max(peak_count, len(active))
        for pid, (_, _, cpu) in active.items():
            observed[pid] = max(cpu, observed.get(pid, 0))
        time.sleep(interval)
    result = {'command': command, 'platform': sys.platform, 'exit_code': process.returncode,
              'wall_seconds': time.monotonic() - started, 'sample_interval_seconds': interval,
              'sampled_peak_family_rss_bytes': peak_memory, 'sampled_peak_processes': peak_count,
              'sampled_processes_seen': len(observed), 'sampled_cpu_seconds': sum(observed.values()),
              'limitation': 'Sampled bounds: short-lived children may be missed; shared pages count in each process RSS.'}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return process.returncode


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('a command is required')
    raise SystemExit(measure(command, args.output))
