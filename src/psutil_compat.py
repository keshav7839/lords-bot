import os
import signal
import time
import glob
import subprocess
import re

STATUS_ZOMBIE = 'zombie'

class NoSuchProcess(Exception):
    pass

class Process:
    def __init__(self, pid):
        self.pid = pid
        self._proc = None
        try:
            os.kill(pid, 0)
        except OSError:
            raise NoSuchProcess(f"No process with PID {pid}")

    def children(self, recursive=True):
        return []

    def terminate(self):
        try:
            os.kill(self.pid, signal.SIGTERM)
        except OSError:
            pass

    def wait(self, timeout=None):
        start = time.time()
        while True:
            try:
                os.kill(self.pid, 0)
            except OSError:
                return
            if timeout and time.time() - start > timeout:
                raise subprocess.TimeoutExpired(self.pid, timeout)
            time.sleep(0.1)

    def is_running(self):
        try:
            os.kill(self.pid, 0)
            return True
        except OSError:
            return False

    def status(self):
        try:
            os.kill(self.pid, 0)
            return 'running'
        except OSError:
            return STATUS_ZOMBIE

    def cpu_percent(self, interval=0.1):
        return 0.0

    def memory_info(self):
        class MemInfo:
            rss = 0
        return MemInfo()

    def __repr__(self):
        return f"Process(pid={self.pid})"

def pid_exists(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False

def wait_procs(procs, timeout=None):
    gone = []
    alive = []
    for p in procs:
        try:
            os.kill(p.pid, 0)
            alive.append(p)
        except OSError:
            gone.append(p)
    return gone, alive

# ──────────────────────────────────────────────────────────────────────────────
# REAL container stats — cgroup v2/v1 aware (no host /proc/stat readings)
# ──────────────────────────────────────────────────────────────────────────────

def _cgroup_mem():
    """Return (limit_bytes, usage_bytes) from cgroup, or (None, None)."""
    # v2
    try:
        with open('/sys/fs/cgroup/memory.max') as f:
            lim_raw = f.read().strip()
        with open('/sys/fs/cgroup/memory.current') as f:
            usage = int(f.read().strip())
        limit = float('inf') if lim_raw == 'max' else int(lim_raw)
        return limit, usage
    except Exception:
        pass
    # v1
    try:
        with open('/sys/fs/cgroup/memory/memory.limit_in_bytes') as f:
            limit = int(f.read().strip())
        with open('/sys/fs/cgroup/memory/memory.usage_in_bytes') as f:
            usage = int(f.read().strip())
        return limit, usage
    except Exception:
        return None, None

def _proc_meminfo():
    """Host-wide fallback from /proc/meminfo."""
    try:
        with open('/proc/meminfo') as f:
            data = {}
            for line in f:
                parts = line.split(':')
                if len(parts) == 2:
                    key = parts[0].strip()
                    val = parts[1].strip().split()[0]
                    try:
                        data[key] = int(val)
                    except Exception:
                        pass
        return data.get('MemTotal', 0) * 1024, data.get('MemAvailable', data.get('MemFree', 0)) * 1024
    except Exception:
        return 0, 0

cpu_count = None
def _get_cpu_count():
    """Real CPU quota of THIS container (not the host)."""
    global cpu_count
    if cpu_count is not None:
        return cpu_count
    # cgroup v2: cpu.max = "quota period" → cores = quota/period
    try:
        with open('/sys/fs/cgroup/cpu.max') as f:
            parts = f.read().split()
        quota = float(parts[0])
        period = float(parts[1])
        if quota > 0 and period > 0:
            cores = quota / period
            if 0 < cores <= 256:
                cpu_count = max(1, int(round(cores)))
                return cpu_count
    except Exception:
        pass
    # cgroup v1: cpu.cfs_quota_us / cpu.cfs_period_us
    try:
        with open('/sys/fs/cgroup/cpu/cpu.cfs_quota_us') as f:
            quota = int(f.read().strip())
        with open('/sys/fs/cgroup/cpu/cpu.cfs_period_us') as f:
            period = int(f.read().strip())
        if quota > 0 and period > 0:
            cpu_count = max(1, int(round(quota / period)))
            return cpu_count
    except Exception:
        pass
    # affinity fallback
    try:
        cpu_count = len(os.sched_getaffinity(0))
        if cpu_count and cpu_count < 256:
            return cpu_count
    except Exception:
        pass
    try:
        cpu_count = int(subprocess.check_output(['nproc'], timeout=3).strip())
    except Exception:
        cpu_count = 1
    return cpu_count

_cpu_prev = None
def _cgroup_cpu_usage_us():
    """CPU microseconds used by this cgroup (v2 cpu.stat → v1 cpuacct.usage)."""
    try:
        with open('/sys/fs/cgroup/cpu.stat') as f:
            for line in f:
                if line.startswith('usage_usec'):
                    return int(line.split()[1])
    except Exception:
        pass
    try:
        with open('/sys/fs/cgroup/cpuacct/cpuacct.usage') as f:
            return int(f.read().strip())
    except Exception:
        return None

def cpu_percent(interval=0.5):
    """Percent of the container's CPU quota actually used (real, cgroup-accurate)."""
    global _cpu_prev
    u1 = _cgroup_cpu_usage_us()
    if u1 is None:
        return 0.0
    time.sleep(interval)
    u2 = _cgroup_cpu_usage_us()
    if u2 is None or u2 < u1:
        return 0.0
    cores = _get_cpu_count()
    total_us = interval * 1_000_000 * cores
    if total_us <= 0:
        return 0.0
    pct = (u2 - u1) / total_us * 100
    return round(max(0.0, min(pct, 100.0)), 1)

class virtual_memory:
    def __init__(self):
        limit, usage = _cgroup_mem()
        if limit is not None and usage is not None and limit not in (0, float('inf')):
            self.total = limit
            self.used = usage
            self.available = max(self.total - self.used, 0)
            self.percent = round((self.used / self.total) * 100, 1)
        else:
            total_b, avail_b = _proc_meminfo()
            self.total = total_b
            self.available = avail_b
            self.used = max(total_b - avail_b, 0)
            self.percent = round((self.used / self.total) * 100, 1) if self.total > 0 else 0
        self.free = self.available

_SKIP_DIRS = frozenset((
    '/proc', '/sys', '/dev', '/usr', '/lib', '/lib64', '/bin', '/sbin',
    '/etc', '/var', '/root', '/boot', '/opt', '/snap', '/run',
))

def _dir_usage_bytes(path, deadline=2.0):
    total = 0
    start = time.time()
    for root, dirs, files in os.walk(path):
        if time.time() - start > deadline:
            break
        rel = root.split('/')
        skip = False
        for d in rel:
            if d in ('proc', 'sys', 'dev', 'usr', 'lib', 'bin', 'sbin',
                     'etc', 'var', 'root', 'boot', 'opt', 'snap', 'run'):
                skip = True
                break
        if skip:
            dirs[:] = []
            continue
        for f in files:
            if time.time() - start > deadline:
                break
            try:
                total += os.path.getsize(os.path.join(root, f))
            except Exception:
                pass
    return total

_SPACE_DISK_TOTAL = None
def _space_disk_total():
    """Real disk quota for the Space (default: 50 GB free tier), env-overridable."""
    global _SPACE_DISK_TOTAL
    if _SPACE_DISK_TOTAL is not None:
        return _SPACE_DISK_TOTAL
    try:
        gb = int(os.environ.get('SPACE_DISK_GB', '50'))
    except Exception:
        gb = 50
    _SPACE_DISK_TOTAL = gb * 1024**3
    return _SPACE_DISK_TOTAL

class disk_usage:
    """Real usage of the Space's own data dirs, not the host's overlay filesystem."""
    def __init__(self, path='/'):
        quota = _space_disk_total()
        self.total = quota
        used = 0
        dirs = ['/data', '/app', '/home/user/app', '/content']
        if path and path not in ('/', ''):
            dirs.insert(0, path)
        for p in dirs:
            if p and os.path.isdir(p):
                try:
                    used += _dir_usage_bytes(p)
                except Exception:
                    pass
        self.used = used
        self.free = max(quota - used, 0)
        self.percent = round((self.used / quota) * 100, 1) if quota > 0 else 0
