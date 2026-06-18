from __future__ import annotations

import os
import time
from dataclasses import dataclass


@dataclass(frozen=True)
class InstanceLock:
    """A very small single-instance lock.

    On Linux boards, prefer a lock in /run (tmpfs). On Windows dev machines,
    falls back to the current directory.
    """

    lock_path: str

    def acquire(self) -> None:
        # O_EXCL makes creation atomic.
        flags = os.O_CREAT | os.O_EXCL | os.O_WRONLY
        try:
            fd = os.open(self.lock_path, flags)
            try:
                os.write(fd, str(os.getpid()).encode("ascii", errors="ignore"))
            finally:
                os.close(fd)
        except FileExistsError as e:
            raise RuntimeError(f"service already running (lock exists): {self.lock_path}") from e

    def release(self) -> None:
        try:
            os.unlink(self.lock_path)
        except FileNotFoundError:
            return


def default_lock_path() -> str:
    # Prefer /run on Linux.
    if os.name != "nt":
        return "/run/wifi_service.lock"
    return os.path.abspath("wifi_service.lock")


def wait_for_file_disappear(path: str, timeout_s: float = 3.0) -> None:
    """Best-effort helper for crash recovery scenarios."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if not os.path.exists(path):
            return
        time.sleep(0.1)
