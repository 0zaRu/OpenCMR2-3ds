#!/usr/bin/env python3
"""Build a Windows x64 USB folder or release ZIP. Requires Python pefile.

Original game data is optional and only added locally with --data; CI never
includes it. All non-system DLL imports (including transitive imports) must
resolve to x64 files before a package is created.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

import pefile

ROOT = Path(__file__).resolve().parents[1]
SYSTEM_DLLS = set("""advapi32 bcrypt cfgmgr32 comdlg32 crypt32 d3d11 d3d12
    dbghelp dwmapi dxgi gdi32 hid imm32 iphlpapi kernel32 msvcrt ntdll ole32
    oleaut32 powrprof psapi rpcrt4 secur32 setupapi shell32 shlwapi ucrtbase
    user32 userenv version winhttp winmm winspool wintrust ws2_32 wtsapi32
    xinput1_4""".split())


def imports(path):
    with pefile.PE(str(path)) as pe:
        if pe.FILE_HEADER.Machine != 0x8664:
            raise ValueError(f"Not a Windows x64 executable/DLL: {path}")
        return [entry.dll.decode("ascii") for attribute in
                ("DIRECTORY_ENTRY_IMPORT", "DIRECTORY_ENTRY_DELAY_IMPORT")
                for entry in getattr(pe, attribute, [])]


def dependencies(binary, directories):
    available = {}
    for directory in directories:
        for path in directory.glob("*"):
            if path.is_file() and path.suffix.lower() == ".dll":
                available.setdefault(path.name.lower(), path)
    files = {binary.name.lower(): binary}
    pending = [binary]
    while pending:
        for name in imports(pending.pop()):
            key = name.lower()
            if key in files or key.removesuffix(".dll") in SYSTEM_DLLS or \
                    key.startswith(("api-ms-win-", "ext-ms-win-")):
                continue
            if key not in available:
                raise ValueError(f"Missing runtime DLL {name}; add its directory with --dll-dir")
            files[key] = available[key]
            pending.append(available[key])
    return list(files.values())


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--dll-dir", action="append", default=[], type=Path)
    parser.add_argument("--license", action="append", default=[], type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--data", type=Path)
    parser.add_argument("--zip", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    output = args.output.resolve()
    if output.exists():
        parser.error(f"Output already exists: {output}; choose a new folder")
    if args.zip and args.zip.exists():
        parser.error(f"ZIP already exists: {args.zip}")
    data = args.data.resolve(strict=True) if args.data else None
    if data and output.is_relative_to(data):
        parser.error("Output must be outside the original data directory")
    runtime = dependencies(binary, [binary.parent, *args.dll_dir])
    for license_path in args.license:
        if not license_path.is_file():
            parser.error(f"Missing license: {license_path}")
    output.mkdir(parents=True)
    for path in runtime:
        shutil.copy2(path, output / path.name)
    if data:
        shutil.copytree(data, output / "Data", symlinks=False)
    else:
        (output / "Data").mkdir()
    (output / "UserData").mkdir()
    licenses = output / "Licenses"
    licenses.mkdir()
    shutil.copy2(ROOT / "LICENSE", licenses / "OpenCMR2-LICENSE")
    shutil.copy2(ROOT / "third_party/enet/LICENSE", licenses / "ENet-LICENSE")
    for index, path in enumerate(args.license):
        shutil.copy2(path, licenses / f"{index + 1}-{path.name}")
    launcher = '@echo off\nsetlocal\ncd /d "%~dp0"\n' \
        'if not exist "UserData" mkdir "UserData"\n' \
        'opencmr2.exe --data "%~dp0Data" --user-dir "%~dp0UserData" ' \
        '> "UserData\\console.log" 2>&1\n' \
        'if errorlevel 1 (\n' \
        '  echo OpenCMR2 no pudo iniciarse. Mira UserData\\console.log y opencmr2.log.\n' \
        '  pause\n)\n'
    (output / "Jugar.cmd").write_bytes(launcher.replace("\n", "\r\n").encode("ascii"))
    (output / "LEEME.txt").write_text(
        "OpenCMR2 - Windows x64 portable\n\n"
        "Copia esta carpeta completa al USB y ejecuta Jugar.cmd.\n"
        "Windows 10/11 de 64 bits y un controlador grafico con Vulkan.\n"
        "Si Data esta vacia, copia dentro los archivos de tu juego original\n"
        "(GAME, FRONTEND, sonidos y demas carpetas). No se distribuyen con las releases.\n"
        "Partidas, ajustes y registros se guardan en UserData junto al ejecutable.\n"
        "Puedes mover la carpeta o cambiar la letra de la unidad.\n\n"
        "SDL3 y las DLL necesarias van incluidas; no requiere instalador.\n"
        "Codigo fuente: https://github.com/pablocpas/OpenCMR2\n",
        encoding="utf-8")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"],
                                        cwd=ROOT, text=True))
    manifest = {"source_revision": revision, "source_dirty": dirty,
                "architecture": "Windows x86-64", "game_data_included": bool(data),
                "files": {p.relative_to(output).as_posix(): sha256(p)
                          for p in sorted(output.rglob("*")) if p.is_file()}}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if args.zip:
        args.zip.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(args.zip, "w", zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(output.rglob("*")):
                archive.write(path, path.relative_to(output.parent))
        args.zip.with_suffix(args.zip.suffix + ".sha256").write_text(
            f"{sha256(args.zip)}  {args.zip.name}\n")
    print(f"Package: {output}; {len(runtime) - 1} runtime DLLs; "
          f"{len(manifest['files'])} files verified by SHA-256")


if __name__ == "__main__":
    main()
