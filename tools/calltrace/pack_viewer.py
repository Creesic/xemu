#!/usr/bin/env python3
"""Bake a .xct recording into a self-contained, self-opening viewer HTML.

Usage:
    python pack_viewer.py <recording.xct> [out.html]

Produces a single HTML file with the recording embedded as base64. Open it
in any browser (double-click) and it renders the graph immediately, with no
file to load. The source viewer.html is unchanged and still works as a
file-loading viewer.
"""
import base64
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VIEWER = os.path.join(HERE, 'viewer.html')
# Matches: const EMBEDDED_XCT = /*__XCT__*/""/*__XCT__*/;
PLACEHOLDER = re.compile(r'/\*__XCT__\*/.*?/\*__XCT__\*/', re.DOTALL)


def main(xct_path, out_path):
    with open(xct_path, 'rb') as f:
        b64 = base64.b64encode(f.read()).decode('ascii')
    with open(VIEWER, 'r', encoding='utf-8') as f:
        html = f.read()
    replacement = '/*__XCT__*/"%s"/*__XCT__*/' % b64
    html, n = PLACEHOLDER.subn(replacement, html, count=1)
    if n != 1:
        sys.exit('error: could not find the __XCT__ placeholder in viewer.html')
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(html)
    kb = os.path.getsize(out_path) / 1024
    print('wrote %s (%.0f KB)' % (out_path, kb))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    xct = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.splitext(os.path.basename(xct))[0] + '.html'
    main(xct, out)
