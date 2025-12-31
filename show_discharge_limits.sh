#!/usr/bin/env bash
set -euo pipefail
LOG="${1:-can.log}"

awk '
function hexval(c) {
  c = tolower(c)
  if (c >= "0" && c <= "9") return c + 0
  if (c == "a") return 10
  if (c == "b") return 11
  if (c == "c") return 12
  if (c == "d") return 13
  if (c == "e") return 14
  if (c == "f") return 15
  return 0
}
function hexbyte(h, pos,   c1, c2) {
  c1 = substr(h, pos, 1)
  c2 = substr(h, pos+1, 1)
  return hexval(c1)*16 + hexval(c2)
}
function le16(h, pos) {
  # little-endian u16 from hex string h at position pos (1-based), 2 bytes
  lo = hexbyte(h, pos)
  hi = hexbyte(h, pos+2)
  return lo + 256*hi
}

$3 ~ /^351#/ {
  # timestamp is field 1 like: (1767032760.914068)
  ts = $1
  gsub(/^\(/, "", ts)
  gsub(/\)$/, "", ts)

  hex = substr($3, 5)  # after "351#"

  vchg = le16(hex, 1)  / 10.0
  ichg = le16(hex, 5)  / 10.0
  idis = le16(hex, 9)  / 10.0
  vlow = le16(hex, 13) / 10.0

  # print only when discharge limit changes (or first time)
  if (idis != last_idis || last_idis == "") {
    printf "%s  Idis_lim=%6.1fA  (Ichg_lim=%6.1fA  Vchg_max=%5.1fV  Vlow=%5.1fV)  raw=%s\n",
           ts, idis, ichg, vchg, vlow, hex
    last_idis = idis
  }
}
' "$LOG"

