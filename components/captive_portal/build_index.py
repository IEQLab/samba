"""Regenerate captive_index.h from captive_index.html: python3 components/captive_portal/build_index.py"""

import gzip
from pathlib import Path

here = Path(__file__).parent
data = gzip.compress((here / "captive_index.html").read_bytes(), compresslevel=9, mtime=0)
rows = [
    "    " + " ".join(f"0x{b:02x}," for b in data[i : i + 15]) for i in range(0, len(data), 15)
]
(here / "captive_index.h").write_text(
    "#pragma once\n"
    "// Generated from captive_index.html by build_index.py; edit the HTML and rerun it\n\n"
    '#include "esphome/core/hal.h"\n\n'
    "namespace esphome::captive_portal {\n\n"
    "constexpr uint8_t INDEX_GZ[] PROGMEM = {\n" + "\n".join(rows) + "\n};\n\n"
    "}  // namespace esphome::captive_portal\n"
)
print(f"captive_index.h: {len(data)} bytes gzipped")
