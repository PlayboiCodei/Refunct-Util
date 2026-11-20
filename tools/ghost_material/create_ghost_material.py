"""Run INSIDE a UE4 Editor with the Python Editor Script Plugin enabled.

Creates /Game/RefunctUtility/M_GhostTranslucent as a real translucent,
unlit surface material. This script does NOT cook a PAK or modify Refunct.
The UE4 Editor used for cooking must match the game's build.

Examples (run in the UE4 Editor Python console):
    exec(open(r'C:\\path\\to\\create_ghost_material.py', encoding='utf-8').read())
"""

try:
    import unreal
except ImportError as exc:
    raise RuntimeError(
        "Run this inside the Unreal Editor Python console, not system Python."
    ) from exc

ASSET_FOLDER = "/Game/RefunctUtility"
ASSET_NAME = "M_GhostTranslucent"
ASSET_PATH = f"{ASSET_FOLDER}/{ASSET_NAME}"


def set_property(object_, key, value):
    try:
        object_.set_editor_property(key, value)
    except Exception as exc:
        raise RuntimeError(
            f"The current Unreal Editor does not expose '{key}'. "
            "Use an editor compatible with Refunct and create the material manually "
            "using the supplied instructions."
        ) from exc


def build_material():
    library = unreal.EditorAssetLibrary
    if library.does_asset_exist(ASSET_PATH):
        raise RuntimeError(
            f"{ASSET_PATH} already exists. Refusing to overwrite an asset."
        )

    factory = unreal.MaterialFactoryNew()
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        ASSET_NAME, ASSET_FOLDER, unreal.Material, factory
    )
    if material is None:
        raise RuntimeError(f"Failed to create {ASSET_PATH}")

    set_property(material, "material_domain", unreal.MaterialDomain.MD_SURFACE)
    set_property(material, "blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    set_property(material, "two_sided", True)
    set_property(material, "shading_model", unreal.MaterialShadingModel.MSM_UNLIT)

    # A bright, cyan hologram. Unlike an opaque material, the compiled
    # BlendMode here actually enables alpha blending on the StaticMeshActor.
    editing = unreal.MaterialEditingLibrary
    tint = editing.create_material_expression(
        material, unreal.MaterialExpressionVectorParameter, -440, -150
    )
    set_property(tint, "parameter_name", "GhostTint")
    set_property(tint, "default_value", unreal.LinearColor(0.27, 0.92, 1.0, 1.0))
    alpha = editing.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -440, 50
    )
    set_property(alpha, "parameter_name", "GhostOpacity")
    set_property(alpha, "default_value", 0.36)

    if not editing.connect_material_property(
        tint, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR
    ):
        raise RuntimeError("Could not connect GhostTint to Emissive Color")
    if not editing.connect_material_property(
        alpha, "", unreal.MaterialProperty.MP_OPACITY
    ):
        raise RuntimeError("Could not connect GhostOpacity to Opacity")

    editing.recompile_material(material)
    if not library.save_loaded_asset(material):
        raise RuntimeError("Material created but saving failed")

    unreal.log(
        "Refunct Utility: created translucent material "
        + ASSET_PATH
        + " (cyan, opacity=0.36). Cook it for Windows, then build a matching .pak."
    )
    return material


build_material()
