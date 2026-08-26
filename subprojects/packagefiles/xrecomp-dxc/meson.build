project(
  'xrecomp-dxc',
  meson_version: '>=1.5.0',
  version: '1.9.2602.24',
  license: ['NCSA', 'MIT', 'LicenseRef-Microsoft'],
)

xrecomp_dxc_root = meson.current_source_dir()
xrecomp_dxc_arch = {
  'x86': 'x86',
  'x86_64': 'x64',
  'aarch64': 'arm64',
}.get(host_machine.cpu_family(), '')
if xrecomp_dxc_arch == ''
  error('DXC does not support Windows host architecture ' + host_machine.cpu_family())
endif
xrecomp_dxc_payloads = files(
  'bin' / xrecomp_dxc_arch / 'dxc.exe',
  'bin' / xrecomp_dxc_arch / 'dxcompiler.dll',
  'bin' / xrecomp_dxc_arch / 'dxil.dll',
  'LICENSE-LLVM.txt',
  'LICENSE-MIT.txt',
  'LICENSE-MS.txt',
)
