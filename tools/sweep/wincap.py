"""Captures a process's main window to a PNG (PrintWindow with full content,
which also works for GPU-rendered windows such as Dolphin's).

    python tools/sweep/wincap.py PID out.png
"""
import ctypes
import ctypes.wintypes as wt
import sys

from PIL import Image

user32, gdi32 = ctypes.WinDLL('user32'), ctypes.WinDLL('gdi32')
PW_RENDERFULLCONTENT = 2


def windows_of(pid):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            r = wt.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(r))
            found.append((hwnd, (r.right - r.left) * (r.bottom - r.top)))
        return True

    user32.EnumWindows(cb, 0)
    return [h for h, _ in sorted(found, key=lambda x: -x[1])]


def capture(pid, path):
    hwnds = windows_of(pid)
    if not hwnds:
        raise OSError(f'no window for process {pid}')
    hwnd = hwnds[0]
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right, r.bottom
    hdc = user32.GetDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)
    user32.PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT | 1)  # client area only
    buf = ctypes.create_string_buffer(w * h * 4)

    class BMIH(ctypes.Structure):
        _fields_ = [('biSize', wt.DWORD), ('biWidth', ctypes.c_long), ('biHeight', ctypes.c_long),
                    ('biPlanes', wt.WORD), ('biBitCount', wt.WORD), ('biCompression', wt.DWORD),
                    ('biSizeImage', wt.DWORD), ('biXPelsPerMeter', ctypes.c_long),
                    ('biYPelsPerMeter', ctypes.c_long), ('biClrUsed', wt.DWORD), ('biClrImportant', wt.DWORD)]
    bi = BMIH(ctypes.sizeof(BMIH), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)
    img = Image.frombuffer('RGBA', (w, h), buf, 'raw', 'BGRA', 0, 1).convert('RGB')
    img.save(path)
    return path


if __name__ == '__main__':
    print(capture(int(sys.argv[1]), sys.argv[2]))
