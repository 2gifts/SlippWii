"""Opts a process out of Windows 11's background throttling: its timer
resolution requests count even while its window is covered (Dolphin's frame
limiter otherwise sleeps in 15.6 ms steps: 47.5 frames a second instead of
60), and no EcoQoS. It also runs at high priority, so a busy PC slows
anything else first: a starved Dolphin falls behind and the Wii waits for it.

    python tools/sweep/unthrottle.py PID
"""
import ctypes
import ctypes.wintypes as wt
import sys

PROCESS_SET_INFORMATION = 0x0200
HIGH_PRIORITY_CLASS = 0x80
ProcessPowerThrottling = 4
PROCESS_POWER_THROTTLING_CURRENT_VERSION = 1
EXECUTION_SPEED = 0x1
IGNORE_TIMER_RESOLUTION = 0x4


class PowerThrottling(ctypes.Structure):
    _fields_ = [('Version', wt.ULONG), ('ControlMask', wt.ULONG), ('StateMask', wt.ULONG)]


def unthrottle(pid):
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    h = k32.OpenProcess(PROCESS_SET_INFORMATION, False, pid)
    if not h:
        raise OSError(f'cannot open process {pid} ({ctypes.get_last_error()})')
    try:
        # control both behaviours, state off: never throttled, timer requests honoured
        state = PowerThrottling(PROCESS_POWER_THROTTLING_CURRENT_VERSION, EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION, 0)
        if not k32.SetProcessInformation(h, ProcessPowerThrottling, ctypes.byref(state), ctypes.sizeof(state)):
            raise OSError(f'SetProcessInformation failed ({ctypes.get_last_error()})')
        if not k32.SetPriorityClass(h, HIGH_PRIORITY_CLASS):
            raise OSError(f'SetPriorityClass failed ({ctypes.get_last_error()})')
    finally:
        k32.CloseHandle(h)


if __name__ == '__main__':
    unthrottle(int(sys.argv[1]))
    print('unthrottled', sys.argv[1])


class CpuLoad:
    """The whole PC's CPU load between calls to sample() (GetSystemTimes)."""

    def __init__(self):
        self.k32 = ctypes.WinDLL('kernel32', use_last_error=True)
        self.last = self._times()
        self.samples = []

    def _times(self):
        idle, kernel, user = wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
        self.k32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user))
        return [(t.dwHighDateTime << 32) | t.dwLowDateTime for t in (idle, kernel, user)]

    def sample(self):
        now = self._times()
        idle, kernel, user = (a - b for a, b in zip(now, self.last))
        self.last = now
        if kernel + user:  # kernel time includes idle time
            self.samples.append(100 * (kernel + user - idle) / (kernel + user))

    def summary(self):
        if not self.samples:
            return 'not measured'
        return f'average {sum(self.samples) / len(self.samples):.0f}%, peak {max(self.samples):.0f}%'
