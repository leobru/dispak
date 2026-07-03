#!/usr/bin/env python3
"""Create АРФА области (archive regions) from the Unix command line.

dispak stores each АРФА область as a separate Unix file (an область-каталог is
a directory) under an archive root, with a shared, fcntl-locked catalog index
file "<root>/.catalog" holding the metadata that has no file-system
representation (owner шифр, identifier, passwords, access list, захват holders).
See dispak/arfa.c (ЭК 063 "КЛЮЧАР", создание) and dispak/disk.c for the
authoritative formats reproduced here.

This tool performs the equivalent of ИС=01 создание области directly, so an
archive can be pre-populated without running a BESM-6 program.  It takes the
same catalog lock a running dispak does, so it is safe to use concurrently.

Composite (каталог) names use the dot as the level separator, as in ДИСПАК;
each каталог level is a directory on disk.

Examples:
    mkarfa.py --owner 419999 --length 2 тест1
    mkarfa.py --owner 419999 --catalog тесткат
    mkarfa.py --owner 419999 --length 4 тесткат.суб
    mkarfa.py --arfa-dir arfa-work --list
"""

import argparse
import fcntl
import os
import struct
import sys
import time

# ---------------------------------------------------------------------------
# Constants mirrored from dispak (arfa.c, arfa.h, diski.h)
# ---------------------------------------------------------------------------
ARFA_MAGIC   = 0x41524641          # "ARFA"
ARFA_MAXREC  = 128
ARFA_PATHLEN = 96
ARFA_MAXACL  = 6
ARFA_MAXZONES = 0o1000             # 512 zones, max область length

GOST_DOT   = 0o16               # каталог level separator in область names
GOST_SPACE = 0o17
GOST_EOF   = 0o377

CWORDS      = 8                    # uint64 control words per zone
DATAWORDS   = 1024                 # uint64 data words per zone
ZONE_BYTES  = (CWORDS + DATAWORDS) * 8   # sizeof(zone_t) == 8256

# arfa_rec_t: <6B H 5I 6I 3i 96s>  (natural C layout, no implicit padding)
REC_FMT  = '<6B H 5I 6I 3i {}s'.format(ARFA_PATHLEN)
REC_SIZE = struct.calcsize(REC_FMT)               # 160
CAT_HDR  = '<II'                                   # magic, idseq
CAT_SIZE = struct.calcsize(CAT_HDR) + ARFA_MAXREC * REC_SIZE   # 20488

assert REC_SIZE == 160, REC_SIZE
assert ZONE_BYTES == 8256

# ---------------------------------------------------------------------------
# GOST-10859 encoding tables, ported verbatim from dispak/encoding.c
# ---------------------------------------------------------------------------
# gost_to_unicode_cyr[256] (cyrillic default, gost_latin == 0)
GOST_TO_UNICODE = [0] * 256
_g2u = [
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,   # 000-007
    0x38, 0x39, 0x2b, 0x2d, 0x2f, 0x2c, 0x2e, 0x20,   # 010-017
    0x23e8, 0x2191, 0x28, 0x29, 0xd7, 0x3d, 0x3b, 0x5b,  # 020-027
    0x5d, 0x2a, 0x2018, 0x2019, 0x2260, 0x3c, 0x3e, 0x3a, # 030-037
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,  # 040-047
    0x0418, 0x0419, 0x041a, 0x041b, 0x041c, 0x041d, 0x041e, 0x041f,  # 050-057
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,  # 060-067
    0x0428, 0x0429, 0x042b, 0x042c, 0x042d, 0x042e, 0x042f, 0x44,    # 070-077
    0x46, 0x47, 0x49, 0x4a, 0x4c, 0x4e, 0x51, 0x52,   # 100-107
    0x53, 0x55, 0x56, 0x57, 0x5a, 0x203e, 0x2a7d, 0x2a7e,  # 110-117
    0x2228, 0x2227, 0x2283, 0xac, 0xf7, 0x2261, 0x25, 0x25c7,  # 120-127
    0x7c, 0x2015, 0x5f, 0x21, 0x22, 0x042a, 0xb0, 0x2032,  # 130-137
]
for _i, _v in enumerate(_g2u):
    GOST_TO_UNICODE[_i] = _v
GOST_TO_UNICODE[0o174] = 0x2424
GOST_TO_UNICODE[0o175] = 0x5c

# unicode_to_gost: tab0[256] for U+0000..U+00FF, ported verbatim.
_O = 0o17   # GOST_SPACE, the "no mapping" fallback used by dispak
UNI_TAB0 = [
    _O, _O, _O, _O, _O, _O, _O, _O,   # 00-07
    _O, _O, 0o214, _O, _O, 0o174, _O, _O,  # 08-0f
    _O, _O, _O, _O, _O, _O, _O, _O,   # 10-17
    _O, _O, _O, _O, _O, _O, _O, _O,   # 18-1f
    0o17, 0o133, 0o134, 0o34, 0o127, 0o126, 0o121, 0o33,   #  !"#$%&'
    0o22, 0o23, 0o31, 0o12, 0o15, 0o13, 0o16, 0o14,        # ()*+,-./
    0o00, 0o01, 0o02, 0o03, 0o04, 0o05, 0o06, 0o07,        # 01234567
    0o10, 0o11, 0o37, 0o26, 0o35, 0o25, 0o36, 0o136,       # 89:;<=>?
    0o21, 0o40, 0o42, 0o61, 0o77, 0o45, 0o100, 0o101,      # @ABCDEFG
    0o55, 0o102, 0o103, 0o52, 0o104, 0o54, 0o105, 0o56,    # HIJKLMNO
    0o60, 0o106, 0o107, 0o110, 0o62, 0o111, 0o112, 0o113,  # PQRSTUVW
    0o65, 0o63, 0o114, 0o27, 0o175, 0o30, 0o115, 0o132,    # XYZ[\]^_
    0o32, 0o40, 0o42, 0o61, 0o77, 0o45, 0o100, 0o101,      # `abcdefg
    0o55, 0o102, 0o103, 0o52, 0o104, 0o54, 0o105, 0o56,    # hijklmno
    0o60, 0o106, 0o107, 0o110, 0o62, 0o111, 0o112, 0o113,  # pqrstuvw
    0o65, 0o63, 0o114, 0o125, 0o130, _O, 0o123, _O,        # xyz{|}~
]
UNI_TAB0 += [_O] * (256 - len(UNI_TAB0))
UNI_TAB0[0xac] = 0o123
UNI_TAB0[0xb0] = 0o136
UNI_TAB0[0xd7] = 0o24
UNI_TAB0[0xf7] = 0o124

# U+0410..U+044F cyrillic (both cases fold to the uppercase GOST letters)
_CYR = {
    0x10: 0o40, 0x11: 0o41, 0x12: 0o42, 0x13: 0o43, 0x14: 0o44, 0x15: 0o45,
    0x16: 0o46, 0x17: 0o47, 0x18: 0o50, 0x19: 0o51, 0x1a: 0o52, 0x1b: 0o53,
    0x1c: 0o54, 0x1d: 0o55, 0x1e: 0o56, 0x1f: 0o57, 0x20: 0o60, 0x21: 0o61,
    0x22: 0o62, 0x23: 0o63, 0x24: 0o64, 0x25: 0o65, 0x26: 0o66, 0x27: 0o67,
    0x28: 0o70, 0x29: 0o71, 0x2a: 0o135, 0x2b: 0o72, 0x2c: 0o73, 0x2d: 0o74,
    0x2e: 0o75, 0x2f: 0o76,
    0x30: 0o40, 0x31: 0o41, 0x32: 0o42, 0x33: 0o43, 0x34: 0o44, 0x35: 0o45,
    0x36: 0o46, 0x37: 0o47, 0x38: 0o50, 0x39: 0o51, 0x3a: 0o52, 0x3b: 0o53,
    0x3c: 0o54, 0x3d: 0o55, 0x3e: 0o56, 0x3f: 0o57, 0x40: 0o60, 0x41: 0o61,
    0x42: 0o62, 0x43: 0o63, 0x44: 0o64, 0x45: 0o65, 0x46: 0o66, 0x47: 0o67,
    0x48: 0o70, 0x49: 0o71, 0x4a: 0o135, 0x4b: 0o72, 0x4c: 0o73, 0x4d: 0o74,
    0x4e: 0o75, 0x4f: 0o76,
}
_MISC = {  # the handful of BMP symbols dispak maps; rare in names, kept for parity
    0x2015: 0o131, 0x2018: 0o32, 0x2019: 0o33, 0x2032: 0o137, 0x203e: 0o115,
    0x212f: 0o20, 0x2191: 0o21, 0x2227: 0o121, 0x2228: 0o120, 0x2260: 0o34,
    0x2261: 0o125, 0x2264: 0o116, 0x2265: 0o117, 0x2283: 0o122, 0x23e8: 0o20,
    0x25c7: 0o127, 0x25ca: 0o127,
}


def unicode_to_gost(u):
    """Return the GOST-10859 code for a Unicode code point, or None."""
    if u <= 0xff:
        g = UNI_TAB0[u]
    elif 0x0410 <= u <= 0x044f:
        g = _CYR[u - 0x0400]
    else:
        g = _MISC.get(u)
    if g is None:
        return None
    return g


def gost_to_unicode(ch):
    return GOST_TO_UNICODE[ch & 0xff]


# ---------------------------------------------------------------------------
# Homoglyphs: GOST letter codes whose Cyrillic and Latin glyphs coincide.
# Values are the *Latin* Unicode; the Cyrillic form is GOST_TO_UNICODE[code].
# (Mirrors gost_to_unicode_lat vs _cyr in dispak/encoding.c.)
# ---------------------------------------------------------------------------
HOMOGLYPH_LAT = {
    0o40: 0x41,  0o42: 0x42,  0o45: 0x45,  0o52: 0x4b,  0o54: 0x4d,
    0o55: 0x48,  0o56: 0x4f,  0o60: 0x50,  0o61: 0x43,  0o62: 0x54,
    0o63: 0x59,  0o65: 0x58,
}


def gost_to_unicode2(ch, latin):
    """gost_to_unicode with an explicit Latin/Cyrillic choice."""
    if latin and ch in HOMOGLYPH_LAT:
        return HOMOGLYPH_LAT[ch]
    return GOST_TO_UNICODE[ch & 0xff]


def is_gost_letter(g):
    return 0o40 <= g <= 0o114


def is_homoglyph(g):
    return g in HOMOGLYPH_LAT


def render_component(comp, amb_latin):
    """Render one GOST component (bytes) to a Unicode string.

    Ambiguous components (only homoglyphs, or both a uniquely-Cyrillic and a
    uniquely-Latin letter) take homoglyphs from amb_latin; deterministic ones
    from their uniquely-scripted letters.
    """
    has_l = has_c = False
    for g in comp:
        if not is_gost_letter(g) or is_homoglyph(g):
            continue
        if GOST_TO_UNICODE[g] < 0x400:
            has_l = True
        else:
            has_c = True
    if (has_l and has_c) or (not has_l and not has_c):
        hscript = amb_latin
    else:
        hscript = has_l
    return ''.join(chr(gost_to_unicode2(g, hscript if is_homoglyph(g) else 0))
                   for g in comp)


def render_display(gname):
    """Dot-separated display form (ambiguous homoglyphs default to Cyrillic)."""
    n = gname.find(GOST_EOF)
    body = gname[:n] if n >= 0 else gname
    return '.'.join(render_component(c, 0) for c in body.split(bytes([GOST_DOT])))


# ---------------------------------------------------------------------------
# Name / шифр conversion
# ---------------------------------------------------------------------------
def is_alnum_gost(g):
    """A GOST-10859 code that is a digit (0..9) or a letter (А..Я, D..Z)."""
    return 0 <= g <= 0o11 or 0o40 <= g <= 0o114


def name_to_gost(name):
    """UTF-8 область name (каталог levels separated by '.') -> canonical GOST path.

    Mirrors dispak's parse_name canonical form: GOST bytes, simple names
    separated by the dot (GOST_DOT), terminated by 0377, padded with 0377 to
    ARFA_PATHLEN.  '.' encodes naturally to GOST_DOT via the ASCII table.
    Only alphanumeric characters are allowed inside a name component.
    """
    out = bytearray()
    for uch in name:
        u = ord(uch)
        g = unicode_to_gost(u)
        if g is None:
            raise ValueError(
                "character %r (U+%04X) is not representable in GOST-10859"
                % (uch, u))
        if g != GOST_DOT and not is_alnum_gost(g):
            raise ValueError(
                "illegal character %r in область name; only letters, digits "
                "and '.' (каталог separator) are allowed" % uch)
        out.append(g)
    if not out or len(out) >= ARFA_PATHLEN:
        raise ValueError("bad or too-long область name")
    # No empty components, matching создание.
    comp = 0
    for b in out:
        if b == GOST_DOT:
            if comp == 0:
                raise ValueError("empty name component in %r" % name)
            comp = 0
        else:
            comp += 1
    if comp == 0:
        raise ValueError("empty final name component in %r" % name)
    # Each component, taken as typed and capitalized, must be one of that
    # component's valid renderings (all homoglyphs Latin, or all Cyrillic) —
    # i.e. no mixed / inconsistent homoglyphs within a component.
    for typed, gcomp in zip(name.split('.'), bytes(out).split(bytes([GOST_DOT]))):
        up = typed.upper()
        if up not in (render_component(gcomp, 0), render_component(gcomp, 1)):
            raise ValueError(
                "mixed homoglyphs in component %r; use one alphabet "
                "consistently within each каталог level" % typed)
    buf = bytearray([GOST_EOF]) * ARFA_PATHLEN
    buf[:len(out)] = out
    buf[len(out)] = GOST_EOF
    return bytes(buf)


def parse_shifr(s):
    """A шифр is 6 BCD-ish nibbles; digits 0..9, '.' (точка) -> 0o16."""
    if len(s) > 6:
        raise ValueError("шифр must be at most 6 characters: %r" % s)
    val = 0
    for c in s:
        if c.isdigit():
            nib = int(c)
        elif c == '.':
            nib = 0o16
        else:
            raise ValueError("bad character %r in шифр (use 0-9 or '.')" % c)
        val = (val << 4) | nib
    return val


def shifr_str(val):
    out = []
    for i in range(20, -1, -4):
        nib = (val >> i) & 0o17
        out.append('.' if nib == 0o16 else str(nib) if nib < 10 else '?')
    return ''.join(out)


def gost_name_str(name_bytes):
    """Render a stored GOST name for display (dot-separated, homoglyph-aware)."""
    return render_display(name_bytes)


def gost_components(name_bytes):
    """List of GOST component byte-strings (каталог levels)."""
    n = name_bytes.find(GOST_EOF)
    body = name_bytes[:n] if n >= 0 else name_bytes
    return body.split(bytes([GOST_DOT]))


def resolve_path(root, name_bytes):
    """Find an область's existing filesystem path, trying both homoglyph
    renderings per каталог level (mirrors arfaname.c's arfa_resolve). Returns
    the path or None."""
    path = root
    for gcomp in gost_components(name_bytes):
        cands = [render_component(gcomp, 0)]
        alt = render_component(gcomp, 1)
        if alt != cands[0]:
            cands.append(alt)
        for c in cands:
            trial = os.path.join(path, c)
            if os.path.exists(trial):
                path = trial
                break
        else:
            return None
    return path


# ---------------------------------------------------------------------------
# Catalog record (de)serialisation
# ---------------------------------------------------------------------------
def unpack_rec(raw):
    f = struct.unpack(REC_FMT, raw)
    return {
        'used': f[0], 'is_catalog': f[1], 'group': f[2], 'kind': f[3],
        'individual': f[4], 'pad': f[5], 'acl_rights': f[6],
        'id': f[7], 'owner': f[8], 'rpass': f[9], 'wpass': f[10], 'len': f[11],
        'acl': list(f[12:18]),
        'excl_pid': f[18], 'shared_cnt': f[19], 'orders': f[20],
        'name': f[21],
    }


def pack_rec(r):
    return struct.pack(
        REC_FMT,
        r['used'], r['is_catalog'], r['group'], r['kind'], r['individual'],
        r['pad'], r['acl_rights'], r['id'], r['owner'], r['rpass'], r['wpass'],
        r['len'], r['acl'][0], r['acl'][1], r['acl'][2], r['acl'][3],
        r['acl'][4], r['acl'][5], r['excl_pid'], r['shared_cnt'], r['orders'],
        r['name'])


def empty_rec():
    # A never-used slot is all-zero, matching dispak's memset-0 of a fresh
    # catalog (cat_lock); the name is filled with 0377 only once used.
    return {
        'used': 0, 'is_catalog': 0, 'group': 0, 'kind': 0, 'individual': 0,
        'pad': 0, 'acl_rights': 0, 'id': 0, 'owner': 0, 'rpass': 0, 'wpass': 0,
        'len': 0, 'acl': [0] * ARFA_MAXACL, 'excl_pid': 0, 'shared_cnt': 0,
        'orders': 0, 'name': bytes(ARFA_PATHLEN),
    }


class Catalog:
    """The <root>/.catalog index, opened under the same lock dispak uses."""

    def __init__(self, root):
        self.root = root
        self.path = os.path.join(root, '.catalog')
        self.fd = None
        self.magic = ARFA_MAGIC
        self.idseq = 1
        self.rec = [empty_rec() for _ in range(ARFA_MAXREC)]

    def __enter__(self):
        self.fd = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o644)
        fcntl.lockf(self.fd, fcntl.LOCK_EX)      # F_SETLKW / F_WRLCK, whole file
        data = os.read(self.fd, CAT_SIZE)
        if len(data) >= CAT_SIZE:
            magic, idseq = struct.unpack_from(CAT_HDR, data, 0)
            if magic == ARFA_MAGIC:
                self.magic, self.idseq = magic, idseq
                off = struct.calcsize(CAT_HDR)
                self.rec = [unpack_rec(data[off + i * REC_SIZE:
                                            off + (i + 1) * REC_SIZE])
                            for i in range(ARFA_MAXREC)]
        return self

    def flush(self):
        buf = bytearray(struct.pack(CAT_HDR, ARFA_MAGIC, self.idseq))
        for r in self.rec:
            buf += pack_rec(r)
        os.lseek(self.fd, 0, os.SEEK_SET)
        os.write(self.fd, bytes(buf))

    def __exit__(self, *exc):
        os.close(self.fd)      # releases the lock
        self.fd = None

    def find_by_name(self, name_bytes):
        for r in self.rec:
            if r['used'] and r['name'] == name_bytes:
                return r
        return None

    def free_slot(self):
        for r in self.rec:
            if not r['used']:
                return r
        return None


# ---------------------------------------------------------------------------
# Область data file (Physical/"new" zone structure), as dispak/disk.c writes it
# ---------------------------------------------------------------------------
def ticks_since_midnight():
    t = time.localtime()
    frac = int((time.time() % 1) / 0.02)     # 0.02 s ticks
    return ((t.tm_hour * 60 + t.tm_min) * 60 + t.tm_sec) * 50 + frac


def date_bcd():
    t = time.localtime()
    mday, mon, year = t.tm_mday, t.tm_mon, t.tm_year
    return ((mday // 10) << 13 | (mday % 10) << 9 | (mon // 10) << 8 |
            (mon % 10) << 4 | (year % 10))


def build_zone(file_zone, diskno, uid):
    """One zero-data zone_t, matching disk_writei2 with convol/check == NULL."""
    coarse = ticks_since_midnight() >> 15
    track = file_zone * 2                     # halfzones (MD29MB unset)
    date = date_bcd()
    mark = 2 << 48                            # служ.-слово format marker
    cw = [0] * 8
    cw[0] = (track << 36 | coarse << 27 | 0o30 << 21 | date << 6 | 1 << 3 | mark)
    cw[1] = (0o1370707 << 24 | diskno << 12 | mark)
    cw[2] = uid | mark
    cw[3] = 0 | mark                          # checksum of all-zero data == 0
    cw[4] = (cw[0] + (1 << 36)) & ((1 << 64) - 1)
    cw[5], cw[6], cw[7] = cw[1], cw[2], cw[3]
    data_word = 0x0001000000000000            # low 48 bits 0, data-conv bit set
    return struct.pack('<8Q', *cw) + struct.pack('<Q', data_word) * DATAWORDS


def write_region_file(path, length, owner):
    """Create a data-область image of `length` zones (like создание).

    Области are addressed physically: zone N is at zone N of the file, with
    no reserved ZONE_OFFSET at the front (unlike numerical volumes/drums).
    """
    uid = owner << 24                          # userid() == user.l<<24 | user.r
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    try:
        for z in range(length):
            os.write(fd, build_zone(z, 0, uid))
    finally:
        os.close(fd)


# ---------------------------------------------------------------------------
def default_root():
    home = os.environ.get('HOME') or '/tmp'
    return os.path.join(home, '.besm6', 'arfa')


def shifr_match(a, b):
    for i in range(0, 24, 4):
        da, db = (a >> i) & 0o17, (b >> i) & 0o17
        if da != db and da != 0o16 and db != 0o16:
            return False
    return True


def name_len(name_bytes):
    i = name_bytes.find(GOST_EOF)
    return len(name_bytes) if i < 0 else i


def has_children(cat, gname):
    """True if any used область is nested directly or deeper under gname."""
    cut = name_len(gname)
    prefix = gname[:cut]
    for s in cat.rec:
        if not s['used']:
            continue
        nm = s['name']
        if (name_len(nm) > cut and nm[:cut] == prefix and nm[cut] == GOST_DOT):
            return True
    return False


def pid_alive(pid):
    if not pid:
        return False
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def do_list(root):
    cat = Catalog(root)
    with cat:
        print("Archive: %s   (idseq=%d)" % (root, cat.idseq))
        print("%-4s %-8s %-6s %5s  %s" % ("id", "owner", "kind", "zones", "name"))
        n = 0
        for r in cat.rec:
            if not r['used']:
                continue
            n += 1
            kind = "каталог" if r['is_catalog'] else "обл."
            print("%-4d %-8s %-6s %5d  %s" % (
                r['id'], shifr_str(r['owner']), kind, r['len'],
                gost_name_str(r['name'])))
        if not n:
            print("(empty)")


def do_delete(args):
    root = args.arfa_dir or default_root()
    gname = name_to_gost(args.name)

    cat = Catalog(root)
    with cat:
        r = cat.find_by_name(gname)
        if r is None:
            sys.exit("no such область: %r" % args.name)
        fpath = resolve_path(root, gname)
        if r['is_catalog'] and has_children(cat, gname) and not args.force:
            sys.exit("каталог %r has sub-области; delete them first (or --force)"
                     % args.name)
        if not args.force:
            if pid_alive(r['excl_pid']):
                sys.exit("область захвачена by pid %d; use --force"
                         % r['excl_pid'])
            if r['shared_cnt'] > 0:
                sys.exit("область has %d shared захват(s); use --force"
                         % r['shared_cnt'])
        try:
            if fpath is None:
                pass                    # file already gone
            elif r['is_catalog']:
                os.rmdir(fpath)
            else:
                os.unlink(fpath)
        except FileNotFoundError:
            pass
        except OSError as e:
            sys.exit("cannot remove %s: %s" % (fpath, e))
        # Free the slot, matching dispak's уничтожение (used=0, name retained).
        r['used'] = 0
        cat.flush()
    print("Deleted %s %s"
          % ("каталог" if r['is_catalog'] else "область", gost_name_str(gname)))


def do_create(args):
    root = args.arfa_dir or default_root()
    os.makedirs(root, exist_ok=True)

    owner = parse_shifr(args.owner)
    gname = name_to_gost(args.name)

    is_catalog = args.catalog or args.length == 0
    length = 0 if is_catalog else args.length
    if length > ARFA_MAXZONES:
        sys.exit("length %d exceeds maximum %d zones" % (length, ARFA_MAXZONES))
    if not is_catalog and length <= 0:
        sys.exit("specify --length (>0) or --catalog")

    cat = Catalog(root)
    with cat:
        existing = cat.find_by_name(gname)
        if existing and not args.force:
            sys.exit("область %r already exists (id %d); use --force to replace"
                     % (args.name, existing['id']))

        # A composite name requires an existing, owned область-каталог parent.
        if GOST_DOT in gname[:gname.index(GOST_EOF)]:
            cut = gname.index(GOST_EOF)
            i = gname[:cut].rindex(GOST_DOT)
            parent = bytearray([GOST_EOF]) * ARFA_PATHLEN
            parent[:i] = gname[:i]
            prec = cat.find_by_name(bytes(parent))
            if not prec or not prec['is_catalog']:
                sys.exit("parent каталог %r does not exist"
                         % gost_name_str(bytes(parent)))
            if not shifr_match(prec['owner'], owner) and not args.force:
                sys.exit("parent каталог is owned by %s, not %s"
                         % (shifr_str(prec['owner']), shifr_str(owner)))

        # Path: leaf as typed (capitalized), under the parent's actual dir.
        typed_leaf = args.name.split('.')[-1].upper()
        cut = gname.index(GOST_EOF)
        if GOST_DOT in gname[:cut]:
            i = gname[:cut].rindex(GOST_DOT)
            parent_path = resolve_path(root, gname[:i])
            if parent_path is None:
                sys.exit("parent каталог directory not found under %s" % root)
            fpath = os.path.join(parent_path, typed_leaf)
        else:
            fpath = os.path.join(root, typed_leaf)

        r = existing or cat.free_slot()
        if r is None:
            sys.exit("no free slot in catalog (%d областей max)" % ARFA_MAXREC)

        # Create the file-system object.
        try:
            if is_catalog:
                os.makedirs(fpath, exist_ok=True)
            else:
                os.makedirs(os.path.dirname(fpath), exist_ok=True)
                write_region_file(fpath, length, owner)
        except OSError as e:
            sys.exit("cannot create %s: %s" % (fpath, e))

        rid = r['id'] if existing else cat.idseq
        if not existing:
            cat.idseq += 1
        r.update(empty_rec())
        r['used'] = 1
        r['is_catalog'] = 1 if is_catalog else 0
        r['group'] = args.group & 3
        r['kind'] = args.kind & 7
        r['individual'] = 1 if args.individual else 0
        r['id'] = rid
        r['owner'] = owner
        r['len'] = length
        r['name'] = gname
        cat.flush()

    kind = "каталог" if is_catalog else "область (%d зон)" % length
    print("Created %s  id=%d  owner=%s  ->  %s"
          % (kind, rid, shifr_str(owner), fpath))


def main():
    ap = argparse.ArgumentParser(
        description="Create АРФА области from the Unix command line.")
    ap.add_argument('name', nargs='?',
                    help="область name (UTF-8; '.' separates каталог levels)")
    ap.add_argument('--arfa-dir', metavar='DIR',
                    help="archive root (default ~/.besm6/arfa)")
    ap.add_argument('--owner', default='000000', metavar='SHIFR',
                    help="owner шифр, up to 6 of [0-9.] (default 000000)")
    ap.add_argument('--length', type=int, default=0, metavar='N',
                    help="length in zones for a data область (1..%d)"
                         % ARFA_MAXZONES)
    ap.add_argument('--catalog', action='store_true',
                    help="create an область-каталог (a directory) instead")
    ap.add_argument('--group', type=int, default=0, help="группа (0..3)")
    ap.add_argument('--kind', type=int, default=0, help="вид (0..7)")
    ap.add_argument('--individual', action='store_true',
                    help="mark as индивидуальная область")
    ap.add_argument('--force', action='store_true',
                    help="replace/relax checks (create) or force removal (delete)")
    ap.add_argument('--delete', '--destroy', action='store_true', dest='delete',
                    help="delete the named область/каталог instead of creating")
    ap.add_argument('--list', action='store_true',
                    help="list the archive instead of creating")
    args = ap.parse_args()

    root = args.arfa_dir or default_root()
    try:
        if args.list:
            if not os.path.exists(os.path.join(root, '.catalog')):
                sys.exit("no archive at %s" % root)
            do_list(root)
            return
        if args.delete:
            if not args.name:
                ap.error("a область name is required for --delete")
            if not os.path.exists(os.path.join(root, '.catalog')):
                sys.exit("no archive at %s" % root)
            do_delete(args)
            return
        if not args.name:
            ap.error("a область name is required (or use --list)")
        do_create(args)
    except ValueError as e:
        sys.exit("mkarfa: %s" % e)


if __name__ == '__main__':
    main()
