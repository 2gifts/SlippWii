"""Opts a process out of Windows 11's background throttling: its timer
resolution requests count even while its window is covered (Dolphin's frame
limiter otherwise sleeps in 15.6 ms steps: 47.5 frames a second instead of
60), and no EcoQoS.

    python tools/sweep/unthrottle.py PID
"""
import ctypes
import ctypes.wintypes as wt
import sys

PROCESS_SET_INFORMATION = 0x0200
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
    finally:
        k32.CloseHandle(h)


if __name__ == '__main__':
    unthrottle(int(sys.argv[1]))
    print('unthrottled', sys.argv[1])
