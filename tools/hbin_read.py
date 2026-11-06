"""Minimal reader for the cecon .hbin columnar format (for validation/inspection).

Layout (see Frame::write_hbin / to_hbin.py):
  8 bytes  magic "CECONHB1"
  u64      header_len
  bytes    header JSON {magic, version, n_rows, columns:[{name,dtype,offset,nbytes,[levels]}]}
  pad to 64-byte boundary  -> data_start
  per column: blob at data_start + offset, nbytes long (f64 = float64, factor = int32 codes)
"""
import json
import struct

import numpy as np


def read_hbin(path):
    """Return (n_rows, dict name->np.ndarray). Factor columns return int32 codes;
    their level dictionary is attached as arr.levels via a wrapper dict entry
    name+'__levels'."""
    with open(path, "rb") as f:
        raw = f.read()
    assert raw[:8] == b"CECONHB1", "bad magic"
    (hlen,) = struct.unpack_from("<Q", raw, 8)
    header = json.loads(raw[16:16 + hlen].decode("utf-8"))
    align = 64
    data_start = ((16 + hlen) + align - 1) // align * align
    n = header["n_rows"]
    out = {}
    for c in header["columns"]:
        off = data_start + c["offset"]
        if c["dtype"] == "f64":
            out[c["name"]] = np.frombuffer(raw, dtype="<f8", count=n, offset=off)
        else:  # factor
            out[c["name"]] = np.frombuffer(raw, dtype="<i4", count=n, offset=off)
            out[c["name"] + "__levels"] = [_level(l) for l in c.get("levels", [])]
    return n, out


def _level(text):
    """A level the writer escaped (hbin_level_escape, src/io/columnar.h): a leading
    NUL, then the level's bytes in hex. Bytes that are not UTF-8 come back as
    surrogate escapes."""
    if not text.startswith("\0"):
        return text
    return bytes.fromhex(text[1:]).decode("utf-8", "surrogateescape")


if __name__ == "__main__":
    import sys
    n, cols = read_hbin(sys.argv[1])
    print(f"n_rows={n}")
    for k, v in cols.items():
        if k.endswith("__levels"):
            continue
        print(f"  {k}: {v.dtype} [{v[:3]}...]")
