"""Offline proof that risky no-argument editor actions cannot mutate by default."""

import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import sys

import pytest


ACTION_ROOT = (
    Path(__file__).parents[2]
    / "Plugins"
    / "UnrealMCPython"
    / "Content"
    / "Python"
    / "UnrealMCPython"
)


def _load_action_module(monkeypatch, domain, unreal):
    monkeypatch.setitem(sys.modules, "unreal", unreal)
    path = ACTION_ROOT / f"{domain}_actions.py"
    spec = importlib.util.spec_from_file_location(
        f"_{domain}_confirm_guard_test", path
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize(
    "domain,action",
    (
        ("util", "start_pie"),
        ("util", "stop_pie"),
        ("level", "save_current_level"),
        ("level", "save_all_levels"),
    ),
)
def test_empty_call_is_rejected_before_touching_editor(
    monkeypatch, domain, action
):
    def unexpected_subsystem(_subsystem_type):
        raise AssertionError("confirmation guard must run before editor access")

    unreal = SimpleNamespace(
        LevelEditorSubsystem=object(),
        get_editor_subsystem=unexpected_subsystem,
    )
    module = _load_action_module(monkeypatch, domain, unreal)

    result = json.loads(getattr(module, f"ue_{action}")())

    assert result["success"] is False
    assert result["code"] == "CONFIRMATION_REQUIRED"


@pytest.mark.parametrize(
    "domain,action,method",
    (
        ("util", "start_pie", "editor_request_begin_play"),
        ("util", "stop_pie", "editor_request_end_play"),
        ("level", "save_current_level", "save_current_level"),
        ("level", "save_all_levels", "save_all_dirty_levels"),
    ),
)
def test_confirm_true_preserves_editor_action(monkeypatch, domain, action, method):
    calls = []

    class Subsystem:
        def __getattr__(self, name):
            assert name == method

            def invoke():
                calls.append(name)
                return True

            return invoke

    unreal = SimpleNamespace(
        LevelEditorSubsystem=object(),
        get_editor_subsystem=lambda _subsystem_type: Subsystem(),
    )
    module = _load_action_module(monkeypatch, domain, unreal)

    result = json.loads(getattr(module, f"ue_{action}")(confirm=True))

    assert result["success"] is True
    assert calls == [method]
