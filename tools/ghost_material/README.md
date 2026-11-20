# Refunct Utility v1.0.0 — dedicated 3D ghost material (PREPARATION)

## Status

This is **not yet a finished transparency patch**. The working game is currently
stable, and the correct approach is to supply a *real cooked Unreal Engine 4
surface material*, rather than editing materials loaded in the Shipping process.

The optional `src/ghost.cpp` update only looks for an **already-loaded** UObject
named `M_GhostTranslucent` that is a `Material`. It never scans material parameter
arrays or modifies shader internals. When the asset is absent, the ghost remains
**opaque, visible, movable, and without collision**, as before.

A `.pak` containing the material **does not guarantee the asset is loaded into
memory**. The automatic loading/mounting step must be validated against the
actual game build before the ghost can use it. This is why this package does
not claim to implement visible transparency today.

## 1. Identify the matching UE4 build

Determine the exact Unreal Engine version/build used by the installed
`Refunct-Win32-Shipping.exe` and original `Refunct-WindowsNoEditor.pak`.
**Do not infer the engine version from Refunct's 2015 release year.**

A cooked Material contains shader data specific to an Unreal Engine target
version and rendering platform. A material cooked with a mismatched editor may
fail to load or crash the game. Preserve the original `.pak` and `.exe`.

## 2. Create the material

Use a blank UE4 project with a compatible editor/build. The project may be
named `Refunct` to match the eventual mount path. If that UE4 editor provides
the Python Editor Script Plugin and MaterialEditingLibrary:

1. Enable **Python Editor Script Plugin** and **Editor Scripting Utilities**.
2. Open **Window > Developer Tools > Output Log**, switch the command input
   to Python (or use the Python console/menu available in that UE4 version).
3. Run (adjust the path):

   ```python
   exec(open(r'C:\RefunctUtil V4\tools\ghost_material\create_ghost_material.py', encoding='utf-8').read())
   ```

This creates `/Game/RefunctUtility/M_GhostTranslucent`, with:

- Material Domain **Surface**
- Blend Mode **Translucent**
- Shading Model **Unlit**
- Two Sided **On**
- Vector Parameter `GhostTint`: `(0.27, 0.92, 1.0, 1.0)` into **Emissive Color**
- Scalar Parameter `GhostOpacity`: `0.36` into **Opacity**

For early UE4 versions that do not have the Python editor scripting APIs,
create the same graph manually in the Material Editor instead. Save the asset
at the same `/Game/RefunctUtility/` path.

## 3. Cook and package (requires the correct UE4 Editor)

Cook project content for the **Windows target supported by Refunct's UE4
build**. Ensure you find `M_GhostTranslucent.uasset` (and any `.uexp` or
`.ubulk` sidecars) under a cooked Content directory.

Then run with normal system Python on Windows (all paths are examples):

```powershell
py -3 tools\ghost_material\make_ghost_pak.py `
  --cooked-content "C:\UEProjects\Refunct\Saved\Cooked\WindowsNoEditor\Refunct\Content" `
  --unrealpak "C:\UE4\Engine\Binaries\Win64\UnrealPak.exe" `
  --output "C:\Temp\GhostMaterials_P.pak"
```

The script refuses to overwrite an existing output and **does not modify or
install any game files**. Review the generated archive's mount path and
compatibility before installing it. The format of UE4 PAKs and the target
cooking tool may differ depending on the exact game build.

## 4. What remains to implement before real in-game transparency

- Confirm the game's exact UE4 version and Win32 shader target from its
  executable/content archive.
- Validate that the material is cooked with compatible shader bytecode.
- Validate how Refunct mounts a new `.pak` and loads a newly introduced asset
  into the UObject registry. The current loader intentionally **does not**
  guess or call unverified native asset-loading functions.
- Once the material appears as a loaded `M_GhostTranslucent` UObject, the new
  `src/ghost.cpp` will apply it using validated component calls.
- Test in-game before changing the old opaque fallback or exposing opacity
  controls in the menu.

## Files in the patch

- Replace: `src/ghost.cpp` (optional exact-name material adapter; opaque fallback)
- Add: `tests/ghost_dedicated_material_safe_runtime_test.py`
- Add: `tests/ghost_dedicated_material_builder_test.py`
- Add: `tools/ghost_material/create_ghost_material.py`
- Add: `tools/ghost_material/make_ghost_pak.py`
- Add: `tools/ghost_material/README.md`
- Delete: nothing

Build after replacing `src/ghost.cpp` with `build_win32.bat`. Windows x86
compilation and actual UE4 asset loading were **not tested in this environment**.

Do not install `GhostMaterials_P.pak` or guess loading offsets until the build
compatibility is verified. The existing stable `.asi` is still the recommended
in-game version until that point.
