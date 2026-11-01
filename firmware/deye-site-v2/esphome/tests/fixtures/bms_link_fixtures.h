// Pinned capture fixtures for the bms-link-sniffer host oracle.
// Provenance (do not regenerate from anything else):
//  - REAL_42_RESPONSE / REAL_44_RESPONSE: docs/analysis/bms073-tools/
//    transcript.txt lines [274]/[278], the 2026-10-26 standalone vendor-tool
//    USB capture. Stored checksums (E0E8 / EE0F) recomputed equal — asserting
//    against them pins the checksum code. The transcript renders CR as the
//    four characters \x0d; the real wire CR is appended by the tests.
//  - REAL_ACK / REAL_WRITE_*: verbatim from docs/PROTOCOL_REFERENCE.md —
//    the captured tool-session write (pack 00, 3421 mV) and the executed
//    in-stack write (Link pack 01, 3430 mV).
//  - DAMAGED_FRAME: docs/analysis/bms073-tools/instack_d547_damaged.bin
//    byte-for-byte — the collision-damaged D547 block-read response from
//    the 2026-10-31 in-stack write verification (19 mid-flight garbage INFO
//    chars; the stored CHKSUM matches only the collision-free
//    reconstruction, so the wire bytes must fail the whole-frame check).
//    Non-printable bytes use octal escapes: C hex escapes are greedy and
//    would merge with the ASCII hex payload around them.
#pragma once

// captured bare ack (RTN 00, empty INFO)
static const char* REAL_ACK = "~8000D5000000FD9F";
// same, interior (no SOI/EOI)
static const char* REAL_ACK_INNER = "8000D5000000FD9F";
// captured tool-session write: pack 00, param 4C, 3421 mV
static const char* REAL_WRITE_TOOL = "~8000D5498008004C0D5DFBBE";
// 
static const char* REAL_WRITE_TOOL_INNER = "8000D5498008004C0D5D";
// executed in-stack write: Link pack 01, param 4C, 3430 mV
static const char* REAL_WRITE_INSTACK = "~8000D5498008014C0D66FBCA";
// 
static const char* REAL_WRITE_INSTACK_INNER = "8000D5498008014C0D66";
// captured analog response, 126-char INFO, stored CHK E0E8
static const char* REAL_42_RESPONSE = "~8000D500B07E0000100CF90CFB0CF90CFC0CF90CFD0CFC0D020CF70CFA0CFB0CF00CFB0CFD0CFA0CF0060B420B3A0B400B460B610B62FCEE14C36467046D600713389E018DE0E8";
// captured status response, 82-char INFO, stored CHK EE0F
static const char* REAL_44_RESPONSE = "~8000D5009052000010000000000000000000000000000000000600000000000000000C000000000000000023000001EE0F";
static const char DAMAGED_FRAME[] = "~8000D500A222010DDE0D480C1C0B540E420D480B54760A280320012C1676154012C01130168013EC11300FA000100CD10C9F0A8D0A790D030CD10A790A470CD10C9F0A470A150D030CD10A1509E30AAB0B410CD10C9F0A150A470D670D0309F709B10F5B0EF700690064FF9CFF97006E000F007801F4FF92000FFF8801F4FE640384003C0003000A000A0064000A00036D60631C0014005E05A00C9F0AAB000A0D660032001\377x\270\370A\3600\306\360\3600\360\301\360\360010\36000001900F0003C0EF70E93D8CCADD42710D8F00E100030003C0003000A006400010ADD0B410C6D0C090ADD0A470AAB0C9F0C6D0D7A0D160DDE0D7A0032001E000A0064001E000A271000C8D8F00BB8000A00030AFFFF9FFF06FB0B02002F3037332D424C4330360087C1\015";
