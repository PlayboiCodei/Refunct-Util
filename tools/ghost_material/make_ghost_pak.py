"""Build a .pak from COOKED UE4 material files using matching UnrealPak.exe.

System-Python helper; fails safely without cooked assets / UnrealPak.
It never writes directly into the game's directory.
"""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

MATERIAL_NAME = 'M_GhostTranslucent'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cooked-content', type=Path, required=True,
                        help='.../Saved/Cooked/WindowsNoEditor/<YourProject>/Content')
    parser.add_argument('--unrealpak', type=Path, required=True,
                        help='UnrealPak.exe from the exact UE4 build used to cook')
    parser.add_argument('--output', type=Path, required=True,
                        help='Output file ending in .pak; never the original game .pak')
    args = parser.parse_args(argv)
    if not args.unrealpak.is_file():
        parser.error(f'UnrealPak.exe not found: {args.unrealpak}')
    if args.output.suffix.lower() != '.pak':
        parser.error('Output must have a .pak extension')
    if args.output.exists():
        parser.error(f'Refusing to overwrite existing output: {args.output}')
    target = args.cooked_content / 'RefunctUtility'
    source_asset = target / f'{MATERIAL_NAME}.uasset'
    if not source_asset.is_file():
        parser.error('Cooked M_GhostTranslucent.uasset was not found in ' + str(target))

    files = [source_asset]
    for extension in ('.uexp', '.ubulk'):
        sidecar = target / f'{MATERIAL_NAME}{extension}'
        if sidecar.is_file():
            files.append(sidecar)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # UnrealPak's response-file format: quoted source path + quoted mount path.
    # No files from the base game are modified. The mount prefix must match
    # the original game project, not the temporary editor project's name.
    with tempfile.TemporaryDirectory(prefix='refunct_ghost_pak_') as temp:
        response = Path(temp) / 'ghost_files.txt'
        response.write_text(''.join(
            f'"{f.resolve()}" "../../../Refunct/Content/RefunctUtility/{f.name}"\n'
            for f in files
        ), encoding='utf-8')
        result = subprocess.run(
            [str(args.unrealpak), str(args.output.resolve()), '-Create=' + str(response)],
            check=False
        )
        if result.returncode:
            parser.error(f'UnrealPak failed: exit {result.returncode}')
    if not args.output.is_file() or args.output.stat().st_size == 0:
        parser.error('UnrealPak reported success but produced no PAK')
    print('Created:', args.output)
    print('Contains:', ', '.join(f.name for f in files))
    print('NOT installed: mount/auto-load support must be validated for Refunct')
    return 0


if __name__ == '__main__':
    sys.exit(main())
