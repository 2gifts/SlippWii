"""Reads and writes a running Dolphin's emulated GameCube RAM (MEM1), as
Dolphin Memory Engine does: MEM1 is a 32 MB mapped view in Dolphin's
process whose first bytes are the game ID once a game has booted.

    from dolphin_mem import Dolphin
    d = Dolphin(pid)            # or Dolphin.find() for the only one running
    d.u32(0x80479D30); d.write_u32(addr, value)

    python tools/sweep/dolphin_mem.py PID ADDR [COUNT]   # dump words
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys

PROCESS_ALL = 0x0438  # QUERY_INFORMATION | VM_READ | VM_WRITE | VM_OPERATION
MEM_MAPPED = 0x40000
MEM1_SIZE = 0x2000000
k32 = ctypes.WinDLL('kernel32', use_last_error=True)


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p),
                ('AllocationProtect', wt.DWORD), ('PartitionId', wt.WORD), ('RegionSize', ctypes.c_size_t),
                ('State', wt.DWORD), ('Protect', wt.DWORD), ('Type', wt.DWORD)]


class Dolphin:
    def __init__(self, pid, game_id=b'GALE01'):
        self.h = k32.OpenProcess(PROCESS_ALL, False, pid)
        if not self.h:
            raise OSError(f'cannot open process {pid} ({ctypes.get_last_error()})')
        self.base = self._find(game_id)

    def _find(self, game_id):
        addr, mbi = 0, MBI()
        while k32.VirtualQueryEx(self.h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            size = mbi.RegionSize
            if mbi.Type == MEM_MAPPED and size >= MEM1_SIZE and mbi.State == 0x1000:
                head = self._read_raw(mbi.BaseAddress, 6)
                if head == game_id:
                    return mbi.BaseAddress
            addr = (mbi.BaseAddress or 0) + size
            if addr >= 0x7FFFFFFFFFFF:
                break
        raise OSError('no MEM1 with the game booted (is Melee running?)')

    def _read_raw(self, address, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(address), buf, n, ctypes.byref(got)):
            return None
        return buf.raw[:got.value]

    def read(self, address, n):
        off = address & 0x01FFFFFF
        data = self._read_raw(self.base + off, n)
        if data is None:
            raise OSError(f'read {address:08x} failed')
        return data

    def write(self, address, data):
        off = address & 0x01FFFFFF
        done = ctypes.c_size_t()
        if not k32.WriteProcessMemory(self.h, ctypes.c_void_p(self.base + off), data, len(data), ctypes.byref(done)):
            raise OSError(f'write {address:08x} failed')

    def u8(self, a):
        return self.read(a, 1)[0]

    def u16(self, a):
        return struct.unpack('>H', self.read(a, 2))[0]

    def u32(self, a):
        return struct.unpack('>I', self.read(a, 4))[0]

    def f32(self, a):
        return struct.unpack('>f', self.read(a, 4))[0]

    def write_u8(self, a, v):
        self.write(a, bytes([v & 0xFF]))

    def write_u32(self, a, v):
        self.write(a, struct.pack('>I', v & 0xFFFFFFFF))

    def scene(self):
        """(major, minor) from Melee's scene controller (0x80479D30)."""
        w = self.u32(0x80479D30)
        return w >> 24, w & 0xFF


if __name__ == '__main__':
    d = Dolphin(int(sys.argv[1]))
    a = int(sys.argv[2], 16)
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    print(f'MEM1 at {d.base:#x}; scene {d.scene()}')
    for i in range(0, n * 4, 16):
        print(f'{a + i:08x}: ' + ' '.join(f'{d.u32(a + i + j):08x}' for j in range(0, min(16, n * 4 - i), 4)))
