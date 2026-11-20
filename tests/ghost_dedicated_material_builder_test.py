"""Offline smoke tests for the UE4 editor material recipe and PAK builder.

This exercises mocked editor calls, not Unreal Engine shader cooking.
"""
import ast
import contextlib
import io
from pathlib import Path
import runpy
import sys
import tempfile
import types

root = Path(__file__).resolve().parents[1]
creator = root / 'tools/ghost_material/create_ghost_material.py'
builder = root / 'tools/ghost_material/make_ghost_pak.py'
for path in (creator, builder):
    ast.parse(path.read_text(encoding='utf-8'))


class Asset:
    def __init__(self):
        self.fields = {}

    def set_editor_property(self, key, value):
        self.fields[key] = value


class Library:
    @staticmethod
    def does_asset_exist(path):
        assert path == '/Game/RefunctUtility/M_GhostTranslucent'
        return False

    @staticmethod
    def save_loaded_asset(asset):
        assert asset.fields['blend_mode'] == 'translucent'
        return True


class Editing:
    links = []
    expressions = []

    @staticmethod
    def create_material_expression(material, cls, x, y):
        expression = Asset()
        Editing.expressions.append((material, cls, x, y, expression))
        return expression

    @staticmethod
    def connect_material_property(node, pin, prop):
        Editing.links.append((node, pin, prop))
        return True

    @staticmethod
    def recompile_material(material):
        assert material.fields['two_sided'] is True


material = Asset()
asset_tools = types.SimpleNamespace(create_asset=lambda name, folder, cls, factory: material)
mock_unreal = types.SimpleNamespace(
    EditorAssetLibrary=Library,
    AssetToolsHelpers=types.SimpleNamespace(get_asset_tools=lambda: asset_tools),
    MaterialFactoryNew=lambda: object(),
    Material=Asset,
    MaterialDomain=types.SimpleNamespace(MD_SURFACE='surface'),
    BlendMode=types.SimpleNamespace(BLEND_TRANSLUCENT='translucent'),
    MaterialShadingModel=types.SimpleNamespace(MSM_UNLIT='unlit'),
    MaterialEditingLibrary=Editing,
    MaterialExpressionScalarParameter='scalar',
    MaterialExpressionVectorParameter='vector',
    MaterialProperty=types.SimpleNamespace(MP_EMISSIVE_COLOR='emissive', MP_OPACITY='opacity'),
    LinearColor=lambda r,g,b,a: (r,g,b,a),
    log=lambda _: None,
)
with contextlib.redirect_stdout(io.StringIO()):
    old = sys.modules.get('unreal')
    sys.modules['unreal'] = mock_unreal
    try:
        runpy.run_path(str(creator), run_name='__main__')
    finally:
        if old is None:
            del sys.modules['unreal']
        else:
            sys.modules['unreal'] = old
assert material.fields['material_domain'] == 'surface'
assert material.fields['shading_model'] == 'unlit'
assert material.fields['blend_mode'] == 'translucent'
assert material.fields['two_sided'] is True
assert {node.fields['parameter_name']: node.fields['default_value']
        for *_, node in Editing.expressions} == {
    'GhostTint': (0.27, 0.92, 1.0, 1.0),
    'GhostOpacity': 0.36,
}
assert {prop for _, _, prop in Editing.links} == {'emissive', 'opacity'}

with tempfile.TemporaryDirectory(prefix='ghost_pak_builder_') as tmp:
    here = Path(tmp)
    cooked = here / 'Saved/Cooked/WindowsNoEditor/Refunct/Content/RefunctUtility'
    cooked.mkdir(parents=True)
    for ext in ('.uasset', '.uexp'):
        (cooked / ('M_GhostTranslucent' + ext)).write_bytes(b'fake cooked asset')
    paktool = here / 'fake_unrealpak.py'
    paktool.write_text('#!/usr/bin/env python3\n'
                       'from pathlib import Path\n'
                       'import sys\n'
                       'response = Path(sys.argv[2].removeprefix("-Create="))\n'
                       'content = response.read_text()\n'
                       'assert "../../../Refunct/Content/RefunctUtility/" in content\n'
                       'assert "M_GhostTranslucent.uasset" in content\n'
                       'assert "M_GhostTranslucent.uexp" in content\n'
                       'Path(sys.argv[1]).write_bytes(b"pretend pak")\n')
    paktool.chmod(0o755)
    output = here / 'GhostMaterials_P.pak'
    previous = sys.argv
    sys.argv = [str(builder), '--cooked-content', str(cooked.parent),
                '--unrealpak', str(paktool), '--output', str(output)]
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            runpy.run_path(str(builder), run_name='__main__')
    except SystemExit as e:
        assert e.code == 0
    finally:
        sys.argv = previous
    assert output.read_bytes() == b'pretend pak'

print('PASS: cyan/alpha material editor graph, cooking manifest, fail-safe PAK output')
