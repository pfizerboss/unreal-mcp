"""Shared helpers for Universal Blueprint 2 in-editor tests."""

import importlib
import json
import uuid


BLUEPRINT2_TEST_ROOT = f"/Game/__MCPTests/Blueprint2_{uuid.uuid4().hex}"


def call_action(module_name, function_name, **kwargs):
    module = importlib.import_module(f"UnrealMCPython.{module_name}")
    importlib.reload(module)
    result = getattr(module, function_name)(**kwargs)
    return json.loads(result)
