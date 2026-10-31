#!/usr/bin/env python3
"""Shared parser for the BMS073 configuration dialect (YD/T 1363.3 ASCII family).

Frame syntax (SOI, LENID/LCHKSUM, INFO, CHKSUM), the D547 parameter-block
layout, and the write-frame builder live HERE and nowhere else. The other
scripts in this directory are thin wrappers; payload names/scales/units are
loaded from BMS073-H17-BL08-16S-en-US.xml at the repo root so the layout is
not re-encoded here.

Protocol reference: docs/PROTOCOL_REFERENCE.md, "BMS073 Configuration
Protocol (Richpower, battery-battery port)".
"""
from __future__ import annotations

import struct
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

SOI = 0x7E                  # '~'
EOI = 0x0D                  # CR
VER_CONFIG = 0x80
CID1_CONFIG = 0xD5
ADR_CLIENT = 0x00           # external tool / config-bridge requests
ADR_STACK_MASTER = 0x50      # the stack master's poll role on the Link bus

XML_PATH = Path(__file__).resolve().parents[3] / "BMS073-H17-BL08-16S-en-US.xml"

HEX_DIGITS = frozenset("0123456789ABCDEFabcdef")
HEADER_CHARS = 12        # VER+ADR+CID1+CID2 (8) + LENID (4) before INFO

# Empirically-signed int parameters: the XML does not declare signedness, so
# it is maintained per field. Signed set (high bit set, negative is the only
# plausible reading): the discharge-current alarm/protect family 0x31/0x32/
# 0x37/0x39/0x3B (-100/-105/-110/-120/-412 A at rest) and the PCS fields
# 0x5C/0x79 (0x5C decodes -1000 A, mirroring 0x5B's +1000 A charge limit -
# an ordinary limit, not a marker). Explicitly
# unsigned despite a high raw: 0x59/0x5A, the PCS charge/discharge cut-off
# voltages 55.5 V / 44.5 V. test_bms073 walks every raw >= 0x8000 against
# this classification so a newly-high index cannot pass unclassified.
SIGNED_PARAMS = frozenset({0x31, 0x32, 0x37, 0x39, 0x3B, 0x5C, 0x79})

# Observed slave pack addresses on the Link bus (master polls 01-04; it never
# polls itself, battery numbering has the master at 0). FF is the broadcast
# address the vendor tool uses for identity probes and 00 is the standalone
# self-address, so neither is a valid write/read target on the live bus.
LINK_SLAVE_PACKS = range(1, 5)


class FrameError(ValueError):
    pass


def checksum(body: str) -> int:
    """CHKSUM: two's complement of the ASCII-byte sum of everything between
    SOI and the checksum field (same function as rs485_calc_chksum() in
    pylontech_protocol.h). Encodes latin-1 so whole-frame validation over
    collision-damaged input is a defined byte sum, not a crash."""
    return (0x10000 - sum(body.encode("latin-1"))) & 0xFFFF


def lenid(info_chars: int) -> str:
    """LENID = LCHKSUM nibble + 3-hex INFO character count."""
    length = f"{info_chars:03X}"
    lchk = (~sum(int(c, 16) for c in length) + 1) & 0xF
    return f"{lchk:X}{length}"


def build(ver: int = VER_CONFIG, adr: int = ADR_CLIENT,
          cid1: int = CID1_CONFIG, cid2: int = 0x49, info: str | bytes = "") -> bytes:
    """Build a wire frame. info may be ASCII-hex text (the dialect's encoding)
    or raw bytes (hex-encoded automatically)."""
    if isinstance(info, (bytes, bytearray)):
        info = bytes(info).hex().upper()
    if len(info) % 2:
        raise FrameError("INFO must be an even number of ASCII-hex chars")
    try:
        int(info, 16) if info else 0
    except ValueError:
        raise FrameError(f"INFO is not ASCII-hex: {info!r}") from None
    body = f"{ver:02X}{adr:02X}{cid1:02X}{cid2:02X}{lenid(len(info))}{info}"
    return f"~{body}{checksum(body):04X}\r".encode("ascii")


@dataclass
class Frame:
    """A parsed frame. INFO is decoded to real bytes exactly once; downstream
    code uses named fields and ParameterBlock, never positional slices."""
    raw: bytes
    ver: int
    adr: int
    cid1: int
    cid2: int                  # on responses this field carries RTN
    info_text: str             # INFO ASCII-hex chars as received (garbage included)
    checksum_valid: bool
    lenid_valid: bool
    truncated: bool = False

    @property
    def rtn(self) -> int:
        return self.cid2

    @property
    def stored_checksum(self) -> int | None:
        try:
            return int(self.raw[-5:-1].decode("ascii"), 16)
        except ValueError:
            return None

    @property
    def damaged(self) -> bool:
        return not self._hex_prefix_is_clean(self.info_text)

    def info_bytes(self) -> bytes:
        """INFO decoded to bytes. Fails on mid-frame collision damage —
        use info_prefix() for damaged frames."""
        if self.damaged:
            raise FrameError("INFO contains non-hex bytes (collision damage)")
        return bytes.fromhex(self.info_text) if self.info_text else b""

    def info_prefix(self) -> bytes:
        """Bytes decoded from INFO up to the first damaged byte position.
        Bytes after the first damage are not trustworthy even if they look
        like valid hex, so the prefix ends there."""
        cut = self._damage_start(self.info_text) & ~1   # whole byte boundary
        return bytes.fromhex(self.info_text[:cut]) if cut else b""

    def damage_span(self) -> tuple[int, int] | None:
        """(start, end) in INFO *character* coords covering the corrupted
        window: from the first to the last non-hex char, inclusive. None if
        the frame is clean."""
        bad = [i for i, c in enumerate(self.info_text) if c not in HEX_DIGITS]
        if not bad:
            return None
        return bad[0], bad[-1]

    def verify_stored_checksum_against(self, reference_info_text: str) -> bool:
        """Prove the responder transmitted a complete, validly-checksummed
        frame despite on-wire collision damage: splice the reference INFO
        over the damaged window and test the stored CHKSUM against it."""
        span = self.damage_span()
        if span is None:
            return self.checksum_valid
        start, end = span
        if len(reference_info_text) < end + 1:
            raise FrameError("reference INFO too short to cover the damaged window")
        text = self.raw.decode("latin-1")[1:]     # strip SOI
        header = text[:HEADER_CHARS]
        info_clean = (self.info_text[:start]                      # own undamaged prefix
                      + reference_info_text[start:end + 1]         # reference over damage
                      + self.info_text[end + 1:])                  # own undamaged tail
        return checksum(header + info_clean) == self.stored_checksum

    @staticmethod
    def _hex_prefix_is_clean(text: str) -> bool:
        return all(c in HEX_DIGITS for c in text)

    @staticmethod
    def _damage_start(text: str) -> int:
        for i, c in enumerate(text):
            if c not in HEX_DIGITS:
                return i
        return len(text)

    @classmethod
    def _from_candidate(cls, raw: bytes) -> "Frame":
        text = raw[1:-1].decode("latin-1")
        if len(text) < HEADER_CHARS or not all(c in HEX_DIGITS for c in text[:HEADER_CHARS]):
            raise FrameError("header is not ASCII-hex")
        ver, adr, cid1, cid2 = (int(text[i:i + 2], 16) for i in (0, 2, 4, 6))
        lchk = int(text[8], 16)
        declared = int(text[9:12], 16)
        info_text = text[HEADER_CHARS:]
        if len(text) < HEADER_CHARS + 4:
            raise FrameError("no checksum field")
        lenid_valid = lchk == ((~sum(int(c, 16) for c in text[9:12]) + 1) & 0xF)
        lenid_valid = lenid_valid and len(text) == HEADER_CHARS + declared + 4
        info_text = info_text[:declared] if lenid_valid else info_text
        try:
            stored = int(text[-4:], 16)
        except ValueError:
            raise FrameError("checksum field is not ASCII-hex") from None
        checksum_valid = stored == checksum(text[:-4]) and lenid_valid
        return cls(raw=raw, ver=ver, adr=adr, cid1=cid1, cid2=cid2,
                   info_text=info_text,
                   checksum_valid=checksum_valid, lenid_valid=lenid_valid)

    @classmethod
    def _from_truncated(cls, raw: bytes) -> "Frame":
        """A frame whose EOI was destroyed by a collision (or cut off at the
        end of a capture): header parsed if sane, declared INFO as far as
        present, checksum treated as unverifiable."""
        text = raw[1:].decode("latin-1")
        if len(text) < HEADER_CHARS or not all(c in HEX_DIGITS for c in text[:HEADER_CHARS]):
            raise FrameError("header is not ASCII-hex")
        ver, adr, cid1, cid2 = (int(text[i:i + 2], 16) for i in (0, 2, 4, 6))
        lchk = int(text[8], 16)
        declared = int(text[9:12], 16)
        return cls(raw=raw, ver=ver, adr=adr, cid1=cid1, cid2=cid2,
                   info_text=text[HEADER_CHARS:HEADER_CHARS + declared],
                   checksum_valid=False,
                   lenid_valid=lchk == ((~sum(int(c, 16) for c in text[9:12]) + 1) & 0xF),
                   truncated=True)

    @classmethod
    def parse(cls, data: bytes) -> "Frame":
        """Strict parse of one complete frame. Raises FrameError."""
        s = data.find(b"~")
        if s == -1:
            raise FrameError("no SOI")
        e = data.find(b"\r", s)
        if e == -1:
            raise FrameError("no EOI (truncated)")
        return cls._from_candidate(data[s:e + 1])


def parse_stream(data: bytes):
    """Tolerant frame extraction from a raw serial byte stream (real captures
    contain mid-frame RS485 collision garbage). Yields Frame objects; frames
    that are damaged or fail their whole-frame CHKSUM are still yielded with
    their validity flags set, so evidence can be extracted from undamaged
    regions. Unrecognized bytes between frames are skipped."""
    pos = 0
    n = len(data)
    while pos < n:
        s = data.find(b"~", pos)
        if s == -1:
            return
        e = data.find(b"\r", s)
        nxt = data.find(b"~", s + 1)
        if e != -1 and (nxt == -1 or e < nxt):
            raw = data[s:e + 1]
            try:
                yield Frame._from_candidate(raw)
            except FrameError:
                pass
            pos = e + 1
        elif nxt != -1:
            # EOI destroyed by a collision (a '~' shows up before any CR):
            # salvage what remains of this frame, then resume at the new '~'
            try:
                yield Frame._from_truncated(data[s:nxt])
            except FrameError:
                pass
            pos = nxt
        else:
            try:
                yield Frame._from_truncated(data[s:])
            except FrameError:
                pass
            return


class ParameterBlock:
    """The CID2 47 (D547) parameter block.

    INFO bytes:   [battery number: 1] [array: 271] ... total 273
    Array bytes:  u16 idx 0x00-0x06 (14 B) + 1 stray byte (observed 0x76,
                  meaning unknown, not in the XML) + u16 idx 0x07-0x7C
                  (236 B) + bit-params 0x7D.. (10 B) + 11-byte tail.
    """
    STRAY_BYTE_ARRAY_OFFSET = 14        # array byte holding the stray 0x76
    BITS_ARRAY_OFFSET = 15 + 2 * (0x7D - 7)      # 251
    BITS_LENGTH = 10
    TAIL_ARRAY_OFFSET = BITS_ARRAY_OFFSET + BITS_LENGTH   # 261
    TAIL_LENGTH = 11
    FULL_INFO_BYTES = 1 + TAIL_ARRAY_OFFSET + TAIL_LENGTH  # 273: battery + array

    def __init__(self, info: bytes, table: dict | None = None):
        if len(info) < self.FULL_INFO_BYTES:
            raise FrameError(f"parameter block short of full 273-byte INFO: {len(info)} bytes")
        self.info = info
        self.battery_number = info[0]
        self.array = info[1:]
        self.stray_byte = self.array[self.STRAY_BYTE_ARRAY_OFFSET]
        self.bit_bytes = self.array[self.BITS_ARRAY_OFFSET:self.BITS_ARRAY_OFFSET + self.BITS_LENGTH]
        self.tail = self.array[self.TAIL_ARRAY_OFFSET:self.TAIL_ARRAY_OFFSET + self.TAIL_LENGTH]
        self.table = table if table is not None else load_xml_table()

    def byte_offset(self, index: int) -> int:
        """INFO-byte offset of int parameter `index` (XML ParaIndex 0x00-0x7C)."""
        if index < 7:
            off = 2 * index
        elif index <= 0x7C:
            off = self.STRAY_BYTE_ARRAY_OFFSET + 1 + 2 * (index - 7)
        else:
            raise FrameError(f"not an int-parameter index: {index:#04x}")
        if off + 2 > len(self.array):
            raise FrameError(f"index {index:#04x} beyond available bytes")
        return 1 + off                              # +1 for the battery-number byte

    def raw(self, index: int) -> int:
        return struct.unpack_from(">H", self.info, self.byte_offset(index))[0]

    def scaled(self, index: int) -> float:
        raw = self.raw(index)
        if index in SIGNED_PARAMS and raw >= 0x8000:
            raw -= 0x10000
        return raw * self.table[index]["scale"]

    def name(self, index: int) -> str:
        return self.table[index]["name"]

    def bit(self, byte_index: int, bit_index: int) -> int:
        """bitPara value as (byte, bit) per the XML's ByteIndex/BitIndex."""
        if not 0 <= byte_index < self.BITS_LENGTH:
            raise FrameError(f"bit byte {byte_index} outside the 10-byte block")
        if not 0 <= bit_index <= 7:
            raise FrameError(f"bit index {bit_index} invalid")
        if len(self.bit_bytes) <= byte_index:
            raise FrameError("bit block unavailable (damaged-frame prefix ends early)")
        return (self.bit_bytes[byte_index] >> bit_index) & 1

    @classmethod
    def from_frame(cls, frame: Frame, table: dict | None = None) -> "ParameterBlock":
        """Full block from a clean frame; on a damaged frame, accepts the
        undamaged prefix (and only indices inside it will be readable)."""
        if frame.damaged:
            info = frame.info_prefix()
            if len(info) <= 1:
                raise FrameError("no undamaged bytes")
            block = cls.__new__(cls)
            block.info = info
            block.battery_number = info[0]
            block.array = info[1:]
            if len(block.array) <= block.STRAY_BYTE_ARRAY_OFFSET:
                raise FrameError("undamaged prefix ends before parameter array")
            block.stray_byte = block.array[block.STRAY_BYTE_ARRAY_OFFSET]
            block.bit_bytes = b""
            block.tail = b""
            block.table = table if table is not None else load_xml_table()
            return block
        return cls(frame.info_bytes(), table)


def load_xml_table(path: Path = XML_PATH) -> dict[int, dict]:
    """int_para entries keyed by XML ParaIndex: {name, scale, unit, bytenum}.
    Indices 0-2 appear twice in the XML (alarm parameters and the calibration
    entries reuse them) — the first entry wins; writes to duplicates are
    ambiguous and must be excluded from any write surface."""
    def txt(el, tag):
        for child in el:
            if child.tag.split("}")[-1] == tag:
                return (child.text or "").strip()
        return None

    root = ET.parse(path).getroot()
    table: dict[int, dict] = {}
    for el in root.iter():
        if el.tag.split("}")[-1] == "int_para" and txt(el, "ParaIndex") is not None:
            idx = int(txt(el, "ParaIndex"), 16)
            if idx not in table:
                table[idx] = {"name": txt(el, "Name") or "",
                              "scale": float(txt(el, "Scale") or 1),
                              "unit": txt(el, "UnitName") or "",
                              "bytenum": int(txt(el, "ByteNum") or 2)}
    return table


def duplicate_xml_indices(path: Path = XML_PATH) -> list[int]:
    seen: set[int] = set()
    dups: set[int] = set()
    for idx in _xml_para_indices(path):
        if idx in seen:
            dups.add(idx)
        seen.add(idx)
    return sorted(dups)


def _xml_para_indices(path: Path) -> list[int]:
    root = ET.parse(path).getroot()
    indices: list[int] = []
    for el in root.iter():
        if el.tag.split("}")[-1] != "int_para":
            continue
        for child in el:
            if child.tag.split("}")[-1] == "ParaIndex" and (child.text or "").strip():
                indices.append(int(child.text.strip(), 16))
                break
    return indices
