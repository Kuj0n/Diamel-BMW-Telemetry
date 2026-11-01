"""
Odczyt parametrów z kodu zadań diagnostycznych (BEST2) - dla sterowników, które nie mają
tabeli SG_FUNKTIONEN (np. LMV_01, xDrive). Tylko zadania wysyłające stałe zapytanie "22 xx xx"
(odczyt DID) - nic, co zapisuje lub steruje.

Zadanie jest wykonywane symbolicznie (liniowo, bez skoków): śledzimy, z których bajtów odpowiedzi
powstaje każdy wynik i jakie działania są na nim wykonane, a potem zamieniamy to na wiersz katalogu
(pozycja, typ, maska, mnożnik, przesunięcie).
"""
import re
import struct

XOR = bytes(b ^ 0xF7 for b in range(256))

# Tryb adresowania -> rozmiar operandu (None = tekst z długością)
_OPERAND_SIZE = {0: 0, 1: 1, 2: 1, 3: 1, 4: 1, 5: 1, 6: 2, 7: 4, 8: None, 9: 3, 10: 2, 11: 4, 12: 5, 13: 4, 14: 4, 15: 3}
_OPS = {
    0x00: "move", 0x01: "clear", 0x02: "comp", 0x03: "subb", 0x04: "adds", 0x05: "mult", 0x06: "divs",
    0x07: "and", 0x08: "or", 0x09: "xor", 0x0A: "not", 0x0B: "jump", 0x0D: "ret", 0x10: "jz", 0x11: "jnz",
    0x15: "jpl", 0x18: "asr", 0x19: "lsl", 0x1A: "lsr", 0x1B: "asl", 0x1C: "nop", 0x1D: "eoj", 0x1E: "push",
    0x1F: "pop", 0x2A: "xsend", 0x34: "ergb", 0x35: "ergw", 0x36: "ergd", 0x37: "ergi", 0x38: "ergr",
    0x39: "ergs", 0x3A: "a2flt", 0x3B: "fadd", 0x3C: "fsub", 0x3D: "fmul", 0x3E: "fdiv", 0x50: "atsp",
    0x68: "fix2flt", 0x7B: "tabset", 0x82: "ergl",
}
_REGS = {**{i: f"B{i:X}" for i in range(0x10)}, **{0x10 + i: f"A{i:X}" for i in range(0x10)},
         **{0x20 + i: f"I{i:X}" for i in range(0x10)}, **{0x30 + i: f"L{i:X}" for i in range(0x10)},
         **{0x80 + i: f"S{i:X}" for i in range(0x20)}}


def _reg(b):
    return _REGS.get(b, f"R{b:02X}")


def _operand(code, p, mode):
    if mode == 0:
        return "", p
    if mode in (1, 2, 3, 4):
        return _reg(code[p]), p + 1
    if mode == 5:
        return f"#{code[p]:#x}", p + 1
    if mode == 6:
        return f"#{struct.unpack_from('<H', code, p)[0]:#x}", p + 2
    if mode == 7:
        return f"#{struct.unpack_from('<I', code, p)[0]:#x}", p + 4
    if mode == 8:
        n = struct.unpack_from("<H", code, p)[0]
        d = code[p + 2:p + 2 + n].rstrip(b"\0")
        txt = d.decode("cp1252") if d and all(32 <= c < 127 for c in d) else d.hex(" ")
        return f'"{txt}"', p + 2 + n
    if mode == 9:
        return f"{_reg(code[p])}[#{struct.unpack_from('<H', code, p + 1)[0]}]", p + 3
    if mode == 10:
        return f"{_reg(code[p])}[{_reg(code[p + 1])}]", p + 2
    sizes = {11: 4, 12: 5, 13: 4, 14: 4, 15: 3}
    return f"{_reg(code[p])}[?]", p + sizes[mode]


def read_jobs(raw: bytes):
    """[(nazwa zadania, początek kodu)] i odszyfrowany plik."""
    t = raw.translate(XOR)
    ptr = struct.unpack_from("<I", raw, 0x88)[0]
    n = struct.unpack_from("<I", raw, ptr)[0]
    jobs = []
    for i in range(n):
        e = ptr + 4 + i * 0x44
        jobs.append((t[e:e + 0x40].split(b"\0")[0].decode("cp1252"), struct.unpack_from("<I", t, e + 0x40)[0]))
    return t, jobs


def disasm(t, start, end):
    p, out = start, []
    while p < end - 1:
        op, modes = t[p], t[p + 1]
        p += 2
        a, p = _operand(t, p, modes >> 4)
        b, p = _operand(t, p, modes & 0xF)
        out.append((_OPS.get(op, f"op{op:02X}"), a, b))
    return out


# --- Wyrażenia ---
class E:
    def __init__(self, op, *a):
        self.op, self.a = op, a

    def __repr__(self):
        if self.op in ("c", "byte", "str", "sym"):
            return {"c": str, "byte": lambda v: f"R[{v}]", "str": lambda v: f'"{v}"', "sym": str}[self.op](self.a[0])
        if self.op in ("flt", "s16"):
            return f"{self.op}({self.a[0]})"
        return f"({self.a[0]} {self.op} {self.a[1]})"


def C(v):
    return E("c", v)


def _fold(e):
    if not isinstance(e, E) or e.op in ("c", "byte", "str", "sym"):
        return e
    if e.op in ("flt", "s16"):
        return E(e.op, _fold(e.a[0]))
    x, y = _fold(e.a[0]), _fold(e.a[1])
    if x.op == "c" and y.op == "c":
        p, q = x.a[0], y.a[0]
        r = {"+": lambda: p + q, "-": lambda: p - q, "*": lambda: p * q, "/": lambda: p / q if q else 0,
             "<<": lambda: int(p) << int(q), ">>": lambda: int(p) >> int(q),
             "&": lambda: int(p) & int(q), "|": lambda: int(p) | int(q)}[e.op]()
        return C(r)
    return E(e.op, x, y)


def run_job(t, start, end):
    """Symboliczne wykonanie zadania: (zapytanie, [(wynik, rodzaj, wyrażenie, tabela tekstów)])."""
    R, mem, st, res = {}, {}, [], []
    tab, request = None, None

    def load(n, w):
        first = mem.get(n)
        if not first:
            return E("sym", "?")
        e, k = first
        if k == 0 and all(mem.get(n + i) and mem[n + i][0] is e and mem[n + i][1] == i for i in range(w)):
            return e
        return E("&", e, C((1 << (8 * w)) - 1)) if k == 0 else E("sym", "?")

    def val(x):
        if x.startswith("#"):
            return C(int(x[1:], 16))
        if x.startswith('"'):
            return E("str", x.strip('"'))
        m = re.fullmatch(r"(\w+)\[(#?\w+)\]", x)
        if m:
            base = R.get(m.group(1))
            idx = val(m.group(2)) if m.group(2).startswith("#") else R.get(m.group(2))
            idx = _fold(idx) if idx is not None else None
            if base is not None and base.op == "sym" and base.a[0] == "RESP" and idx is not None and idx.op == "c":
                return E("byte", idx.a[0])
            return E("sym", "?")
        if x in ("B0", "A0"):
            return R.get("A8", C(0))
        return R.get(x, E("sym", x))

    for o, a, b in disasm(t, start, end):
        if o == "clear":
            R[a] = E("str", "") if a == "AD" else C(0)
        elif o == "move":
            if a.startswith("AC["):
                if re.fullmatch(r"B[0-3]", b):
                    mem[int(re.search(r"#(\w+)", a).group(1))] = (R.get("A8", C(0)), int(b[1]))
            elif a in ("B0", "A0", "A8") and b.startswith("AC["):
                R["A8"] = load(int(re.search(r"#(\w+)", b).group(1)), {"B0": 1, "A0": 2, "A8": 4}[a])
            elif a in ("B0", "A0"):
                R["A8"] = val(b)
            elif a == "A1" and b == "#0xffff":
                R["A8"] = E("s16", R.get("A8", C(0)))  # rozszerzenie znaku po "jpl"
            elif re.fullmatch(r"B[1-9A-F]|A[1-7]", a):
                pass
            else:
                v = val(b)
                R[a] = v
                if v.op == "str" and request is None and re.fullmatch(r"22( [0-9a-fA-F]{2}){2}", v.a[0]):
                    request = v.a[0].upper()
        elif o == "push":
            st.append(val(a))
        elif o == "pop":
            R[a] = st.pop() if st else E("sym", "?")
        elif o == "atsp":
            R[a] = st[-1] if st else E("sym", "?")
        elif o in ("adds", "subb", "mult", "divs", "and", "or", "asl", "asr", "lsl", "lsr"):
            sym = {"adds": "+", "subb": "-", "mult": "*", "divs": "/", "and": "&", "or": "|",
                   "asl": "<<", "lsl": "<<", "asr": ">>", "lsr": ">>"}[o]
            R[a] = _fold(E(sym, val(a), val(b)))
        elif o in ("fadd", "fsub", "fmul", "fdiv"):
            R[a] = _fold(E({"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/"}[o], val(a), val(b)))
        elif o == "fix2flt":
            R[a] = E("flt", val(b))
        elif o == "a2flt":
            s = val(b)
            try:
                R[a] = C(float(s.a[0].replace(",", "."))) if s.op == "str" else E("sym", "?")
            except ValueError:
                R[a] = E("sym", "?")
        elif o == "xsend":
            R[a] = E("sym", "RESP")
        elif o == "tabset":
            tab = a.strip('"')
        elif o in ("ergl", "ergi", "ergr", "ergd", "ergw", "ergb"):
            res.append((a.strip('"'), o, _fold(val(b)), None))
        elif o == "ergs":
            res.append((a.strip('"'), o, val(b), tab))
    return request, res


# --- Wyrażenie -> pole odpowiedzi ---
def _field(e):
    """(pozycja bajtu w odpowiedzi, rozmiar, ze znakiem, maska) albo None."""
    if e.op == "s16":
        f = _field(e.a[0])
        if f and f[1] == 2:
            return (f[0], 2, True, f[3])
        if e.a[0].op == "+" and e.a[0].a[0].op == "s16":  # s16(s16(R[n]<<8) + R[n+1])
            inner = E("+", e.a[0].a[0].a[0], e.a[0].a[1])
            f = _field(inner)
            return (f[0], 2, True, f[3]) if f and f[1] == 2 else None
        return None
    if e.op == ">>" and e.a[1].op == "c":
        k = int(e.a[1].a[0])
        inner = e.a[0]
        if inner.op == "&" and inner.a[1].op == "c":
            f = _field(inner.a[0])
            m = int(inner.a[1].a[0])
            return (f[0], f[1], f[2], m) if f and m and (m & -m) == (1 << k) else None
        f = _field(inner)
        return (f[0], f[1], f[2], ((1 << (8 * f[1])) - 1) & ~((1 << k) - 1)) if f else None
    if e.op == "&" and e.a[1].op == "c":
        f = _field(e.a[0])
        return (f[0], f[1], f[2], int(e.a[1].a[0])) if f else None
    terms = []

    def collect(x):
        if x.op == "+":
            return collect(x.a[0]) and collect(x.a[1])
        if x.op == "byte":
            terms.append((x.a[0], 0))
            return True
        if x.op == "<<" and x.a[0].op == "byte" and x.a[1].op == "c":
            terms.append((x.a[0].a[0], int(x.a[1].a[0])))
            return True
        return False

    if not collect(e) or not terms:
        return None
    terms.sort(key=lambda t: -t[1])
    n = len(terms)
    first = terms[0][0]
    if all(terms[i] == (first + i, 8 * (n - 1 - i)) for i in range(n)) and n in (1, 2, 4):
        return (first, n, False, 0)
    return None


def to_row(e):
    """Wyrażenie wyniku -> (pole, mnożnik, przesunięcie) albo None, gdy wzór jest złożony."""
    # wartość = pole * mnożnik + przesunięcie
    offset = 0.0
    if e.op in ("+", "-") and e.a[1].op == "c":
        offset = e.a[1].a[0] * (1 if e.op == "+" else -1)
        e = e.a[0]
    factor = _scale_of(e)
    fld = _field(_strip_scale(e))
    return (fld, factor, offset) if fld else None


def _scale_of(e):
    """Mnożnik z mnożenia/dzielenia przez stałe."""
    f = 1.0
    while True:
        if e.op == "flt":
            e = e.a[0]
        elif e.op == "*" and e.a[1].op == "c":
            f *= e.a[1].a[0]
            e = e.a[0]
        elif e.op == "/" and e.a[1].op == "c" and e.a[1].a[0]:
            f /= e.a[1].a[0]
            e = e.a[0]
        elif e.op == "/" and e.a[1].op == "flt" and e.a[1].a[0].op == "c" and e.a[1].a[0].a[0]:
            f /= e.a[1].a[0].a[0]
            e = e.a[0]
        else:
            return f


def _strip_scale(e):
    while True:
        if e.op == "flt":
            e = e.a[0]
        elif e.op in ("*", "/") and (e.a[1].op == "c" or (e.a[1].op == "flt" and e.a[1].a[0].op == "c")):
            e = e.a[0]
        else:
            return e


def result_comments(plain: bytes, job: str):
    i = plain.find(b"JOBNAME:" + job.encode() + b"\n")
    if i < 0:
        return {}
    e = plain.find(b"JOBNAME:", i + 10)
    out, cur = {}, None
    for line in plain[i:e if e > 0 else len(plain)].decode("cp1252", "replace").split("\n"):
        if line.startswith("RESULT:"):
            cur = line[7:].strip()
        elif line.startswith("RESULTCOMMENT:") and cur and cur not in out:
            out[cur] = line[14:].strip()
    return out


def extract(raw: bytes):
    """Parametry z zadań odczytu: lista słowników (did, wynik, pole, mnożnik, przesunięcie, jednostka,
    tabela tekstów, opis) oraz lista pominiętych wyników ze złożonym wzorem."""
    t, jobs = read_jobs(raw)
    starts = sorted(s for _, s in jobs)
    params, skipped = [], []
    for name, start in jobs:
        if not name.startswith("STATUS_"):
            continue
        end = min([s for s in starts if s > start] or [len(t)])
        request, results = run_job(t, start, end)
        if not request:
            continue  # brak stałego zapytania 22 xx xx - pomijamy (nie tylko odczyt albo dynamiczne)
        did = request.replace(" ", "")[2:]
        comments = result_comments(t, name)
        units = {r[0][:-5]: r[2].a[0] for r in results if r[0].endswith("_EINH") and r[2].op == "str"}
        texts = {r[0][:-5]: r[3] for r in results if r[0].endswith("_TEXT") and r[3]}
        for rname, kind, expr, _ in results:
            if not rname.startswith("STAT_") or not rname.endswith("_WERT") or kind == "ergs":
                continue
            row = to_row(expr)
            if not row:
                skipped.append(f"{name}.{rname}")
                continue
            (pos, size, signed, mask), factor, offset = row
            base = rname[:-5]
            unit = units.get(base, "")
            params.append({
                "job": name, "did": did, "result": rname, "pos": pos - 3, "size": size, "signed": signed,
                "mask": mask, "factor": factor, "offset": offset,
                "unit": "" if unit in ("00", "-") else unit,
                "texts": texts.get(base, ""), "comment": comments.get(rname, ""),
            })
    return params, skipped
