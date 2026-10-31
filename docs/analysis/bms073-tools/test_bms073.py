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


if __name__ == "__main__":
    unittest.main()
