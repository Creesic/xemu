#!/usr/bin/env python3
"""Apply xemu's reviewed one-line Plume D3D12 build overlay."""

from pathlib import Path
import sys


source = Path(sys.argv[1]).read_text(encoding="utf-8")
old = "static const uint32_t SamplerDescriptorHeapSize = 1024;"
new = "static const uint32_t SamplerDescriptorHeapSize = 2048;"
if source.count(old) != 1:
    raise SystemExit("unexpected Plume sampler heap declaration")
Path(sys.argv[2]).write_text(source.replace(old, new), encoding="utf-8")
