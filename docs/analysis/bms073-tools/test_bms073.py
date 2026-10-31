#!/usr/bin/env python3
"""Fixture tests for bms073.py — pin the protocol facts documented in
docs/PROTOCOL_REFERENCE.md against the committed capture data.

Fixtures: docs/analysis/bms073-tools/transcript.txt (USB capture frames,
byte-verbatim) and the in-stack D547 read-back frame from LINK log
linkbus_verify_162203.log (raw bytes embedded below, including the
collision damage at INFO chars 319-337).

Run: python3 -m unittest test_bms073 -v
"""
import re
import unittest
from pathlib import Path

import bms073
from bms073 import Frame, ParameterBlock, build, parse_stream

HERE = Path(__file__).resolve().parent

# In-stack D547 response, 2026-10-31 read-back of pack 01 after writing
# Start Balance Voltage 0x0D66. Raw wire bytes incl. RS485 collision damage
# (the master's poll fired ~120 ms before the end of this ~590 ms response).
# Byte-exact from LINK log linkbus_verify_162203.log.
DAMAGED_D547 = (HERE / "instack_d547_damaged.bin").read_bytes()
MASTER_NEXT_REQUEST = b"~8050D544E00203FD18\r"


def standalone_frames():
    frames = []
    with (HERE / "transcript.txt").open(errors="replace") as fh:
        for line in fh:
            m = re.search(r"(~[0-9A-F]+)\\x0d", line)
            if m and "A222" in m.group(1):
                frames.append(Frame.parse(m.group(1).encode() + b"\r"))
    return frames


class TestLenidAndBuilder(unittest.TestCase):
    def test_lenid_formulas(self):
        self.assertEqual(bms073.lenid(2), "E002")
        self.assertEqual(bms073.lenid(8), "8008")
        self.assertEqual(bms073.lenid(0), "0000")
        self.assertEqual(bms073.lenid(546), "A222")

    def test_builder_reproduces_captured_frames(self):
        self.assertEqual(build(cid2=0x49, info="014C0D66"),
                         b"~8000D5498008014C0D66FBCA\r")
        self.assertEqual(build(cid2=0x42, info="01"),
                         b"~8000D542E00201FD21\r")
        self.assertEqual(build(cid2=0x00, info=""),
                         b"~8000D5000000FD9F\r")

    def test_builder_rejects_bad_info(self):
        with self.assertRaises(bms073.FrameError):
            build(info="0G")
        with self.assertRaises(bms073.FrameError):
            build(info="014")


class TestStandaloneCapture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.frames = standalone_frames()
        cls.first, cls.second = cls.frames

    def test_two_clean_reads(self):
        self.assertEqual(len(self.frames), 2)
        for f in self.frames:
            self.assertTrue(f.checksum_valid)
            self.assertTrue(f.lenid_valid)
            self.assertEqual(f.rtn, 0x00)
            self.assertEqual(f.ver, 0x80)
            self.assertEqual(f.cid1, 0xD5)

    def test_doc_anchor_char_positions(self):
        block = ParameterBlock.from_frame(self.first)
        self.assertEqual(block.byte_offset(0x4C), 154)
        self.assertEqual(self.first.info_text[308:312], "0D5C")
        self.assertEqual(f"{block.raw(0x4C):04X}", self.first.info_text[308:312])

    def test_layout(self):
        block = ParameterBlock.from_frame(self.first)
        self.assertEqual(block.battery_number, 0)
        self.assertEqual(block.stray_byte, 0x76)
        self.assertEqual(block.tail, b"/073-BLC06\x00")
        self.assertEqual(block.name(0x4C), "Start Balance Voltage")

    def test_key_values(self):
        b = ParameterBlock.from_frame(self.first)
        self.assertEqual(b.raw(0x04), 3650)
        self.assertAlmostEqual(b.scaled(0x04), 3.650, places=6)
        self.assertEqual(b.raw(0x48), 1440)          # Sleep Delay 24 h
        self.assertEqual(b.raw(0x50), 240)           # Wake-up delay
        self.assertEqual(b.raw(0x51), 10)            # Wake-up times
        self.assertEqual(b.raw(0x5D), 3600)
        self.assertEqual(b.raw(0x5E), 48)
        self.assertEqual(b.raw(0x77), 10000)         # design capacity scale 0.01
        self.assertAlmostEqual(b.scaled(0x77), 100.0, places=6)
        self.assertEqual(b.raw(0x4C), 3420)
        self.assertAlmostEqual(b.scaled(0x4C), 3.420, places=6)

    def test_residual_capacity_is_live(self):
        b1 = ParameterBlock.from_frame(self.first)
        b2 = ParameterBlock.from_frame(self.second)
        self.assertEqual(b1.raw(0x45), 25703)
        self.assertEqual(b2.raw(0x45), 25701)


class TestInStackDamagedFrame(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        stream = list(parse_stream(DAMAGED_D547 + MASTER_NEXT_REQUEST))
        cls.frames = stream

    def test_stream_yields_both_frames(self):
        self.assertEqual(len(self.frames), 2)

    def test_damaged_response_flags(self):
        f = self.frames[0]
        self.assertFalse(f.checksum_valid)
        self.assertTrue(f.lenid_valid)
        self.assertEqual(f.rtn, 0x00)
        self.assertEqual(f.damage_span(), (319, 337))

    def test_balance_reads_from_undamaged_prefix(self):
        f = self.frames[0]
        block = ParameterBlock.from_frame(f)
        self.assertEqual(block.battery_number, 1)
        self.assertEqual(block.raw(0x4C), 0x0D66)
        self.assertAlmostEqual(block.scaled(0x4C), 3.430, places=6)
        self.assertNotIn("0D5C", f.info_text)

    def test_stored_checksum_proves_full_transmission(self):
        f = self.frames[0]
        reference = standalone_frames()[0].info_text
        self.assertEqual(f.stored_checksum, 0x87C1)
        self.assertTrue(f.verify_stored_checksum_against(reference))

    def test_master_next_request_untouched(self):
        f = self.frames[1]
        self.assertEqual(f.adr, 0x50)
        self.assertEqual(f.cid2, 0x44)
        self.assertTrue(f.checksum_valid)
        self.assertEqual(f.info_bytes(), b"\x03")


class TestGuardRails(unittest.TestCase):
    def test_parse_rejects_garbage(self):
        with self.assertRaises(bms073.FrameError):
            Frame.parse(b"not a frame")
        with self.assertRaises(bms073.FrameError):
            Frame.parse(b"~8000\r")

    def test_block_rejects_short_info(self):
        with self.assertRaises(bms073.FrameError):
            ParameterBlock(b"\x00")

    def test_duplicate_xml_indices(self):
        self.assertEqual(bms073.duplicate_xml_indices(), [0, 1, 2])

    def test_ptrs_beyond_prefix_raise(self):
        f = next(f for f in parse_stream(DAMAGED_D547) if f.lenid_valid)
        block = ParameterBlock.from_frame(f)
        with self.assertRaises(bms073.FrameError):
            block.raw(0x5E)


class TestStreamTolerance(unittest.TestCase):
    """Crafted collision shapes the single real capture does not contain
    (external-review round 2): an embedded SOI inside a damaged frame, and a
    non-hex checksum field from an end-of-frame collision."""

    @classmethod
    def setUpClass(cls):
        cls.standalone = standalone_frames()[0]

    def test_embedded_soi_resumes_parsing(self):
        # response whose EOI was destroyed; the colliding master poll (~...)
        # appears before any CR, then a clean frame follows after the poll's CR
        craft = (self.standalone.raw[:100]          # block head, no EOI
                 + b"~8050D544E00203FD18\r"         # colliding poll, clean
                 + b"~8000D547E00200FD1D\r")        # clean short frame
        out = list(parse_stream(craft))
        self.assertEqual(len(out), 3)
        self.assertTrue(out[0].truncated)
        self.assertTrue(out[0].lenid_valid)
        self.assertEqual(out[1].adr, 0x50)
        self.assertTrue(out[1].checksum_valid)
        self.assertEqual(out[2].adr, 0x00)
        self.assertTrue(out[2].checksum_valid)

    def test_nonhex_checksum_does_not_crash(self):
        craft = b"~8000D5000000" + b"\xff\xff\xff\xff" + b"\r"
        self.assertEqual(list(parse_stream(craft)), [])   # skipped, no ValueError
        with self.assertRaises(bms073.FrameError):
            Frame.parse(craft)

    def test_lenid_requires_exact_length(self):
        text = self.standalone.raw.decode("latin-1")[1:-1]
        two_chars_short = ("~" + text[:-2] + "\r").encode()
        self.assertFalse(Frame._from_candidate(two_chars_short).lenid_valid)
        self.assertTrue(Frame._from_candidate(self.standalone.raw).lenid_valid)


class TestHexShapedCorruption(unittest.TestCase):
    """A tampered value that still looks like valid hex: the block reports
    damaged=False and the tampered value, but the whole-frame checksum fails -
    rs485_verify_write.py treats checksum-invalid-without-damage as
    untrustworthy (exit 7), which is the protection for this case."""

    def test_hex_only_tamper_detected_only_by_checksum(self):
        sa = standalone_frames()[0]
        assert sa.info_text.count("0D5C") == 1
        tampered = sa.raw.replace(b"0D5C", b"0D99")
        self.assertNotEqual(tampered, sa.raw)
        f = Frame.parse(tampered)
        self.assertFalse(f.checksum_valid)
        self.assertFalse(f.damaged)
        self.assertEqual(ParameterBlock.from_frame(f).raw(0x4C), 0x0D99)


class TestDocAnchorsRoundTwo(unittest.TestCase):
    """Pins the numbers corrected in external-review round 2."""

    @classmethod
    def setUpClass(cls):
        cls.sa = standalone_frames()[0]
        cls.dmg = next(f for f in parse_stream(DAMAGED_D547) if f.lenid_valid)

    def test_uniform_layout_reads_2_573v(self):
        # without the stray 0x76 byte, the balance region reads at char 306
        self.assertEqual(self.sa.info_text[306:310], "0A0D")
        self.assertAlmostEqual(int(self.sa.info_text[306:310], 16) * 0.001,
                               2.573, places=6)

    def test_damage_span_is_19_chars(self):
        start, end = self.dmg.damage_span()
        self.assertEqual((start, end), (319, 337))
        self.assertEqual(end - start + 1, 19)

    def test_comparable_chars_527_match_521(self):
        sa_info = self.sa.info_text
        clean = (self.dmg.info_text[:319] + sa_info[319:338]
                 + self.dmg.info_text[338:])
        comparable = [(i, a, b) for i, (a, b) in enumerate(zip(clean, sa_info))
                      if not 319 <= i <= 337]
        self.assertEqual(len(comparable), 527)
        self.assertEqual(sum(1 for _, a, b in comparable if a == b), 521)
        self.assertEqual([i for i, a, b in comparable if a != b],
                         [1, 281, 282, 283, 310, 311])

    def test_instack_stray_byte_is_also_0x76(self):
        block = ParameterBlock.from_frame(self.dmg)
        self.assertEqual(block.stray_byte, 0x76)

    def test_bit_flags_match_both_frames(self):
        sa_block = ParameterBlock.from_frame(self.sa)
        self.assertEqual(sa_block.bit_bytes.hex().upper(), "0AFFFF9FFF06FB0B0200")
        flags = {(5, 1): 1, (5, 2): 1, (5, 3): 0, (5, 7): 0,
                 (3, 5): 0, (3, 6): 0}
        for (byte, bit), want in flags.items():
            self.assertEqual(sa_block.bit(byte, bit), want, f"{byte}.{bit}")
        # in-stack pack 01: bit region is outside the damage window, readable
        # via the reconstruction the stored checksum proves complete
        self.assertTrue(self.dmg.verify_stored_checksum_against(self.sa.info_text))
        bit_chars = (1 + sa_block.BITS_ARRAY_OFFSET) * 2
        self.assertEqual(self.dmg.info_text[bit_chars:bit_chars + 20].upper(),
                         sa_block.bit_bytes.hex().upper())

    def test_signed_overdischarge_alarm(self):
        block = ParameterBlock.from_frame(self.sa)
        self.assertEqual(block.name(0x32), "Over Discharge Current Alarm")
        self.assertEqual(block.raw(0x32), 0xFF97)
        self.assertEqual(block.scaled(0x32), -105.0)


class TestBlockLength(unittest.TestCase):
    def test_requires_full_273_byte_info(self):
        full = standalone_frames()[0].info_bytes()
        self.assertEqual(len(full), 273)
        ParameterBlock(full)                       # exact 273: fine
        for short in (272, 261, 262):
            with self.assertRaises(bms073.FrameError):
                ParameterBlock(bytes(short))


class TestWriteGates(unittest.TestCase):
    """rs485_write_param.py validation - it gates writes to live packs."""

    @staticmethod
    def args(pack="01", param="4C", mv=3420, raw=None):
        import types
        return types.SimpleNamespace(pack=pack, param=param, mv=mv, raw=raw)

    @classmethod
    def setUpClass(cls):
        import rs485_write_param
        cls.validate = staticmethod(rs485_write_param.validate)

    def test_accepts_valid_request(self):
        self.assertEqual(self.validate(self.args()), (1, 0x4C, 3420))

    def test_range_boundaries(self):
        self.assertEqual(self.validate(self.args(mv=3400))[2], 3400)
        self.assertEqual(self.validate(self.args(mv=3500))[2], 3500)

    def test_rejects_out_of_range(self):
        for mv in (3399, 3501, 9999):
            with self.assertRaises(SystemExit):
                self.validate(self.args(mv=mv))

    def test_rejects_non_slave_packs(self):
        for pack in ("00", "05", "FF", "zz"):
            with self.assertRaises(SystemExit):
                self.validate(self.args(pack=pack))

    def test_rejects_multibyte_param_index(self):
        with self.assertRaises(SystemExit):
            self.validate(self.args(param="0100", mv=None, raw="0001"))

    def test_rejects_off_allowlist_without_force_escape(self):
        # there is no --force: protection params, live values and duplicated
        # XML indices can only be enabled by editing ALLOWED_PARAMS with a range
        for param in ("04", "45", "00", "5F"):
            with self.assertRaises(SystemExit):
                self.validate(self.args(param=param, mv=None, raw="0D66"))


class TestSignedClassification(unittest.TestCase):
    """Every index whose raw value has the high bit set must be explicitly
    classified as signed (SIGNED_PARAMS) or known-unsigned-high. Guards
    against a new high-bit index passing unclassified."""

    KNOWN_UNSIGNED_HIGH = {0x59, 0x5A}   # PCS cut-off voltages 55.5 V / 44.5 V

    def test_discharge_family_reads_negative(self):
        block = ParameterBlock.from_frame(standalone_frames()[0])
        for idx, want in ((0x31, -100.0), (0x32, -105.0), (0x37, -110.0),
                          (0x39, -120.0), (0x3B, -412.0)):
            self.assertEqual(block.scaled(idx), want, f"{idx:#04x} {block.name(idx)}")

    def test_pcs_cutoffs_stay_unsigned(self):
        block = ParameterBlock.from_frame(standalone_frames()[0])
        self.assertEqual(block.name(0x59), "PCS charging cut-off voltage")
        self.assertAlmostEqual(block.scaled(0x59), 55.5, places=6)
        self.assertEqual(block.name(0x5A), "PCS discharge cut-off voltage")
        self.assertAlmostEqual(block.scaled(0x5A), 44.5, places=6)

    def test_every_high_bit_index_is_classified(self):
        block = ParameterBlock.from_frame(standalone_frames()[0])
        for idx in range(0x7D):
            raw = block.raw(idx)
            if raw >= 0x8000:
                self.assertIn(idx, bms073.SIGNED_PARAMS | self.KNOWN_UNSIGNED_HIGH,
                              f"{idx:#04x} ({block.name(idx)}) reads {raw:#06x} "
                              f"- classify it as signed or known-unsigned")


class TestVerifySemantics(unittest.TestCase):
    """rs485_verify_write.py: --pack validation and the evidence-graded
    exit codes, including the two crash shapes from external-review round 3
    (EOI lost with garbage at the cut; damage before the parameter region
    with a passing reconstruction)."""

    @classmethod
    def setUpClass(cls):
        import rs485_verify_write
        cls.tool = rs485_verify_write
        cls.sa = standalone_frames()[0]
        cls.dmg = next(f for f in parse_stream(DAMAGED_D547) if f.lenid_valid)
        cls.ref = cls.sa.info_text

    def test_pack_validation(self):
        for bad in ("FF", "00", "05", "zz"):
            with self.assertRaises(SystemExit):
                self.tool.parse_args(["--pack", bad, "--expect", "0D66"])
        args = self.tool.parse_args(["--pack", "01", "--expect", "0D66"])
        self.assertEqual(args.pack, 1)

    def test_clean_frame_exits_zero(self):
        lines, code = self.tool.evaluate(self.sa, 0, 3420, self.ref)
        self.assertEqual(code, 0)
        self.assertTrue(any("value matches" in ln for ln in lines))

    def test_wrong_value_exits_five(self):
        lines, code = self.tool.evaluate(self.sa, 0, 3429, self.ref)
        self.assertEqual(code, 5)

    def test_wrong_battery_exits_four(self):
        lines, code = self.tool.evaluate(self.dmg, 2, 0x0D66, self.ref)
        self.assertEqual(code, 4)

    def test_damage_after_parameter_with_proof_exits_zero(self):
        lines, code = self.tool.evaluate(self.dmg, 1, 0x0D66, self.ref)
        self.assertEqual(code, 0)

    def test_eoi_lost_with_garbage_cut_is_inconclusive_not_crash(self):
        craft = self.sa.raw[:400] + b"\xff\xff\xff\xff" + b"~8050D544E00203FD18\r"
        frames = list(parse_stream(craft))
        self.assertTrue(frames)                       # salvaged at all
        lines, code = self.tool.evaluate(frames[0], 1, 0x0D66, self.ref)
        self.assertEqual(code, 6)

    def test_damage_before_parameter_with_proof_is_inconclusive_not_crash(self):
        info_chars = list(self.sa.info_text)
        for i in range(200, 240):
            info_chars[i] = "\xff"
        text = self.sa.raw.decode("latin-1")
        tampered = ("~" + text[1:13] + "".join(info_chars) + text[-5:]).encode("latin-1")
        f = Frame.parse(tampered)                     # parses; checksum now stale
        self.assertFalse(f.checksum_valid)
        span = f.damage_span()
        self.assertEqual(span, (200, 239))            # before the 0x4C region
        lines, code = self.tool.evaluate(f, 0, 3420, self.ref)
        self.assertEqual(code, 6)
        self.assertTrue(any("unreadable" in ln for ln in lines))

    def test_clean_truncation_is_hex_corrupt_exit(self):
        craft = self.sa.raw[:400] + b"~8000D547E00200FD1D\r"
        frames = list(parse_stream(craft))
        lines, code = self.tool.evaluate(frames[0], 1, 0x0D66, self.ref)
        self.assertEqual(code, 7)


if __name__ == "__main__":
    unittest.main()
