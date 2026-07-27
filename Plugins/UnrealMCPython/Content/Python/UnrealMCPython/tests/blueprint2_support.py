"""Shared helpers for Universal Blueprint 2 in-editor tests."""

import asyncio
import importlib
import inspect
import json


def call_action(module_name, function_name, **kwargs):
    module = importlib.import_module(f"UnrealMCPython.{module_name}")
    importlib.reload(module)
    result = getattr(module, function_name)(**kwargs)
    if inspect.isawaitable(result):
        result = asyncio.run(result)
    return json.loads(result)
