#!/usr/bin/env python3

import argparse
import hashlib
from pathlib import Path


FILES = {
    "_XRECOMP_DXC_EXE": "dxc.exe",
    "_XRECOMP_DXC_COMPILER_DLL": "dxcompiler.dll",
    "_XRECOMP_DXC_VALIDATOR_DLL": "dxil.dll",
}
LICENSES = {
    "_XRECOMP_DXC_LICENSE_LLVM": "LICENSE-LLVM.txt",
    "_XRECOMP_DXC_LICENSE_MIT": "LICENSE-MIT.txt",
    "_XRECOMP_DXC_LICENSE_MS": "LICENSE-MS.txt",
}


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def write_if_changed(path: Path, content: str) -> None:
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def configure(template: Path, values: dict[str, str]) -> str:
    content = template.read_text(encoding="utf-8")
    for name, value in values.items():
        content = content.replace(f"@{name}@", value)
    return content


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--manifest-template", type=Path, required=True)
    parser.add_argument("--resource-template", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--release-tag", required=True)
    parser.add_argument("--archive-url", required=True)
    parser.add_argument("--archive-sha256", required=True)
    args = parser.parse_args()

    binary_dir = args.root / "bin" / args.arch
    paths = {name: binary_dir / filename for name, filename in FILES.items()}
    paths.update({name: args.root / filename for name, filename in LICENSES.items()})
    missing = [str(path) for path in paths.values() if not path.is_file()]
    if missing:
        raise SystemExit("Official DXC archive is missing: " + ", ".join(missing))

    values = {
        "XRECOMP_DXC_RELEASE_TAG": args.release_tag,
        "XRECOMP_DXC_ARCHIVE_URL": args.archive_url,
        "XRECOMP_DXC_ARCHIVE_SHA256": args.archive_sha256,
        "XRECOMP_DXC_BUNDLE_ID": (
            f"{args.release_tag}-{args.arch}-{args.archive_sha256}"[:32]
        ),
    }
    for name, path in paths.items():
        values[f"{name}_RC"] = path.as_posix()
    for name in FILES:
        path = paths[name]
        manifest_name = name.removeprefix("_")
        values[f"{manifest_name}_SIZE"] = str(path.stat().st_size)
        values[f"{manifest_name}_SHA256"] = digest(path)

    write_if_changed(
        args.output_dir / "plume_embedded_dxc_manifest.h",
        configure(args.manifest_template, values),
    )
    write_if_changed(
        args.output_dir / "plume_embedded_dxc.rc",
        configure(args.resource_template, values),
    )


if __name__ == "__main__":
    main()
