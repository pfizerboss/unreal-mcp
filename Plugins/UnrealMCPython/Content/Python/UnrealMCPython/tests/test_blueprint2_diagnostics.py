"""In-editor coverage for structured Blueprint compiler diagnostics."""

import uuid

import unreal

from UnrealMCPython.tests.base import MCPTestCase
from UnrealMCPython.tests.blueprint2_support import (
    BLUEPRINT2_TEST_ROOT,
    call_action,
)


class TestBlueprint2Diagnostics(MCPTestCase):

    def setUp(self):
        self.ensure_test_dir()
        self._created_assets = []
        self.addCleanup(self._cleanup_created_assets)

        name = f"Blueprint2Diagnostics_{uuid.uuid4().hex[:10]}"
        factory = unreal.BlueprintFactory()
        factory.set_editor_property("parent_class", unreal.Actor)
        self.blueprint = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name,
            BLUEPRINT2_TEST_ROOT,
            unreal.Blueprint,
            factory,
        )
        self.assertIsNotNone(self.blueprint)
        self.asset_path = f"{BLUEPRINT2_TEST_ROOT}/{name}.{name}"
        self._created_assets.append(self.asset_path)
        self.graph_id = self._event_graph_id()

    def tearDown(self):
        self._cleanup_created_assets()

    def _cleanup_created_assets(self):
        if not self._created_assets:
            return
        unreal.SystemLibrary.collect_garbage()
        created_assets = list(reversed(self._created_assets))
        self._created_assets = []
        for asset_path in created_assets:
            with self.subTest(cleanup_asset=asset_path):
                self.delete_asset(asset_path)

    @classmethod
    def tearDownClass(cls):
        if not unreal.EditorAssetLibrary.does_directory_exist(
            BLUEPRINT2_TEST_ROOT
        ):
            return
        remaining = unreal.EditorAssetLibrary.list_assets(
            BLUEPRINT2_TEST_ROOT,
            recursive=True,
            include_folder=False,
        )
        if remaining:
            raise AssertionError(
                f"Blueprint diagnostics tests left assets behind: {remaining}"
            )
        deleted = unreal.EditorAssetLibrary.delete_directory(
            BLUEPRINT2_TEST_ROOT
        )
        if (
            not deleted
            and unreal.EditorAssetLibrary.does_directory_exist(
                BLUEPRINT2_TEST_ROOT
            )
        ):
            raise AssertionError(
                "Failed to remove the empty Blueprint diagnostics test root"
            )

    def _inspect(self, queries):
        result = call_action(
            "blueprint_actions",
            "ue_inspect_blueprint",
            asset_path=self.asset_path,
            queries=queries,
        )
        self.assertSuccess(result)
        return result["data"]["results"]

    def _event_graph_id(self):
        nodes = self._inspect(
            [{"op": "nodes", "detail": "compact", "limit": 500}]
        )[0]["items"]
        event_graphs = {
            node["graph_id"]
            for node in nodes
            if node["class_path"] == "/Script/BlueprintGraph.K2Node_Event"
        }
        self.assertEqual(len(event_graphs), 1, event_graphs)
        return next(iter(event_graphs))

    def _node_records(self):
        return self._inspect(
            [{"op": "nodes", "detail": "detailed", "limit": 500}]
        )[0]["items"]

    def _pin(self, node_id, pin_name):
        pins = self._inspect(
            [
                {
                    "op": "pins",
                    "node_id": node_id,
                    "detail": "detailed",
                    "limit": 500,
                }
            ]
        )[0]["items"]
        matches = [pin for pin in pins if pin["name"] == pin_name]
        self.assertEqual(len(matches), 1, matches)
        return matches[0]

    def _begin_play(self):
        matches = [
            node
            for node in self._node_records()
            if node["class_path"] == "/Script/BlueprintGraph.K2Node_Event"
            and "BeginPlay" in node["title"]
        ]
        self.assertEqual(len(matches), 1, matches)
        return matches[0]

    def _add_common(self, graph_name="EventGraph", **node_json):
        result = call_action(
            "blueprint_actions",
            "ue_add_blueprint_node",
            asset_path=self.asset_path,
            graph_name=graph_name,
            node_json=node_json,
        )
        self.assertSuccess(result)
        return result["data"]["node_id"]

    def _add_reflected(self, member_path):
        result = call_action(
            "blueprint_actions",
            "ue_add_reflected_blueprint_node",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            member_kind="function",
            member_path=member_path,
            position={"x": 420, "y": 160},
        )
        self.assertSuccess(result)
        return result["data"]["node_id"]

    def _connect(
        self,
        source_node,
        source_pin,
        target_node,
        target_pin,
        graph_name="EventGraph",
    ):
        result = call_action(
            "blueprint_actions",
            "ue_connect_blueprint_pins",
            asset_path=self.asset_path,
            graph_name=graph_name,
            source_node=source_node,
            source_pin=source_pin,
            target_node=target_node,
            target_pin=target_pin,
        )
        self.assertSuccess(result)

    def _compile(self):
        return call_action(
            "blueprint_actions",
            "ue_compile_blueprint",
            asset_path=self.asset_path,
        )

    def _health(self, include_warnings=True):
        return call_action(
            "blueprint_actions",
            "ue_get_blueprint_health",
            asset_path=self.asset_path,
            include_warnings=include_warnings,
        )

    def _assert_health_envelope(self, result):
        for field in (
            "success",
            "status",
            "summary",
            "data",
            "warnings",
            "errors",
            "next_actions",
            "trace_id",
        ):
            self.assertIn(field, result)
        for field in (
            "asset_path",
            "healthy",
            "compile_status",
            "issue_count",
            "error_count",
            "warning_count",
            "issues",
        ):
            self.assertIn(field, result["data"])
        self.assertEqual(
            result["data"]["issue_count"], len(result["data"]["issues"])
        )
        self.assertEqual(
            result["data"]["error_count"],
            sum(
                issue["severity"] == "error"
                for issue in result["data"]["issues"]
            ),
        )
        self.assertEqual(
            result["data"]["warning_count"],
            sum(
                issue["severity"] == "warning"
                for issue in result["data"]["issues"]
            ),
        )
        for issue in result["data"]["issues"]:
            self.assertEqual(
                set(issue),
                {
                    "code",
                    "severity",
                    "message",
                    "hint",
                    "graph_id",
                    "node_id",
                    "pin_id",
                    "member_id",
                },
            )
            self.assertIn(issue["severity"], ("error", "warning"))
            self.assertTrue(issue["message"])
            self.assertTrue(issue["hint"])
            for field, prefix in (
                ("graph_id", "graph:"),
                ("node_id", "node:"),
                ("pin_id", "pin:"),
            ):
                if issue[field]:
                    self.assertTrue(issue[field].startswith(prefix), issue)

    def _is_dirty(self):
        package_name = self.blueprint.get_outer().get_name()
        return any(
            package.get_name() == package_name
            for package in unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()
        )

    def _assert_envelope(self, result):
        for field in (
            "success",
            "status",
            "message",
            "summary",
            "data",
            "diagnostics",
            "warnings",
            "errors",
            "next_actions",
            "trace_id",
        ):
            self.assertIn(field, result)
        self.assertEqual(result["data"]["result_status"], result["status"])
        self.assertEqual(
            result["data"]["diagnostic_count"], len(result["diagnostics"])
        )
        self.assertEqual(
            result["data"]["error_count"],
            sum(item["severity"] == "error" for item in result["diagnostics"]),
        )
        self.assertEqual(
            result["data"]["warning_count"],
            sum(item["severity"] == "warning" for item in result["diagnostics"]),
        )
        for diagnostic in result["diagnostics"]:
            self.assertEqual(
                set(diagnostic),
                {
                    "code",
                    "severity",
                    "message",
                    "hint",
                    "graph_id",
                    "node_id",
                    "pin_id",
                },
            )
            self.assertIn(diagnostic["severity"], ("error", "warning"))
            self.assertTrue(diagnostic["message"])
            self.assertTrue(diagnostic["hint"])
            if diagnostic["graph_id"]:
                self.assertTrue(diagnostic["graph_id"].startswith("graph:"))
            if diagnostic["node_id"]:
                self.assertTrue(diagnostic["node_id"].startswith("node:"))
            if diagnostic["pin_id"]:
                self.assertTrue(diagnostic["pin_id"].startswith("pin:"))

    def _assert_compile_failure(self, expected_code):
        was_dirty = self._is_dirty()
        self.assertTrue(was_dirty)
        result = self._compile()
        self._assert_envelope(result)
        self.assertFalse(result["success"], result)
        self.assertEqual(result["status"], "Error")
        self.assertEqual(len(result["errors"]), 1)
        self.assertEqual(result["errors"][0]["code"], "COMPILE_FAILED")
        self.assertTrue(self._is_dirty())
        matches = [
            item for item in result["diagnostics"] if item["code"] == expected_code
        ]
        self.assertTrue(matches, result)
        self.assertTrue(any(item["node_id"] for item in matches), result)
        inspect_actions = [
            item
            for item in result["next_actions"]
            if item["action"] == "inspect_blueprint"
        ]
        self.assertEqual(len(inspect_actions), 1, result)
        queries = inspect_actions[0]["params"]["queries"]
        self.assertTrue(
            any(
                query["op"] == "nodes" and query["detail"] == "detailed"
                for query in queries
            ),
            result,
        )
        targeted_nodes = {
            item["node_id"] for item in matches if item["node_id"]
        }
        self.assertTrue(
            any(
                query["op"] == "pins"
                and query["detail"] == "detailed"
                and query["node_id"] in targeted_nodes
                for query in queries
            ),
            result,
        )
        return result

    def test_valid_compile_preserves_legacy_fields_and_dirty_state(self):
        self.assertTrue(self._is_dirty())

        result = self._compile()

        self._assert_envelope(result)
        self.assertSuccess(result)
        self.assertEqual(result["status"], "UpToDate")
        self.assertEqual(result["message"], "Blueprint compiled successfully.")
        self.assertEqual(result["data"]["error_count"], 0)
        self.assertTrue(self._is_dirty())
        self.assertTrue(result["next_actions"])

    def test_get_blueprint_health_reports_a_healthy_asset(self):
        result = self._health()

        self._assert_health_envelope(result)
        self.assertSuccess(result)
        self.assertTrue(result["data"]["healthy"], result)
        self.assertEqual(result["data"]["issues"], [])
        self.assertEqual(result["data"]["compile_status"], "UpToDate")

    def test_warning_compile_returns_stable_node_diagnostic(self):
        analog_input = self._add_common(
            type="InputKey",
            key_name="MouseX",
            pos_x=700,
            pos_y=160,
        )
        branch = self._add_common(
            type="Branch",
            pos_x=980,
            pos_y=160,
        )
        self._connect(
            analog_input,
            self._pin(analog_input, "Pressed")["pin_id"],
            branch,
            self._pin(branch, "execute")["pin_id"],
        )

        result = self._compile()

        self._assert_envelope(result)
        self.assertSuccess(result)
        self.assertEqual(result["status"], "UpToDateWithWarnings")
        warnings = [
            item for item in result["diagnostics"] if item["severity"] == "warning"
        ]
        self.assertTrue(warnings, result)
        self.assertTrue(any(item["code"] == "BP_COMPILE_WARNING" for item in warnings))
        self.assertTrue(
            any(item["node_id"] == analog_input for item in warnings), result
        )

        health = self._health()
        self._assert_health_envelope(health)
        self.assertSuccess(health)
        self.assertTrue(health["data"]["healthy"], health)
        self.assertGreater(health["data"]["warning_count"], 0)
        self.assertTrue(
            any(
                issue["severity"] == "warning"
                for issue in health["data"]["issues"]
            ),
            health,
        )

        filtered = self._health(include_warnings=False)
        self._assert_health_envelope(filtered)
        self.assertEqual(filtered["warnings"], [])
        self.assertEqual(filtered["data"]["warning_count"], 0)
        self.assertFalse(
            any(
                issue["severity"] == "warning"
                for issue in filtered["data"]["issues"]
            ),
            filtered,
        )

    def test_missing_required_pin_returns_pin_targeted_diagnostic(self):
        begin_play = self._begin_play()
        timer = self._add_reflected(
            "/Script/Engine.PlayerController:SetMouseCursorWidget"
        )
        self._connect(
            begin_play["node_id"],
            self._pin(begin_play["node_id"], "then")["pin_id"],
            timer,
            self._pin(timer, "execute")["pin_id"],
        )

        result = self._assert_compile_failure("BP_MISSING_REQUIRED_PIN")
        matches = [
            item
            for item in result["diagnostics"]
            if item["code"] == "BP_MISSING_REQUIRED_PIN"
        ]
        self.assertTrue(any(item["pin_id"] for item in matches), result)

        health = self._health()
        self._assert_health_envelope(health)
        self.assertFalse(health["success"], health)
        self.assertFalse(health["data"]["healthy"], health)
        self.assertEqual(len(health["errors"]), 1)
        self.assertEqual(health["errors"][0]["code"], "COMPILE_FAILED")
        health_matches = [
            issue
            for issue in health["data"]["issues"]
            if issue["code"] == "BP_MISSING_REQUIRED_PIN"
        ]
        self.assertEqual(len(health_matches), 2, health)
        targeted_pin_ids = {
            issue["pin_id"] for issue in health_matches if issue["pin_id"]
        }
        self.assertEqual(len(targeted_pin_ids), 2, health)

    def test_deleted_function_returns_unresolved_member_diagnostic(self):
        function_name = "RemovedDiagnosticFunction"
        created = self.call(
            "blueprint_actions",
            "ue_create_blueprint_function",
            asset_path=self.asset_path,
            function_name=function_name,
            inputs=[],
            outputs=[],
        )
        self.assertSuccess(created)
        initial_compile = self._compile()
        self.assertSuccess(initial_compile)

        caller = self._add_common(
            type="CallFunction",
            function_name=function_name,
            pos_x=420,
            pos_y=320,
        )
        begin_play = self._begin_play()
        self._connect(
            begin_play["node_id"],
            self._pin(begin_play["node_id"], "then")["pin_id"],
            caller,
            self._pin(caller, "execute")["pin_id"],
        )
        deleted = call_action(
            "blueprint_actions",
            "ue_delete_blueprint_function",
            asset_path=self.asset_path,
            function_id=created["data"]["function_id"],
        )
        self.assertSuccess(deleted)

        self._assert_compile_failure("BP_UNRESOLVED_MEMBER")

        health = self._health()
        self._assert_health_envelope(health)
        self.assertFalse(health["success"], health)
        unresolved = [
            issue
            for issue in health["data"]["issues"]
            if issue["code"] == "BP_UNRESOLVED_MEMBER"
        ]
        self.assertEqual(len(unresolved), 1, health)
        self.assertEqual(unresolved[0]["node_id"], caller)

if __name__ == "__main__":
    import unittest

    unittest.main(verbosity=2)
