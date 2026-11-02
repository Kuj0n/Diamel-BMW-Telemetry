"""
Ekstrakcja parametrów (UDS 0x22, tylko odczyt) i kodów błędów dla sterowników konkretnego auta.

Użycie (z katalogu projektu):
    python tools/wyciagnij_f31.py <folder_z_plikami_prg> <folder_wyjsciowy>
    np.  python tools/wyciagnij_f31.py docs/private/prg docs/private/diamel_f31

Pliki .prg (opisy sterowników BMW dla EDIABAS) NIE są częścią projektu - każdy używa własnych.
Lista sterowników (CAR_ECUS) to sterowniki konkretnego auta (BMW F31 B47 xDrive), odczytane
narzędziem do identyfikacji pojazdu (np. BimmerDeepOBD) - dla innego auta trzeba ją zmienić.
Adresy diagnostyczne czytane z tabeli ZUORDNUNGSTABELLEUDS w T_GRTB.PRG.

Wynik:
    moduly.csv     - adres, plik SGBD, nazwa modułu, liczba parametrów i błędów
    parametry.csv  - jeden wiersz = jedna wartość do odczytu i przeliczenia
    teksty.csv     - opisy wartości liczbowych (np. 3 -> "3. Gang")
    bledy_XX.csv   - kody błędów (F) i wpisów informacyjnych (I) modułu o adresie XX

Przeliczenie wartości w urządzeniu:
    surowa = bajty[pozycja : pozycja+rozmiar(typ)] w kolejności BE/LE, ze znakiem dla S*
    jeśli maska != 0: surowa = (surowa & maska) >> (liczba zer na końcu maski)
    wartość = surowa * mnoznik + przesuniecie
Typy: U8 S8 U16 S16 U32 S32 F32 F64, STRn (tekst n bajtów), HEXn (bajty n), n=0 -> do końca odpowiedzi.
"""
import datetime
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import best_dekoder  # noqa: E402  odczyty zaszyte w kodzie zadań (np. xDrive LMV_01)

HEADER_MAGIC = b"@EDIABAS OBJECT"
XOR_KEY = 0xF7

# Wersja formatu katalogu (urządzenie pokazuje ją w Info) i opis auta w nagłówku plików
CATALOG_VERSION = 2
VEHICLE = "BMW F31 B47 xDrive"

# Standardowe OBD-II (tryb 01, SAE J1979) - działa w każdym aucie z silnikiem spalinowym od ok. 2004 r.
# W F31 sterownik silnika nie podaje pomiarów przez numery BMW (odmowa 0x31), za to odpowiada na OBD-II.
# Moduł wirtualny o adresie EF (urządzenie pyta wtedy 0x7DF / 0x7E8). Wiersz: PID, nazwa, typ, pozycja
# (od bajtu A), mnożnik, przesunięcie, jednostka, miejsca po przecinku, czy lista podstawowa.
OBD_ADDR = "EF"
OBD_PIDS = [
    ("0C", "Obroty silnika", "U16", 0, 0.25, 0, "rpm", 0, 1),
    ("05", "Temperatura plynu chlodzacego", "U8", 0, 1, -40, "°C", 0, 1),
    ("0F", "Temperatura powietrza dolotowego", "U8", 0, 1, -40, "°C", 0, 1),
    ("46", "Temperatura otoczenia", "U8", 0, 1, -40, "°C", 0, 1),
    ("5C", "Temperatura oleju", "U8", 0, 1, -40, "°C", 0, 1),
    ("0B", "Cisnienie w kolektorze dolotowym", "U8", 0, 1, 0, "kPa", 0, 1),
    ("33", "Cisnienie atmosferyczne", "U8", 0, 1, 0, "kPa", 0, 0),
    ("04", "Obciazenie silnika", "U8", 0, 0.392157, 0, "%", 0, 1),
    ("62", "Moment obrotowy rzeczywisty", "U8", 0, 1, -125, "%", 0, 1),
    ("61", "Moment obrotowy zadany przez kierowce", "U8", 0, 1, -125, "%", 0, 0),
    ("10", "Przeplyw powietrza (MAF)", "U16", 0, 0.01, 0, "g/s", 1, 1),
    ("23", "Cisnienie paliwa w szynie", "U16", 0, 0.1, 0, "bar", 0, 1),
    ("5E", "Zuzycie paliwa", "U16", 0, 0.05, 0, "l/h", 1, 1),
    ("2C", "EGR zadane", "U8", 0, 0.392157, 0, "%", 0, 0),
    ("0D", "Predkosc pojazdu", "U8", 0, 1, 0, "km/h", 0, 1),
    ("2F", "Poziom paliwa", "U8", 0, 0.392157, 0, "%", 0, 1),
    ("42", "Napiecie modulu sterujacego", "U16", 0, 0.001, 0, "V", 2, 1),
    ("1F", "Czas pracy od uruchomienia", "U16", 0, 1, 0, "s", 0, 0),
    ("31", "Przebieg od skasowania bledow", "U16", 0, 1, 0, "km", 0, 0),
    ("21", "Przebieg z kontrolka silnika", "U16", 0, 1, 0, "km", 0, 0),
]


def obd_rows():
    rows = []
    for pid, name, typ, pos, mul, add, unit, dec, basic in OBD_PIDS:
        rows.append([OBD_ADDR, f"obd_{pid.lower()}", name, f"00{pid}", typ, str(pos), "0", "BE", fmt_num(mul),
                     fmt_num(add), unit, str(dec), "", f"OBD-II PID {pid}", str(basic)])
    return rows


# Sterowniki wykryte w aucie: SGBD -> polska nazwa modułu
CAR_ECUS = {
    "AAG_F15": "Modul haka holowniczego",
    "ACSM4": "Poduszki powietrzne",
    "FEM_20": "Front Electronic Module",
    "D70BX7A0": "Silnik (DDE B47)",
    "DSC_25": "DSC (ABS / kontrola trakcji)",
    "ATM": "Modul telematyczny (eCall)",
    "GKEB23TU": "Skrzynia biegow (ZF 8HP)",
    "EPS_TK02": "Wspomaganie kierownicy",
    "REM_20": "Rear Electronic Module",
    "FRR_10": "Aktywny tempomat (radar)",
    "FZD_20": "Centrum funkcji dachu",
    "GWS2": "Dzwignia zmiany biegow",
    "NBTEVO": "Radio / nawigacja",
    "HKFM_G11": "Modul tylnej klapy",
    "ICM_25": "Zintegrowane zarzadzanie podwoziem",
    "IHX_I1": "Klimatyzacja",
    "KAFAS20": "Kamera asystentow",
    "KOMB01": "Zestaw wskaznikow",
    "FLE02_L": "Reflektor lewy",
    "FLE02_R": "Reflektor prawy",
    "LMV_01": "Skrzynia rozdzielcza xDrive",
    "SCR01": "Uklad SCR (AdBlue)",
    "TBX5_01": "Touchbox",
    "TRSVC01": "Kamery widoku dookolnego",
    "ZBE6": "Kontroler iDrive",
    "ZGW_01": "Gateway (ZGW)",
}

# Polskie nazwy (i ewentualnie przeliczenie jednostki) dla najważniejszych wartości.
# RESULTNAME -> (nazwa, jednostka lub None = bez zmian, dodatkowy mnożnik)
TRANSLATIONS = {
    "STAT_MOTORDREHZAHL_WERT": ("Obroty silnika", "rpm", 1),
    "STAT_LADEDRUCK_WERT": ("Cisnienie doladowania", "bar", 0.001),
    "STAT_LADEDRUCK_SOLL_WERT": ("Cisnienie doladowania zadane", "bar", 0.001),
    "STAT_KUEHLMITTELTEMPERATUR_WERT": ("Temperatura plynu chlodzacego", "°C", 1),
    "STAT_OELTEMPERATUR_WERT": ("Temperatura oleju", "°C", 1),
    "STAT_LUFTMASSE_WERT": ("Masa powietrza", None, 1),
    "STAT_RAILDRUCK_WERT": ("Cisnienie na listwie (rail)", "bar", 1),
    "STAT_RAILDRUCK_SOLL_WERT": ("Cisnienie na listwie zadane", "bar", 1),
    "STAT_GESCHWINDIGKEIT_WERT": ("Predkosc pojazdu", "km/h", 1),
    "STAT_BATTERIESPANNUNG_WERT": ("Napiecie akumulatora", "V", 1),
    "STAT_UBATT_WERT": ("Napiecie zasilania", "V", 1),
    "STAT_UMGEBUNGSTEMPERATUR_WERT": ("Temperatura zewnetrzna", "°C", 1),
    "STAT_ANSAUGLUFTTEMPERATUR_WERT": ("Temperatura powietrza dolotowego", "°C", 1),
    "STAT_ABGASTEMPERATUR_VOR_PARTIKELFILTER_WERT": ("Temperatura spalin przed DPF", "°C", 1),
    "STAT_ABGASTEMPERATUR_VOR_KAT_WERT": ("Temperatura spalin przed katalizatorem", "°C", 1),
    "STAT_DIFFERENZDRUCK_PARTIKELFILTER_WERT": ("Roznica cisnien na DPF", None, 1),
    "STAT_FAHRPEDALWERT_WERT": ("Pozycja pedalu gazu", "%", 1),
    # xDrive (LMV_01) - z kodu zadań
    "STAT_AVL_REPAT_XTRQ_FTAX_BAX_WERT": ("xDrive moment na przod (rzeczywisty)", "Nm", 1),
    "STAT_TAR_REPAT_XTRQ_FTAX_BAX_ACT_WERT": ("xDrive moment na przod (zadany)", "Nm", 1),
    "STAT_PHISM_WERT": ("xDrive kat zapiecia sprzegla (rampa)", "°", 1),
    "STAT_ISM_WERT": ("xDrive prad silnika sprzegla", "A", 1),
    "STAT_PWM_WERT": ("xDrive sterowanie silnika sprzegla", "%", 1),
    "STAT_NSM_WERT": ("xDrive obroty silnika sprzegla", "rpm", 1),
    "STAT_UKL30_WERT": ("xDrive napiecie zasilania", "V", 1),
    "STAT_PHISPERREKAL_WERT": ("xDrive kat kalibracji sprzegla", "°", 1),
    "STAT_COUNT_KAL_WERT": ("xDrive liczba kalibracji", None, 1),
    "STAT_COUNT_FORCE_WERT": ("xDrive liczba kalibracji wymuszonych", None, 1),
    "STAT_HO_LAMELLE_1_WERT": ("xDrive zuzycie lamel 1", None, 1),
    "STAT_HO_LAMELLE_2_WERT": ("xDrive zuzycie lamel 2", None, 1),
    "STAT_HO_LAMELLE_3_WERT": ("xDrive zuzycie lamel 3", None, 1),
    "STAT_HO_GETRIEBE_3_WERT": ("xDrive zuzycie oleju", None, 1),
    "STAT_HO_INTEGRATOR_KM_WERT": ("xDrive km od wymiany oleju", "km", 1),
    "STAT_QU_SER_VV_KUPPLUNG_WERT": ("xDrive dostepnosc sprzegla", None, 1),
    "STAT_QU_SER_VV_STELLGLIED_WERT": ("xDrive dostepnosc silownika", None, 1),
    "STAT_QU_SER_NOTLAUF_WERT": ("xDrive tryb awaryjny", None, 1),
}

DATATYPES = {
    "unsigned char": ("U8", 1), "char": ("S8", 1), "signed char": ("S8", 1),
    "unsigned int": ("U16", 2), "unsigned integer": ("U16", 2),
    "int": ("S16", 2), "integer": ("S16", 2), "signed int": ("S16", 2), "signed integer": ("S16", 2),
    "unsigned long": ("U32", 4), "long": ("S32", 4), "signed long": ("S32", 4),
    "float": ("F32", 4), "motorola float": ("F32", 4), "intel float": ("F32", 4),
    "double": ("F64", 8), "motorola double": ("F64", 8), "intel double": ("F64", 8),
}

UNITS = {
    "grad c": "°C", "degc": "°C", "°c": "°C", "grad": "°", "1/min": "rpm", "u/min": "rpm", "upm": "rpm",
    "-": "", "": "", "%tastverhaeltnis": "%PWM", "%tastverhältnis": "%PWM",
}

TRANSLIT = str.maketrans({
    "ä": "ae", "ö": "oe", "ü": "ue", "ß": "ss", "Ä": "Ae", "Ö": "Oe", "Ü": "Ue",
    "é": "e", "è": "e", "á": "a", "à": "a", ";": ",", '"': "", "\t": " ", "\r": " ", "\n": " ",
})


def ascii_text(text: str, limit: int) -> str:
    s = (text or "").translate(TRANSLIT)
    s = "".join(ch if 32 <= ord(ch) <= 126 or ch == "°" else " " for ch in s)
    s = re.sub(r"\s+", " ", s).strip()
    return s[:limit].rstrip()


# --- Odczyt tabel z pliku .prg/.grp (na podstawie generate_diamel_sd.py) ---
def xor_bytes(data: bytes) -> bytes:
    return data.translate(bytes(b ^ XOR_KEY for b in range(256)))


def read_cstring(plain: bytes, offset: int, max_len: int = 4096):
    end = plain.find(b"\x00", offset, offset + max_len)
    if end == -1:
        end = min(len(plain), offset + max_len)
    return plain[offset:end].decode("cp1252", errors="replace").strip(), end + 1


def read_tables(path: Path) -> dict:
    raw = path.read_bytes()
    if len(raw) < 0xA0 or not raw.startswith(HEADER_MAGIC):
        return {}
    plain = xor_bytes(raw)
    size = len(raw)
    for off in range(0x70, 0x94, 4):
        ptr = struct.unpack_from("<I", raw, off)[0]
        if not (0x90 <= ptr < size - 80):
            continue
        for meta in (raw, plain):
            count = struct.unpack_from("<I", meta, ptr)[0]
            pos = ptr + 4
            if not (0 < count <= 3000) or pos + count * 80 > size:
                continue
            first = plain[pos:pos + 64].split(b"\x00", 1)[0]
            if not first or not all(32 <= b <= 126 for b in first):
                continue
            tables, ok = {}, True
            for _ in range(count):
                name = plain[pos:pos + 64].split(b"\x00", 1)[0].decode("cp1252", errors="replace").strip()
                p1, p2, p3, p4 = struct.unpack_from("<IIII", meta, pos + 64)
                pos += 80
                if 0 < p3 <= 300 and 0 <= p4 <= 200000 and 0x90 <= p1 < size:
                    start, end, ncols, nrows = p1, p2, p3, p4
                elif 0 < p1 <= 300 and 0 <= p2 <= 200000 and 0x90 <= p3 < size:
                    ncols, nrows, start, end = p1, p2, p3, p4
                else:
                    ok = False
                    break
                sp = start
                cols = []
                for _ in range(ncols):
                    c, sp = read_cstring(plain, sp)
                    cols.append(c.upper())
                rows = []
                for _ in range(nrows):
                    if sp >= size or (end > start and sp >= end):
                        break
                    vals = []
                    for _ in range(ncols):
                        v, sp = read_cstring(plain, sp)
                        vals.append(v)
                    rows.append(dict(zip(cols, vals)))
                tables[name.upper()] = rows
            if ok and tables:
                return tables
    return {}


# --- Pomocnicze ---
def num(text: str, default: float) -> float:
    try:
        t = (text or "").strip()
        if t in ("", "-"):
            return default
        return float(int(t, 16)) if t.lower().startswith("0x") else float(t)
    except ValueError:
        return default


def fmt_num(v: float) -> str:
    if v == int(v) and abs(v) < 1e15:
        return str(int(v))
    return f"{v:.10g}"


def decimals_for(factor: float, offset: float) -> int:
    for d in range(0, 4):
        if abs(factor * 10 ** d - round(factor * 10 ** d)) < 1e-9 and \
           abs(offset * 10 ** d - round(offset * 10 ** d)) < 1e-9:
            return d
    return 3


def parse_type(datentyp: str):
    """(kod typu, rozmiar w bajtach) albo None. Rozmiar 0 = do końca odpowiedzi."""
    d = (datentyp or "").strip().lower()
    if d in DATATYPES:
        return DATATYPES[d]
    m = re.fullmatch(r"(string|data)\s*(?:\[\s*(\d+)\s*\])?", d)
    if m:
        n = int(m.group(2)) if m.group(2) else 0
        return (("STR" if m.group(1) == "string" else "HEX") + str(n), n)
    return None


def byte_order(lh: str, datentyp: str) -> str:
    d = (datentyp or "").lower()
    if d.startswith("intel"):
        return "LE"
    if d.startswith("motorola"):
        return "BE"
    return "LE" if (lh or "").strip().lower() == "low" else "BE"


def unit_of(einheit: str) -> str:
    u = (einheit or "").strip()
    return UNITS.get(u.lower(), ascii_text(u, 10))


def is_text_table(rows) -> bool:
    return bool(rows) and "WERT" in rows[0] and "TEXT" in rows[0]


class Extractor:
    def __init__(self, adres: str, tables: dict):
        self.adres = adres
        self.tables = tables
        self.params = []
        self.text_tables = {}
        self.keys = set()
        self.skipped = 0

    def key_for(self, resultname: str, did: str) -> str:
        base = re.sub(r"^STAT_", "", resultname.upper()).lower()
        base = re.sub(r"[^a-z0-9_]", "_", base).strip("_")[:44] or f"did_{did.lower()}"
        key = base
        n = 2
        if key in self.keys:
            key = f"{base}_{did.lower()}"
        while key in self.keys:
            key = f"{base}_{did.lower()}_{n}"
            n += 1
        self.keys.add(key)
        return key

    def text_table_for(self, name: str) -> str:
        n = (name or "").strip()
        if n in ("", "-"):
            return ""
        rows = self.tables.get(n.upper())
        if not is_text_table(rows):
            return ""
        if n.upper() not in self.text_tables:
            self.text_tables[n.upper()] = [(r["WERT"], ascii_text(r["TEXT"], 60)) for r in rows]
        return n.upper()

    def add(self, did, row, typ, pos, mask, order):
        resultname = row.get("RESULTNAME", "-")
        info = row.get("INFO", "") or row.get("BESCHREIBUNG", "")
        factor = num(row.get("MUL"), 1.0) / (num(row.get("DIV"), 1.0) or 1.0)
        offset = num(row.get("ADD"), 0.0)
        unit = unit_of(row.get("EINHEIT"))
        if resultname.upper() in TRANSLATIONS:
            name, new_unit, scale = TRANSLATIONS[resultname.upper()]
            factor *= scale
            offset *= scale
            unit = new_unit or unit
        else:
            name = ascii_text(info.split(".")[0] if info and info not in ("-", "--") else resultname, 60)
        texts = self.text_table_for(row.get("NAME", ""))
        self.params.append([
            self.adres, self.key_for(resultname if resultname != "-" else f"did_{did}", did), name, did,
            typ, str(pos), f"0x{mask:X}" if mask else "0", order, fmt_num(factor), fmt_num(offset),
            unit, str(0 if typ.startswith(("STR", "HEX")) else decimals_for(factor, offset)),
            texts, ascii_text(info, 200),
        ])

    def bit_rows(self, did, rows, pos, order_hint):
        """Pola bitowe: wiersze z maską na tej samej pozycji. Zwraca rozmiar pola albo None."""
        size = 0
        for r in rows:
            t = parse_type(r.get("DATENTYP"))
            mask = int(num(r.get("MASKE"), 0))
            if not t or not mask or t[1] == 0:
                return None
            size = max(size, t[1])
            self.add(did, r, t[0], pos, mask, byte_order(r.get("L/H") or order_hint, r.get("DATENTYP")))
        return size

    def res_table(self, did, rows) -> bool:
        """Ramka z wieloma wartościami - pozycje liczone kolejno; pola z maską dzielą pozycję."""
        pos = 0
        group_type, group_mask = None, 0
        for r in rows:
            dt = (r.get("DATENTYP") or "").strip()
            mask = int(num(r.get("MASKE"), 0))
            if dt.upper() == "BITFIELD":
                if group_type:
                    pos += parse_type(group_type)[1]
                    group_type, group_mask = None, 0
                sub = self.tables.get((r.get("NAME") or "").upper())
                size = self.bit_rows(did, sub, pos, r.get("L/H")) if sub and "MASKE" in sub[0] else None
                if size is None:
                    return False  # nieznany rozmiar - dalsze pozycje byłyby błędne
                pos += size
                continue
            t = parse_type(dt)
            if not t:
                return False
            if mask:
                # Kolejna maska tego samego typu bez nakładania bitów = ten sam bajt/słowo
                if group_type == dt and not (group_mask & mask):
                    group_mask |= mask
                else:
                    if group_type:
                        pos += parse_type(group_type)[1]
                    group_type, group_mask = dt, mask
                self.add(did, r, t[0], pos, mask, byte_order(r.get("L/H"), dt))
                continue
            if group_type:
                pos += parse_type(group_type)[1]
                group_type, group_mask = None, 0
            self.add(did, r, t[0], pos, 0, byte_order(r.get("L/H"), dt))
            if t[1] == 0:
                return r is rows[-1]  # "do końca" tylko jako ostatnie pole
            pos += t[1]
        return True

    def run_best(self, raw: bytes) -> int:
        """Odczyty z kodu zadań (stałe zapytanie 22 xx xx), tylko DID nieobecne w tabeli."""
        known = {p[3] for p in self.params}
        found, _ = best_dekoder.extract(raw)
        added = 0
        for x in found:
            if x["did"] in known:
                continue
            typ = ("S" if x["signed"] else "U") + str(8 * x["size"])
            row = {"RESULTNAME": x["result"], "INFO": x["comment"], "EINHEIT": x["unit"],
                   "MUL": repr(x["factor"]), "DIV": "1", "ADD": repr(x["offset"]), "NAME": x["texts"]}
            self.add(x["did"], row, typ, x["pos"], x["mask"], "BE")
            added += 1
        return added

    def run(self):
        for r in self.tables.get("SG_FUNKTIONEN", []):
            services = [s.strip().upper() for s in (r.get("SERVICE") or "").split(";")]
            if "22" not in services:
                continue  # tylko odczyt (pomija 2E zapis, 2F sterowanie, 31 procedury)
            did = (r.get("ID") or "").strip().upper().replace("0X", "")
            if not re.fullmatch(r"[0-9A-F]{4}", did):
                continue
            res = (r.get("RES_TABELLE") or "-").strip().upper()
            dt = (r.get("DATENTYP") or "-").strip()
            if res not in ("", "-"):
                rows = self.tables.get(res)
                if not rows or not self.res_table(did, rows):
                    self.skipped += 1
            elif dt.upper() == "BITFIELD":
                sub = self.tables.get((r.get("NAME") or "").upper())
                if not sub or "MASKE" not in sub[0] or self.bit_rows(did, sub, 0, r.get("L/H")) is None:
                    self.skipped += 1
            elif dt not in ("", "-"):
                t = parse_type(dt)
                if t:
                    self.add(did, r, t[0], 0, 0, byte_order(r.get("L/H"), dt))
                else:
                    self.skipped += 1


# --- Lista podstawowa: najważniejsze wartości do jazdy, z polskimi nazwami ---
# adres -> [(początek klucza, nazwa, jednostka albo None, mnożnik jednostki)]; pozostałe = "wszystkie"
BASIC = {
    "12": [  # silnik DDE
        ("motordrehzahl_wert", "Obroty silnika", None, 1),
        ("ladedruck_wert", "Doladowanie", None, 1),
        ("ladedruck_soll_wert", "Doladowanie zadane", None, 1),
        ("raildruck_wert", "Cisnienie paliwa (rail)", None, 1),
        ("raildruck_soll_wert", "Cisnienie paliwa (rail) zadane", None, 1),
        ("luftmasse_wert", "Masa powietrza", None, 1),
        ("luftmasse_pro_hub_wert", "Masa powietrza na suw", None, 1),
        ("luftmasse_soll_wert", "Masa powietrza na suw zadana", None, 1),
        ("einspritzmenge_aktuell_wert", "Dawka wtrysku", None, 1),
        ("kuehlmitteltemperatur_wert", "Temperatura plynu chlodzacego", None, 1),
        ("motoroel_temperatur_wert", "Temperatura oleju silnika", None, 1),
        ("layer_stoillvlrel_wert", "Poziom oleju (wzgledny)", None, 1),
        ("ansauglufttemperatur_wert", "Temperatura powietrza dolotowego", None, 1),
        ("ladelufttemperatur_wert", "Temperatura za intercoolerem", None, 1),
        ("abgastemperatur_vor_katalysator", "Temperatura spalin przed katalizatorem", None, 1),
        ("abgastemperatur_vor_partikelfilter", "Temperatura spalin przed DPF", None, 1),
        ("russmasse_im_partikelfilter_wert", "DPF: sadza (pomiar)", None, 1),
        ("partikelmasse_simuliert_wert", "DPF: sadza (model)", None, 1),
        ("oelaschenmasse_wert", "DPF: popiol", None, 1),
        ("strecke_seit_erfolgreicher_regeneration", "DPF: km od regeneracji", "km", 0.001),
        ("durchschnitt_kilometer_zwischen_regenera", "DPF: sredni odstep regeneracji", None, 1),
        ("pfltrgn_numrgn_wert", "DPF: zadanie regeneracji", None, 1),
        ("status_regeneration_unterbrochen_wert", "DPF: regeneracja przerwana", None, 1),
        ("kraftstoffverbrauch_seit_erfolgreich", "DPF: paliwo od regeneracji", None, 1),
        ("egrvlvlp_ract_wert", "Zawor EGR (niskocisn.) - pozycja", None, 1),
        ("thrvlv_ract_wert", "Przepustnica - pozycja", None, 1),
        ("trbchvnt_rsens_wert", "Lopatki turbiny (VNT) - pozycja", None, 1),
        ("asmod_ntrbch_wert", "Obroty turbosprezarki (model)", None, 1),
        ("lambda0_wert", "Lambda", None, 1),
        ("verhaeltnis_aktuelles_moment_maxim", "Obciazenie silnika (% momentu max.)", None, 1),
        ("accped_trqdes_wert", "Moment zadany przez kierowce", None, 1),
        ("fahrerwunsch_pedal_wert", "Pedal gazu", None, 1),
        ("app_stkd_wert", "Kickdown", None, 1),
        ("geschwindigkeit_wert", "Predkosc pojazdu", None, 1),
        ("batteriespannung_ibs_wert", "Napiecie akumulatora (IBS)", None, 1),
        ("bcuecu_u_wert", "Napiecie instalacji", None, 1),
        ("kraftstoffverbrauch_absolut_wert", "Paliwo w zbiorniku", None, 1),
        ("umgebungstemperatur_wert", "Temperatura zewnetrzna", None, 1),
        ("umgebungsdruck_wert", "Cisnienie atmosferyczne", None, 1),
        ("udc_mrdcagact_wert", "AdBlue zuzyte od startu", None, 1),
    ],
    "0B": [  # SCR / AdBlue
        ("masse_aktivtank_wert", "AdBlue w zbiorniku aktywnym", None, 1),
        ("masse_passivtank_wert", "AdBlue w zbiorniku pasywnym", None, 1),
        ("aktivtankfuellstand_ungefiltert", "AdBlue - poziom zbiornika aktywnego", None, 1),
        ("pasivtankfuellstand_ungefiltert", "AdBlue - poziom zbiornika pasywnego", None, 1),
        ("temperatur_aktivtank_wert", "AdBlue - temperatura zbiornika aktywnego", None, 1),
        ("temperatur_passivtank_wert", "AdBlue - temperatura zbiornika pasywnego", None, 1),
    ],
    "18": [  # skrzynia biegów
        ("istgang", "Bieg aktualny", None, 1),
        ("eingangsdrehzahl_wert", "Obroty wejsciowe skrzyni", None, 1),
        ("motoroeltemperatur_wert", "Temperatura oleju (wg skrzyni)", None, 1),
        ("motortemperatur_wert", "Temperatura silnika (wg skrzyni)", None, 1),
    ],
    "78": [  # klimatyzacja
        ("temp_innen_wert", "Temperatura wewnatrz", None, 1),
        ("temp_verdampfer_wert", "Temperatura parownika", None, 1),
        ("bus_in_r134a_druck_wert", "Cisnienie czynnika chlodniczego", None, 1),
        ("klima_vorn_geblaeseleistung_wert", "Dmuchawa", None, 1),
        ("klima_vorn_geblaesestufe_anz_wert", "Dmuchawa - stopien", None, 1),
        ("bus_out_klimakompressor_pwm_wert", "Sprezarka klimatyzacji", None, 1),
    ],
    "29": [  # DSC
        ("geschwindigkeit_rad_vl_wert", "Predkosc kola przod lewe", None, 1),
        ("geschwindigkeit_rad_vr_wert", "Predkosc kola przod prawe", None, 1),
        ("geschwindigkeit_rad_hl_wert", "Predkosc kola tyl lewe", None, 1),
        ("geschwindigkeit_rad_hr_wert", "Predkosc kola tyl prawe", None, 1),
    ],
    "60": [  # licznik
        ("bc_rw_l_km_wert", "Zasieg (komputer)", None, 1),
        ("a_temp_anzeige_wert", "Temperatura zewnetrzna (licznik)", None, 1),
    ],
}
# xDrive: wszystkie przetłumaczone wyniki z kodu zadań (TRANSLATIONS) są podstawowe
BASIC_FROM_TRANSLATIONS = {"19"}

# Słownik niemiecko-polski dla pozostałych nazw: całe słowa i części złożeń (bez polskich znaków -
# czcionka ekranu ich nie ma). Złożenie tłumaczone tylko, gdy w całości składa się ze znanych części.
DE_PL = {
    "temperatur": "temperatura", "temp": "temp.", "druck": "cisnienie", "drehzahl": "obroty",
    "spannung": "napiecie", "strom": "prad", "moment": "moment", "drehmoment": "moment obrotowy",
    "geschwindigkeit": "predkosc", "fahrzeuggeschwindigkeit": "predkosc pojazdu", "position": "pozycja",
    "stellung": "pozycja", "winkel": "kat", "zeit": "czas", "dauer": "czas trwania", "anzahl": "liczba",
    "zaehler": "licznik", "wert": "wartosc", "werte": "wartosci", "sollwert": "wartosc zadana",
    "istwert": "wartosc rzeczywista", "soll": "zadane", "ist": "rzeczywiste", "aktuell": "aktualne",
    "aktuelle": "aktualna", "aktueller": "aktualny", "aktuellen": "aktualnego", "status": "status",
    "zustand": "stan", "fehler": "blad", "aktiv": "aktywny", "inaktiv": "nieaktywny", "vorne": "przod",
    "hinten": "tyl", "links": "lewy", "rechts": "prawy", "oben": "gora", "unten": "dol", "motor": "silnik",
    "getriebe": "skrzynia", "kupplung": "sprzeglo", "gang": "bieg", "rad": "kolo", "reifen": "opona",
    "luft": "powietrze", "luftmasse": "masa powietrza", "masse": "masa", "menge": "ilosc", "oel": "olej",
    "kraftstoff": "paliwo", "kuehlmittel": "plyn chlodzacy", "abgas": "spaliny", "ansaug": "dolot",
    "ladedruck": "doladowanie", "ladeluft": "powietrze doladowane", "kuehler": "chlodnica",
    "partikelfilter": "filtr DPF", "russ": "sadza", "asche": "popiol", "regeneration": "regeneracja",
    "einspritzung": "wtrysk", "einspritzmenge": "dawka wtrysku", "injektor": "wtryskiwacz", "rail": "rail",
    "ventil": "zawor", "pumpe": "pompa", "sensor": "czujnik", "fuehler": "czujnik", "signal": "sygnal",
    "batterie": "akumulator", "bordnetz": "instalacja", "klemme": "zacisk", "heizung": "ogrzewanie",
    "sitzheizung": "podgrzewanie fotela", "geblaese": "dmuchawa", "klappe": "klapa", "verdampfer": "parownik",
    "kaeltemittel": "czynnik chlodniczy", "kompressor": "sprezarka", "innen": "wnetrze", "aussen": "zewn.",
    "aussentemperatur": "temperatura zewn.", "umgebung": "otoczenie", "fahrer": "kierowca",
    "beifahrer": "pasazer", "tuer": "drzwi", "fenster": "okno", "fensterheber": "podnosnik szyby",
    "spiegel": "lusterko", "licht": "swiatlo", "lampe": "lampa", "blinker": "kierunkowskaz",
    "taste": "przycisk", "taster": "przycisk", "schalter": "przelacznik", "betaetigt": "wcisniety",
    "gedrueckt": "wcisniety", "nicht": "nie", "kein": "brak", "vorhanden": "obecny", "verbaut": "zamontowany",
    "und": "i", "oder": "lub", "mit": "z", "ohne": "bez", "fuer": "dla", "von": "od", "nach": "po",
    "vor": "przed", "seit": "od", "bei": "przy", "beim": "przy", "im": "w", "in": "w", "der": "", "die": "",
    "das": "", "des": "", "dem": "", "den": "", "ein": "", "eine": "", "einer": "", "letzten": "ostatniej",
    "letzte": "ostatnia", "gefiltert": "filtrowane", "gefilterter": "filtrowany", "gemessen": "zmierzone",
    "berechnet": "obliczone", "modelliert": "modelowane", "maximal": "maks.", "minimal": "min.",
    "mittelwert": "srednia", "durchschnitt": "srednia", "kilometer": "km", "kilometerstand": "przebieg",
    "verbrauch": "zuzycie", "leistung": "moc", "last": "obciazenie", "pedal": "pedal", "fahrpedal": "pedal gazu",
    "lenkung": "uklad kierowniczy", "lenkwinkel": "kat skretu", "bremse": "hamulec", "bremsdruck": "cisnienie hamowania",
    "turbolader": "turbosprezarka", "lader": "turbosprezarka", "drosselklappe": "przepustnica", "agr": "EGR",
    "steller": "nastawnik", "ansteuerung": "sterowanie", "tastverhaeltnis": "wypelnienie PWM", "roh": "surowy",
    "rohwert": "wartosc surowa", "adaption": "adaptacja", "adaptionswerte": "wartosci adaptacji",
    "auslesen": "odczyt", "kalibrierung": "kalibracja", "hochschaltung": "zmiana w gore",
    "rueckschaltung": "redukcja biegu", "schaltung": "zmiana biegu", "wandler": "przemiennik", "tank": "zbiornik",
    "fuellstand": "poziom", "niveau": "poziom", "zylinder": "cylinder", "kamera": "kamera", "anzeige": "wskazanie",
    "alarm": "alarm", "alarme": "alarmy", "kontakt": "styk", "motorhaube": "maska", "wasser": "woda",
    "prozent": "procent", "enthaelt": "zawiera", "zaehlerstand": "stan licznika", "bisher": "dotychczas",
    "steuergeraet": "sterownik", "auf": "na", "zu": "do", "zum": "do", "zur": "do", "hohem": "wysokim",
    "hoher": "wysoki", "schutz": "ochrona", "ergebnis": "wynik", "ergebnisse": "wyniki",
    "ueberwachung": "monitorowanie", "messwert": "pomiar", "beschleunigung": "przyspieszenie",
    "laengs": "wzdluzne", "quer": "poprzeczne", "hoehenstand": "wysokosc nadwozia", "abfrage": "odczyt",
    "defekt": "uszkodzony", "defekter": "uszkodzonych", "pruefung": "test", "pruefungen": "testy",
    "betriebsspannung": "napiecie zasilania", "ausgang": "wyjscie", "eingang": "wejscie", "antrieb": "naped",
    "aktiviert": "aktywowany", "aktivierte": "aktywowane", "darstellung": "wyswietlanie", "parameter": "parametr",
    "satellit": "czujnik satelitarny", "zentral": "centralny", "doppel": "podwojna", "mehrfach": "wielokrotna",
    "bereich": "zakres", "grenze": "granica", "begrenzung": "ograniczenie", "freigabe": "zezwolenie",
    "anforderung": "zadanie", "zeitpunkt": "chwila", "letzter": "ostatni", "erfolgreich": "udany",
    "erfolgreichen": "udanej", "erfolgreicher": "udanej", "gefahrene": "przejechana", "strecke": "droga",
    "betrieb": "praca", "betriebsdauer": "czas pracy", "stunden": "godziny", "sekunden": "sekundy",
    "kurbelwelle": "wal korbowy", "nockenwelle": "wal rozrzadu", "gluehkerze": "swieca zarowa",
    "vorgluehen": "zarzenie", "hochdruck": "wysokie cisnienie", "niederdruck": "niskie cisnienie",
    "turbine": "turbina", "verdichter": "sprezarka", "katalysator": "katalizator", "speicher": "zasobnik",
    "harnstoff": "AdBlue", "dosierung": "dozowanie", "dosierventil": "zawor dozujacy", "beheizung": "ogrzewanie",
    "nox": "NOx", "sauerstoff": "tlen", "kennfeld": "mapa", "korrektur": "korekta", "faktor": "wspolczynnik",
    "rohspannung": "napiecie surowe", "spannungswert": "napiecie", "relativ": "wzgledny", "absolut": "bezwzgledny",
    "gesamt": "calkowity", "summe": "suma", "integral": "calka", "gewicht": "waga", "lage": "polozenie",
    "hoehe": "wysokosc", "laenge": "dlugosc", "breite": "szerokosc", "abstand": "odstep", "objekt": "obiekt",
    "erkannt": "wykryty", "ungueltig": "niewazny", "gueltig": "wazny", "aus": "z",  # "ein" = rodzajnik (wyżej)
    "fahrzeug": "pojazd", "wagen": "pojazd", "lenk": "kierownica", "sitz": "fotel", "gurt": "pas",
    "airbag": "poduszka", "schloss": "zamek", "heck": "tyl", "front": "przod", "dach": "dach",
    "schiebedach": "szyberdach", "wischer": "wycieraczka", "regen": "deszcz", "scheibe": "szyba",
    "helligkeit": "jasnosc", "beleuchtung": "oswietlenie", "scheinwerfer": "reflektor", "warn": "ostrzezenie",
    "warnung": "ostrzezenie", "ereignis": "zdarzenie", "fehlerspeicher": "pamiec bledow", "speicherung": "zapis",
}
_PARTS = sorted(DE_PL, key=len, reverse=True)


def translate_word(word: str) -> str:
    low = word.lower()
    if low in DE_PL:
        return DE_PL[low]
    # Złożenie z samych znanych części, np. "kuehlmitteltemperatur" -> "plyn chlodzacy temperatura"
    out, i = [], 0
    while i < len(low):
        part = next((p for p in _PARTS if len(p) >= 3 and low.startswith(p, i)), None)
        if not part:
            return word
        out.append(DE_PL[part])
        i += len(part)
    return " ".join(o for o in out if o)


def translate_de(name: str) -> str:
    words = re.findall(r"[A-Za-z]+|[^A-Za-z]+", name)
    s = "".join(translate_word(w) if w.isalpha() else w for w in words)
    s = re.sub(r"\s+", " ", s).strip()
    return s[:1].upper() + s[1:] if s else name


def simplify(row: list) -> list:
    """Wiersz parametru -> nazwa po polsku, ewentualnie przeliczona jednostka, poziom (1 = podstawowy)."""
    adres, key, unit = row[0], row[1], row[10]
    level = 0
    for prefix, name, new_unit, scale in BASIC.get(adres, []):
        if key.startswith(prefix) and unit != "mV":  # mV = surowe napięcie czujnika, nie wartość
            level = 1
            row[2] = name
            if new_unit:
                row[10] = new_unit
                row[8] = fmt_num(float(row[8]) * scale)
                row[9] = fmt_num(float(row[9]) * scale)
                row[11] = str(min(3, decimals_for(float(row[8]), float(row[9]))))
            break
    if not level and adres in BASIC_FROM_TRANSLATIONS and row[2].startswith("xDrive"):
        level = 1
    if not level:
        row[2] = ascii_text(translate_de(row[2]), 60)
    return row + [str(level)]


def find_file(folder: Path, sgbd: str):
    for p in folder.iterdir():
        if p.stem.upper() == sgbd and p.suffix.lower() == ".prg":
            return p
    return None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    prg_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    grtb = read_tables(find_file(prg_dir, "T_GRTB"))
    addr_of = {}
    for r in grtb.get("ZUORDNUNGSTABELLEUDS", []):
        addr_of.setdefault(r["SGBD"].upper(), r["ADR_INDEX"].split()[0].upper())

    modules, all_params, all_texts, all_dtcs = [], [], [], []
    for sgbd, label in CAR_ECUS.items():
        adres = addr_of.get(sgbd, "")
        path = find_file(prg_dir, sgbd)
        note = []
        if not adres:
            note.append("brak adresu w T_GRTB")
        if not path:
            modules.append([adres, sgbd, label, "0", "0", "brak pliku .prg"])
            print(f"[--] {sgbd:9} {adres:2}  brak pliku .prg")
            continue
        tables = read_tables(path)
        ex = Extractor(adres, tables)
        ex.run()
        n_best = ex.run_best(path.read_bytes())
        if n_best:
            note.append(f"z kodu zadan: {n_best}")
        if "SG_FUNKTIONEN" not in tables and not n_best:
            note.append("brak tabeli parametrow")
        if ex.skipped:
            note.append(f"pominiete ramki: {ex.skipped}")

        dtcs = []
        for tname, kind in (("FORTTEXTE", "F"), ("IORTTEXTE", "I")):
            for r in tables.get(tname, []):
                code = (r.get("ORT") or "").strip().upper().replace("0X", "")
                text = ascii_text(r.get("ORTTEXT", ""), 160)
                if re.fullmatch(r"[0-9A-F]{6}", code) and text:
                    dtcs.append((code, kind, text))
        dtcs = sorted(set(dtcs))
        if adres:
            all_dtcs += [(adres, code, kind, text) for code, kind, text in dtcs]

        all_params += ex.params
        for tname, entries in ex.text_tables.items():
            for wert, text in entries:
                all_texts.append([adres, tname, wert, text])
        modules.append([adres, sgbd, label, str(len(ex.params)), str(len(dtcs)), ", ".join(note)])
        print(f"[OK] {sgbd:9} {adres:2}  parametry: {len(ex.params):5}  bledy: {len(dtcs):5}  {', '.join(note)}")

    # Pierwsza linia każdego pliku: wersja i data (urządzenie pomija linie z "#", pokazuje je w Info/WWW)
    meta = (f"# Diamel katalog;wersja={CATALOG_VERSION};data={datetime.datetime.now():%Y-%m-%d %H:%M};"
            f"auto={VEHICLE}\n")
    for old in out_dir.glob("bledy_*.csv"):  # stare pliki sprzed scalenia w bledy.csv
        old.unlink()

    with open(out_dir / "moduly.csv", "w", encoding="utf-8", newline="\n") as f:
        f.write(meta)
        f.write("adres;sgbd;nazwa;parametry;bledy;uwagi\n")
        for m in sorted(modules):
            f.write(";".join(m) + "\n")
        f.write(f"{OBD_ADDR};OBD2;OBD-II (silnik);{len(OBD_PIDS)};0;standard SAE J1979, tryb 01\n")
    with open(out_dir / "parametry.csv", "w", encoding="utf-8", newline="\n") as f:
        f.write(meta)
        f.write("adres;klucz;nazwa;did;typ;pozycja;maska;kolejnosc;mnoznik;przesuniecie;jednostka;miejsca;teksty;opis;"
                "poziom\n")
        basic = 0
        seen = set()  # ta sama wartość pod kilkoma DID (np. xDrive) - w podstawowej tylko raz
        for p in sorted(all_params, key=lambda p: (p[0], p[3], int(p[5]), int(p[6], 16))):
            row = simplify(p)
            if row[-1] == "1":
                if (row[0], row[2]) in seen:
                    row[-1] = "0"
                seen.add((row[0], row[2]))
            basic += row[-1] == "1"
            f.write(";".join(row) + "\n")
        for row in obd_rows():
            basic += row[-1] == "1"
            f.write(";".join(row) + "\n")
    print(f"Lista podstawowa: {basic} parametrow (pozostale w 'wszystkie')")
    with open(out_dir / "teksty.csv", "w", encoding="utf-8", newline="\n") as f:
        f.write(meta)
        f.write("adres;tabela;wartosc;tekst\n")
        for t in all_texts:
            f.write(";".join(t) + "\n")
    # Wszystkie kody błędów w jednym pliku, po adresie modułu (urządzenie robi z niego indeks)
    with open(out_dir / "bledy.csv", "w", encoding="utf-8", newline="\n") as f:
        f.write(meta)
        f.write("adres;kod;rodzaj;opis\n")
        for d in sorted(all_dtcs):
            f.write(";".join(d) + "\n")
    print(f"Kody bledow: {len(all_dtcs)} w bledy.csv")
    print(f"\nRazem: {len(all_params)} parametrow, {len(all_texts)} opisow wartosci -> {out_dir}")


if __name__ == "__main__":
    main()
