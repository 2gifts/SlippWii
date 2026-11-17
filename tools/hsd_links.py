"""Resolves the "slp:" links of Slippi's game files from the player's own disc.

    python tools/hsd_links.py resolve ARCHIVE --disc FOLDER -o OUT
    python tools/hsd_links.py info ARCHIVE

From the version after 3.6.4, two of the files Slippi ships (GameSetup_gui.dat, SlippiCSS_gui.dat) no
longer hold Nintendo's assets. Where an asset was, the file has an external symbol

    slp:<disc file>:<root symbol>/<field>/<field>...[:at=0x<offset>][:cut=<field>,...][:sha=<8 hex>][:strict=1]

and Slippi Dolphin copies the asset in from the disc when the game loads the file. This is that
step, as slippi-rust-extensions' `hsd` crate does it (archive.rs, link.rs, hash.rs; GPLv3, the Slippi
team), with the same layout rules, so the result is byte for byte Dolphin's: tools/test_hsd_links.py
compares it with the output of Slippi's own tool.

An archive (.dat/.usd) is a 0x20-byte header, a data section, a table of the data's pointer fields,
tables of exported ("root") and imported ("external") symbols, and their names. The data is cut into
extents at every address something points to; an asset is an extent and all it points to; copying
one in is copying its extents and renumbering the pointers.

Only what an unmodified disc needs is done here: a link is followed by its path (or offset) in the
file it names, and the asset found must have the hash the link records. Slippi's tool goes on to
search a modified disc for a moved asset; this says the disc is not the original instead, as
SlippWii's patches for the disc's menu files do.
"""
import argparse
import bisect
import hashlib
import struct
import sys
from pathlib import Path

HEADER_SIZE = 0x20
CHAIN_END = 0xFFFFFFFF
ALIGN = 32
LINK_PREFIX = 'slp:'


class LinkError(Exception):
    pass


class Extent:
    __slots__ = ('data', 'phase', 'pointers', 'origin')

    def __init__(self, data, phase, pointers, origin):
        self.data = data  # bytearray; pointer and chain fields are zero in it
        self.phase = phase  # the data offset modulo 32 it must be placed at
        self.pointers = pointers  # [(field offset in the extent, target extent)]
        self.origin = origin  # its data offset in the file it was read from


class Archive:
    def __init__(self):
        self.version = b'\0\0\0\0'
        self.header_tail = b'\0' * 8
        self.extents = []
        self.roots = []  # [(name, extent)]
        self.refs = []  # [(name, [(extent, field)])]
        self.strings = None  # (bytes, root offsets, ref offsets) of the file read, while the symbols are unchanged


def _cstr(table, at):
    if at > len(table):
        raise LinkError('a symbol name lies outside the file')
    end = table.find(b'\0', at)
    return table[at:end if end >= 0 else len(table)].decode('utf-8', 'replace')


def parse(raw):
    if len(raw) < HEADER_SIZE:
        raise LinkError('too short to be an archive')
    file_size, data_size, reloc_count, root_count, ref_count = struct.unpack('>5I', raw[:20])
    if file_size != len(raw):
        raise LinkError(f'the header says {file_size} bytes, the file has {len(raw)}')
    a = Archive()
    a.version, a.header_tail = raw[0x14:0x18], raw[0x18:0x20]
    data_end = HEADER_SIZE + data_size
    roots_off = data_end + reloc_count * 4
    refs_off = roots_off + root_count * 8
    strings_off = refs_off + ref_count * 8
    if len(raw) < strings_off:
        raise LinkError('the tables run past the end of the file')
    data, strings = raw[HEADER_SIZE:data_end], raw[strings_off:]

    fields, bounds = [], {0, data_size}
    for field in struct.unpack(f'>{reloc_count}I', raw[data_end:roots_off]):
        if field + 4 > len(data):
            raise LinkError(f'a pointer field at 0x{field:x} lies outside the data')
        target = struct.unpack_from('>I', data, field)[0]
        if target > data_size:
            raise LinkError(f'the pointer at 0x{field:x} leads outside the data')
        bounds.add(target)
        fields.append((field, target))
    roots = []
    for i in range(root_count):
        target, name_at = struct.unpack_from('>II', raw, roots_off + i * 8)
        if target > data_size:
            raise LinkError('a root symbol leads outside the data')
        bounds.add(target)
        roots.append((_cstr(strings, name_at), target, name_at))
    refs = []
    for i in range(ref_count):
        at, name_at = struct.unpack_from('>II', raw, refs_off + i * 8)
        chain = []
        while True:
            if at + 4 > len(data) or len(chain) > len(data) // 4:
                raise LinkError('a broken chain of external references')
            chain.append(at)
            at = struct.unpack_from('>I', data, at)[0]
            if at == CHAIN_END:
                break
        refs.append((_cstr(strings, name_at), chain, name_at))

    # (something may point at the very end of the data: an empty struct, kept so the pointer survives)
    end_targeted = any(t == data_size for _, t in fields) or any(t == data_size for _, t, _ in roots)
    starts = [b for b in sorted(bounds) if b < data_size or (end_targeted and b == data_size)]
    index_of = {start: i for i, start in enumerate(starts)}
    for i, start in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else data_size
        a.extents.append(Extent(bytearray(data[start:end]), start % ALIGN, [], start))

    def locate(field):
        idx = bisect.bisect_right(starts, field) - 1
        within = field - starts[idx]
        if within + 4 > len(a.extents[idx].data):
            raise LinkError(f'the pointer field at 0x{field:x} straddles two structs')
        return idx, within

    for field, target in fields:
        idx, within = locate(field)
        a.extents[idx].data[within:within + 4] = b'\0\0\0\0'
        a.extents[idx].pointers.append((within, index_of[target]))
    a.roots = [(name, index_of[target]) for name, target, _ in roots]
    for name, chain, _ in refs:
        locations = []
        for at in chain:
            idx, within = locate(at)
            a.extents[idx].data[within:within + 4] = b'\0\0\0\0'
            locations.append((idx, within))
        a.refs.append((name, locations))
    a.strings = (bytes(strings), [r[2] for r in roots], [r[2] for r in refs])
    return a


def layout(a):
    offsets, cursor = [], 0
    for e in a.extents:
        cursor += (e.phase + ALIGN - cursor % ALIGN) % ALIGN
        offsets.append(cursor)
        cursor += len(e.data)
    return offsets, cursor


def serialize(a):
    offsets, data_size = layout(a)
    data, relocs = bytearray(data_size), []
    for i, e in enumerate(a.extents):
        base = offsets[i]
        data[base:base + len(e.data)] = e.data
        for field, target in e.pointers:
            struct.pack_into('>I', data, base + field, offsets[target])
            relocs.append(base + field)
    relocs.sort()
    heads = []
    for name, locations in a.refs:
        if not locations:
            raise LinkError(f'{name}: an external reference nothing uses')
        places = [offsets[e] + f for e, f in locations]
        for n, at in enumerate(places):
            struct.pack_into('>I', data, at, places[n + 1] if n + 1 < len(places) else CHAIN_END)
        heads.append(places[0])
    strings = None
    if a.strings and len(a.strings[1]) == len(a.roots) and len(a.strings[2]) == len(a.refs) and \
            all(_cstr(a.strings[0], o) == n for o, (n, _) in zip(a.strings[1], a.roots)) and \
            all(_cstr(a.strings[0], o) == n for o, (n, _) in zip(a.strings[2], a.refs)):
        strings, root_str, ref_str = a.strings
    if strings is None:
        table, root_str, ref_str = bytearray(), [], []
        for name, _ in a.roots:
            root_str.append(len(table))
            table += name.encode() + b'\0'
        for name, _ in a.refs:
            ref_str.append(len(table))
            table += name.encode() + b'\0'
        strings = bytes(table)
    size = HEADER_SIZE + len(data) + len(relocs) * 4 + (len(a.roots) + len(a.refs)) * 8 + len(strings)
    out = bytearray(struct.pack('>5I', size, data_size, len(relocs), len(a.roots), len(a.refs)) + a.version + a.header_tail)
    out += data
    out += struct.pack(f'>{len(relocs)}I', *relocs)
    for (_, extent), at in zip(a.roots, root_str):
        out += struct.pack('>II', offsets[extent], at)
    for head, at in zip(heads, ref_str):
        out += struct.pack('>II', head, at)
    return bytes(out + strings)


# ---- links -----------------------------------------------------------------------------------

class Locator:
    def __init__(self, symbol):
        def bad(why):
            return LinkError(f'{symbol}: {why}')

        def number(text):
            try:
                return int(text[2:] if text[:2] in ('0x', '0X') else text, 16)
            except ValueError:
                raise bad(f'bad number {text}')

        if not symbol.startswith(LINK_PREFIX):
            raise bad('not a link')
        parts = symbol[len(LINK_PREFIX):].split(':')
        if len(parts) < 2 or not parts[0]:
            raise bad('no file or no anchor')
        self.file, anchor = parts[0], parts[1]
        self.root, self.hops, self.offset = None, [], None
        if '/' in anchor:
            self.root, _, hops = anchor.partition('/')
            if not self.root:
                raise bad('no root symbol')
            for hop in [h for h in hops.split('/') if h]:
                field, _, count = hop.partition('*')
                self.hops += [number(field)] * (int(count) if count else 1)
        else:
            self.offset = number(anchor)
            if self.offset < HEADER_SIZE:
                raise bad('the offset is inside the header')
        self.at, self.cut, self.sha, self.strict = None, [], None, False
        for part in parts[2:]:
            key, _, value = part.partition('=')
            if key == 'at':
                self.at = number(value)
            elif key == 'cut':
                self.cut = [number(v) for v in value.split(',')]
            elif key == 'sha' and len(value) == 8:
                self.sha = value.lower()
            elif part == 'strict=1':
                self.strict = True
            else:
                raise bad(f'unknown option {part}')


def reachable(a, start, cut):
    """Extents reachable from `start` through pointers, breadth first; `cut`: fields of `start` not followed."""
    seen, order, queue = {start}, [], [start]
    while queue:
        idx = queue.pop(0)
        order.append(idx)
        for field, target in a.extents[idx].pointers:
            if (idx == start and field in cut) or target in seen:
                continue
            seen.add(target)
            queue.append(target)
    return order


def asset_sha(a, extent, cut):
    """The first 8 hex digits of an asset's structural hash, as a link's `sha` records it: of each struct,
    its bytes without trailing zeros and, in field order, each pointer's field and the hash of what it
    points to (a struct met again while it is being hashed counts as "cycle")."""
    memo, busy = {}, set()
    cycle = hashlib.sha1(b'cycle').digest()

    def enter(idx, top):
        e = a.extents[idx]
        busy.add(idx)
        return [idx, hashlib.sha1(bytes(e.data).rstrip(b'\0')), sorted(p for p in e.pointers if not (top and p[0] in cut)), 0]

    # (depth first, without recursion: lists of hundreds of structs hang one inside the other)
    stack, result = [enter(extent, True)], None
    while stack:
        frame = stack[-1]
        idx, sha, pointers, at = frame
        if at == len(pointers):
            busy.discard(idx)
            result = memo[idx] = sha.digest()
            stack.pop()
            continue
        field, target = pointers[at]
        if target in memo:
            child = memo[target]
        elif target in busy:
            child = cycle
        else:
            stack.append(enter(target, False))
            continue
        sha.update(struct.pack('>I', field) + child)
        frame[3] += 1
    return result.hex()[:8]


def find(src, locator, symbol):
    """The extent a link names in its disc file (an unmodified disc: see the module's text)."""
    if locator.root is None:
        hits = [i for i, e in enumerate(src.extents) if e.origin == locator.offset - HEADER_SIZE]
        if not hits:
            raise LinkError(f'{symbol}: no struct at 0x{locator.offset:x} of {locator.file}: not the original disc?')
        cur = hits[0]
    else:
        roots = [extent for name, extent in src.roots if name == locator.root]
        if not roots:
            raise LinkError(f'{symbol}: {locator.file} has no {locator.root}: not the original disc?')
        cur = roots[0]
        for n, hop in enumerate(locator.hops):
            nxt = [t for f, t in src.extents[cur].pointers if f == hop]
            if not nxt:
                raise LinkError(f'{symbol}: step {n} (field 0x{hop:x}) is not a pointer in {locator.file}: not the original disc?')
            cur = nxt[0]
    if locator.sha is not None:
        got = asset_sha(src, cur, locator.cut)
        if got != locator.sha:
            raise LinkError(f"{symbol}: the disc's asset is not the one the link records ({got}): not the original disc?")
    return cur


def resolve(a, load):
    """A copy of archive `a` with every slp: link's asset copied in. load(name) -> a disc file's bytes.
    Returns (archive, links resolved, disc files read)."""
    out = Archive()
    out.version, out.header_tail, out.strings = a.version, a.header_tail, a.strings
    out.extents = [Extent(bytearray(e.data), e.phase, list(e.pointers), e.origin) for e in a.extents]
    out.roots = list(a.roots)
    sources, read, links = {}, [], 0
    for name, locations in a.refs:
        if not name.startswith(LINK_PREFIX):
            out.refs.append((name, list(locations)))
            continue
        locator = Locator(name)
        if locator.file not in sources:
            raw = load(locator.file)
            if raw is None:
                raise LinkError(f'{locator.file} is not on the disc')
            sources[locator.file] = parse(raw)
            read.append(locator.file)
        src = sources[locator.file]
        root = find(src, locator, name)
        ordered = sorted(reachable(src, root, locator.cut), key=lambda i: src.extents[i].origin)
        new_index = {}
        for i in ordered:
            new_index[i] = len(out.extents)
            e = src.extents[i]
            out.extents.append(Extent(bytearray(e.data), e.phase, [], e.origin))
        for i in ordered:
            out.extents[new_index[i]].pointers = [
                (field, new_index[target]) for field, target in src.extents[i].pointers
                if not (i == root and field in locator.cut) and target in new_index]
        for extent, field in locations:
            out.extents[extent].pointers.append((field, new_index[root]))
        links += 1
    if links:
        out.strings = None  # the symbols changed: the names are written anew
    return out, links, read


def resolve_file(raw, load):
    """The bytes of an archive with its links resolved, and how many there were."""
    archive, links, read = resolve(parse(raw), load)
    return serialize(archive), links, read


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('resolve')
    r.add_argument('archive')
    r.add_argument('--disc', required=True, help="a folder of the disc's files")
    r.add_argument('-o', '--out', required=True)
    i = sub.add_parser('info')
    i.add_argument('archive')
    args = ap.parse_args()
    raw = Path(args.archive).read_bytes()
    if args.cmd == 'info':
        a = parse(raw)
        print(f'{len(a.extents)} extents, {sum(len(e.data) for e in a.extents)} data bytes, '
              f'{sum(len(e.pointers) for e in a.extents)} pointers; same bytes when written again: {serialize(a) == raw}')
        for name, extent in a.roots:
            print(f'  root {name} -> 0x{a.extents[extent].origin:x}')
        for name, locations in a.refs:
            print(f'  {"link" if name.startswith(LINK_PREFIX) else "external"} {name} ({len(locations)} places)')
        return
    try:
        out, links, read = resolve_file(raw, lambda n: (Path(args.disc) / n).read_bytes() if (Path(args.disc) / n).exists() else None)
    except LinkError as e:
        sys.exit(str(e))
    Path(args.out).write_bytes(out)
    print(f'{links} links resolved from {len(read)} disc files ({", ".join(read)}): {args.out}, {len(out)} bytes, '
          f'sha256 {hashlib.sha256(out).hexdigest()[:16]}')


if __name__ == '__main__':
    main()
