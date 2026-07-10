#!/usr/bin/env python3
"""Bake a .xct recording into a self-contained, self-opening viewer HTML.

Usage:
    python pack_viewer.py <recording.xct> [out.html] [--symbols map.txt]

Produces a single HTML file with the recording embedded as base64. Open it
in any browser (double-click) and it renders the graph immediately, with no
file to load. With --symbols, a symbol map (see make_symbols.py) is embedded
too and applied on open, so the graph shows real function names. The source
viewer.html is unchanged and still works as a file-loading viewer.
"""
import base64
import os
import re
import sys


HERE = os.path.dirname(os.path.abspath(__file__))
VIEWER = os.path.join(HERE, 'viewer.html')


def embed(html, marker, data_bytes):
    # Matches: /*__MARKER__*/ ... /*__MARKER__*/
    pat = re.compile(r'/\*__%s__\*/.*?/\*__%s__\*/' % (marker, marker),
                     re.DOTALL)
    b64 = base64.b64encode(data_bytes).decode('ascii')
    repl = '/*__%s__*/"%s"/*__%s__*/' % (marker, b64, marker)
    html, n = pat.subn(repl, html, count=1)
    if n != 1:
        sys.exit('error: could not find the __%s__ placeholder' % marker)
    return html


def main(xct_path, out_path, symbols_path):
    with open(VIEWER, 'r', encoding='utf-8') as f:
        html = f.read()
    with open(xct_path, 'rb') as f:
        html = embed(html, 'XCT', f.read())
    if symbols_path:
        with open(symbols_path, 'rb') as f:
            html = embed(html, 'SYMS', f.read())
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(html)
    kb = os.path.getsize(out_path) / 1024
    print('wrote %s (%.0f KB)%s' %
          (out_path, kb, ' with symbols' if symbols_path else ''))


if __name__ == '__main__':
    args = sys.argv[1:]
    symbols = None
    if '--symbols' in args:
        i = args.index('--symbols')
        symbols = args[i + 1]
        del args[i:i + 2]
    if not args:
        sys.exit(__doc__)
    xct = args[0]
    out = args[1] if len(args) > 1 else \
        os.path.splitext(os.path.basename(xct))[0] + '.html'
    main(xct, out, symbols)
