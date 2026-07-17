"""Export an IDA database's function names as a frame-inspector symbol map.

Run inside IDA (File -> Script file..., or the console: `exec(open(r"...").read())`).
Writes `<input>.symbols.txt` next to the analysed binary in the format the
xemu frame inspector (and the call-trace viewer) load:

    HEXADDR HEXSIZE NAME        # one function per line; # / blank lines ignored

Addresses are the XBE virtual addresses, which match the guest EIPs the
inspector records, so no rebasing is needed. Default `sub_*` / `nullsub_*` /
`j_*` names are skipped (they carry no more information than the raw hex), and
MSVC-mangled names are demangled to their short form. Re-run it whenever you
rename functions in IDA and reload the map in the inspector (Load symbols...).
"""
import idautils
import idc
import ida_funcs
import ida_name
import os
import re

_DEFAULT = re.compile(r'^(sub_|nullsub_|j_|unknown_|loc_|def_|SEH_|__imp_)')


def _clean(name):
    dn = idc.demangle_name(name, idc.get_inf_attr(idc.INF_SHORT_DN))
    if dn:
        name = dn
    for cc in ('__thiscall ', '__cdecl ', '__stdcall ', '__fastcall '):
        name = name.replace(cc, '')
    paren = name.find('(')
    if paren > 0:
        name = name[:paren]
    return ' '.join(name.split())


def export(out_path=None):
    rows = []
    for ea in idautils.Functions():
        f = ida_funcs.get_func(ea)
        if not f:
            continue
        raw = ida_name.get_ea_name(ea, ida_name.GN_VISIBLE) or idc.get_func_name(ea)
        if not raw or _DEFAULT.match(raw):
            continue
        rows.append((ea, f.end_ea - f.start_ea, _clean(raw)))
    rows.sort()
    if out_path is None:
        out_path = os.path.dirname(idc.get_input_file_path()) or os.getcwd()
        out_path = os.path.join(
            out_path, os.path.basename(idc.get_input_file_path()) + '.symbols.txt')
    with open(out_path, 'w', encoding='utf-8') as fh:
        fh.write('# %s symbol map (addr size name), exported from IDA\n'
                 % os.path.basename(idc.get_input_file_path()))
        for ea, size, name in rows:
            fh.write('%08X %X %s\n' % (ea & 0xFFFFFFFF, size, name))
    print('frame-inspector: wrote %d symbols to %s' % (len(rows), out_path))
    return out_path


if __name__ == '__main__':
    export()
