"""
Run all MCP unittest suites inside the Unreal Python environment.

Usage — single line in the Unreal Python console (Output Log):

    import runpy; runpy.run_module("UnrealMCPython.tests.run_all", run_name="__main__")

Or via MCP execute_python tool (multi-line is fine there):

    import runpy
    runpy.run_module("UnrealMCPython.tests.run_all", run_name="__main__")
"""
import sys
import importlib
import unittest

import unreal

_MODULES = [
    "UnrealMCPython.tests.test_util",
    "UnrealMCPython.tests.test_workflow",
    "UnrealMCPython.tests.test_actor",
    "UnrealMCPython.tests.test_anim_blueprint",
    "UnrealMCPython.tests.test_animation",
    "UnrealMCPython.tests.test_asset",
    "UnrealMCPython.tests.test_level",
    "UnrealMCPython.tests.test_level_sequence",
    "UnrealMCPython.tests.test_material",
    "UnrealMCPython.tests.test_blueprint",
    "UnrealMCPython.tests.test_blueprint2_inspection",
    "UnrealMCPython.tests.test_blueprint2_members",
    "UnrealMCPython.tests.test_blueprint2_graph",
    "UnrealMCPython.tests.test_blueprint2_palette",
    "UnrealMCPython.tests.test_blueprint2_semantic",
    "UnrealMCPython.tests.test_blueprint2_diagnostics",
    "UnrealMCPython.tests.test_behavior_tree",
    "UnrealMCPython.tests.test_data_table",
    "UnrealMCPython.tests.test_umg",
    "UnrealMCPython.tests.test_editor",
    "UnrealMCPython.tests.test_game",
    "UnrealMCPython.tests.test_static_mesh",
    "UnrealMCPython.tests.test_layer",
    "UnrealMCPython.tests.test_texture",
    "UnrealMCPython.tests.test_retarget",
    "UnrealMCPython.tests.test_control_rig",
    "UnrealMCPython.tests.test_gas",
    "UnrealMCPython.tests.test_vision",
]

suite = unittest.TestSuite()
loader = unittest.TestLoader()
_load_errors = []

for mod_name in _MODULES:
    try:
        mod = importlib.import_module(mod_name)
        importlib.reload(mod)
        suite.addTests(loader.loadTestsFromModule(mod))
    except Exception as e:
        message = f"[LOAD ERROR] {mod_name}: {e}"
        print(message)
        _load_errors.append(message)

if _load_errors:
    raise RuntimeError(
        "In-editor test suites failed to load:\n" + "\n".join(_load_errors)
    )

runner = unittest.TextTestRunner(verbosity=2, stream=sys.stdout)
result = runner.run(suite)

remaining_assets = []
if unreal.EditorAssetLibrary.does_directory_exist("/Game/__MCPTests"):
    remaining_assets = unreal.EditorAssetLibrary.list_assets(
        "/Game/__MCPTests",
        recursive=True,
        include_folder=False,
    )

total = result.testsRun
fails = len(result.failures)
errors = len(result.errors)
skipped = len(result.skipped)
passed = total - fails - errors - skipped

print(f"\n{'='*60}")
print(f"Results: {passed} passed | {fails} failed | {errors} errors | {skipped} skipped / {total} total")
print(f"remaining_assets={len(remaining_assets)}")
print('='*60)

if not result.wasSuccessful():
    raise RuntimeError(
        f"In-editor suite failed: {fails} failures, {errors} errors"
    )

if remaining_assets:
    raise RuntimeError(
        f"In-editor test assets remain under /Game/__MCPTests: {remaining_assets}"
    )
