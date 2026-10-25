"""Shared diagnostic collection, cleanup and cross-channel locking for native lanes."""
import contextlib
import os
from pathlib import Path
import shutil
import stat
import sys
import tarfile
import time


def collect(root, lane):
    workspace = root / lane
    destination = root / (lane + "-artifacts.tar")
    # A recovery stop may arrive after the original channel archived and removed
    # its checkout. Preserve that complete archive rather than replacing it with just a log.
    if destination.exists() and not workspace.exists():
        return
    temporary = destination.with_suffix(".tmp")
    with tarfile.open(temporary, "w") as archive:
        for suffix in (".session.log", ".launch.log"):
            log = root / (lane + suffix)
            if log.is_file():
                archive.add(log, arcname=log.name)
        for pattern in ("**/test-results.xml", "**/LastTest.log", "**/CMakeConfigureLog.yaml"):
            for path in workspace.glob(pattern):
                if path.is_file() and not path.is_symlink():
                    archive.add(path, arcname=path.relative_to(workspace), recursive=False)
    temporary.replace(destination)


def cleanTree(root, target):
    root, target = root.resolve(), target.resolve()
    if (root.parent.name != "runs" or root.parent.parent.name != ".webfront-local-ci"
            or not target.is_relative_to(root)):
        raise ValueError("Refusing cleanup outside a local CI run")
    def readonly(function, path, error):
        # Git object files are read-only on Windows, even in an owned disposable clone.
        if not Path(path).resolve().is_relative_to(target):
            raise ValueError("Cleanup path escaped the local CI run")
        if not os.stat(path).st_mode & stat.S_IWRITE:
            os.chmod(path, stat.S_IWRITE)
            function(path)
        else:
            raise error
    if sys.version_info >= (3, 12):
        shutil.rmtree(target, onexc=readonly)
    else:
        shutil.rmtree(target, onerror=lambda function, path, error: readonly(function, path, error[1]))


@contextlib.contextmanager
def runLock(root, timeout=60):
    """Serialize finalization and recovery SSH channels, including run-directory removal."""
    root = root.resolve()
    if root.parent.name != "runs" or root.parent.parent.name != ".webfront-local-ci":
        raise ValueError("Refusing lock outside a local CI run")
    # Outside the removable run: every channel must keep opening the same lock file.
    locks = root.parent.parent / "locks"
    locks.mkdir(exist_ok=True)
    with open(locks / (root.name + ".lock"), "a+b") as lock:
        if lock.tell() == 0:
            lock.write(b"\0")
            lock.flush()
        deadline = time.monotonic() + timeout
        while True:
            try:
                lock.seek(0)
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise TimeoutError("Another native CI channel is still finalizing this run")
                time.sleep(0.1)
        try:
            yield
        finally:
            lock.seek(0)
            if os.name == "nt":
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


