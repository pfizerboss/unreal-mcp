import unreal
from UnrealMCPython.tests.base import MCPTestCase, TEST_ROOT

_BP_NAME = "MCP_TestBlueprint"
_BP_PATH = f"{TEST_ROOT}/{_BP_NAME}"


class TestBlueprintActions(MCPTestCase):

    def setUp(self):
        self._bp_path = None
        self.ensure_test_dir()
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        factory = unreal.BlueprintFactory()
        factory.set_editor_property('parent_class', unreal.Actor)
        bp = tools.create_asset(_BP_NAME, TEST_ROOT, unreal.Blueprint, factory)
        if bp:
            self._bp_path = _BP_PATH
            unreal.EditorAssetLibrary.save_loaded_asset(bp)

    def tearDown(self):
        if self._bp_path:
            self.delete_asset(self._bp_path)

    def _skip_if_no_bp(self):
        if not self._bp_path:
            self.skipTest("Blueprint not created in setUp")

    def _compile_status(self):
        brief = self.call(
            "blueprint_actions",
            "ue_get_blueprint_brief",
            asset_path=self._bp_path,
        )
        self.assertSuccess(brief)
        return brief["data"]["compile_status"]

    def _is_blueprint_package_dirty(self):
        blueprint = unreal.EditorAssetLibrary.load_asset(self._bp_path)
        self.assertIsNotNone(blueprint)
        package_name = blueprint.get_outer().get_name()
        return any(
            package.get_name() == package_name
            for package in unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()
        )

    # ── read ──────────────────────────────────────────────────────────────────

    def test_get_graph_info(self):
        self._skip_if_no_bp()
        r = self.call("blueprint_actions", "ue_get_blueprint_graph_info",
                      asset_path=self._bp_path)
        self.assertSuccess(r)
        self.assertIn("nodes", r)

    def test_list_callable_functions(self):
        self._skip_if_no_bp()
        r = self.call("blueprint_actions", "ue_list_callable_functions",
                      asset_path=self._bp_path)
        self.assertSuccess(r)

    def test_list_blueprint_variables(self):
        self._skip_if_no_bp()
        before_status = self._compile_status()
        before_dirty = self._is_blueprint_package_dirty()
        r = self.call("blueprint_actions", "ue_list_blueprint_variables",
                      asset_path=self._bp_path)
        self.assertSuccess(r)
        self.assertEqual(self._compile_status(), before_status)
        self.assertEqual(self._is_blueprint_package_dirty(), before_dirty)

    def test_list_blueprint_components(self):
        self._skip_if_no_bp()
        r = self.call("blueprint_actions", "ue_list_blueprint_components",
                      asset_path=self._bp_path)
        self.assertSuccess(r)

    # ── write ─────────────────────────────────────────────────────────────────

    def test_add_node_and_compile(self):
        self._skip_if_no_bp()
        # K2_GetActorLocation lives in Actor, which is the parent class;
        # no "target" needed — the code searches the Blueprint's class hierarchy.
        node_json = {
            "type": "CallFunction",
            "function_name": "K2_GetActorLocation"
        }
        r = self.call("blueprint_actions", "ue_add_blueprint_node",
                      asset_path=self._bp_path,
                      graph_name="EventGraph",
                      node_json=node_json)
        self.assertSuccess(r)

        r = self.call("blueprint_actions", "ue_compile_blueprint",
                      asset_path=self._bp_path)
        self.assertSuccess(r)

    def test_add_component(self):
        self._skip_if_no_bp()
        r = self.call("blueprint_actions", "ue_add_component_to_blueprint",
                      asset_path=self._bp_path,
                      component_class_path="/Script/Engine.StaticMeshComponent",
                      component_name="TestMeshComp")
        self.assertSuccess(r)
        self.assertNotEqual(self._compile_status(), "UpToDate")
        self.assertTrue(self._is_blueprint_package_dirty())

    def test_add_and_remove_component(self):
        self._skip_if_no_bp()
        r = self.call("blueprint_actions", "ue_add_component_to_blueprint",
                      asset_path=self._bp_path,
                      component_class_path="/Script/Engine.PointLightComponent",
                      component_name="TestLightComp")
        self.assertSuccess(r)
        compiled = self.call("blueprint_actions", "ue_compile_blueprint",
                             asset_path=self._bp_path)
        self.assertSuccess(compiled)
        blueprint = unreal.EditorAssetLibrary.load_asset(self._bp_path)
        self.assertTrue(unreal.EditorAssetLibrary.save_loaded_asset(blueprint))
        self.assertFalse(self._is_blueprint_package_dirty())
        r = self.call("blueprint_actions", "ue_remove_component_from_blueprint",
                      asset_path=self._bp_path,
                      component_name="TestLightComp")
        self.assertSuccess(r)
        self.assertNotEqual(self._compile_status(), "UpToDate")
        self.assertTrue(self._is_blueprint_package_dirty())

    def test_legacy_component_name_adapter_is_case_sensitive(self):
        self._skip_if_no_bp()
        added = self.call(
            "blueprint_actions",
            "ue_add_component_to_blueprint",
            asset_path=self._bp_path,
            component_class_path="/Script/Engine.StaticMeshComponent",
            component_name="ExactCaseComp",
        )
        self.assertSuccess(added)

        rejected = self.call(
            "blueprint_actions",
            "ue_remove_component_from_blueprint",
            asset_path=self._bp_path,
            component_name="exactcasecomp",
        )
        self.assertFalse(rejected.get("success"), rejected)
        components = self.call(
            "blueprint_actions",
            "ue_list_blueprint_components",
            asset_path=self._bp_path,
        )
        self.assertSuccess(components)
        self.assertIn(
            "ExactCaseComp",
            [component["variable_name"] for component in components["components"]],
        )

    def test_auto_layout_graph(self):
        self._skip_if_no_bp()
        r = self.call("blueprint_actions", "ue_auto_layout_graph",
                      asset_path=self._bp_path, graph_name="EventGraph")
        self.assertSuccess(r)

    # ── selection queries ───────────────────────────────────────────────────────

    def test_get_selected_bp_nodes(self):
        r = self.call("blueprint_actions", "ue_get_selected_bp_nodes")
        self.assertSuccess(r)
        self.assertIn("selected_nodes", r)

    def test_get_selected_bp_node_infos(self):
        r = self.call("blueprint_actions", "ue_get_selected_bp_node_infos")
        self.assertSuccess(r)
        self.assertIn("nodes", r)

    # ── node position / remove ──────────────────────────────────────────────────

    def _add_node(self, **node_json):
        r = self.call("blueprint_actions", "ue_add_blueprint_node",
                      asset_path=self._bp_path, graph_name="EventGraph",
                      node_json=node_json)
        self.assertSuccess(r)
        return r["node_name"]

    def test_set_blueprint_node_position(self):
        self._skip_if_no_bp()
        node = self._add_node(type="CallFunction", function_name="K2_GetActorLocation")
        r = self.call("blueprint_actions", "ue_set_blueprint_node_position",
                      asset_path=self._bp_path, graph_name="EventGraph",
                      node_name=node, pos_x=320.0, pos_y=128.0)
        self.assertSuccess(r)

    def test_remove_blueprint_node(self):
        self._skip_if_no_bp()
        node = self._add_node(type="CallFunction", function_name="K2_GetActorLocation")
        r = self.call("blueprint_actions", "ue_remove_blueprint_node",
                      asset_path=self._bp_path, graph_name="EventGraph",
                      node_name=node)
        self.assertSuccess(r)

    # ── connect pins ────────────────────────────────────────────────────────────

    def test_connect_blueprint_pins(self):
        self._skip_if_no_bp()
        event = self._add_node(type="Event", event_name="ReceiveBeginPlay")
        setter = self._add_node(type="CallFunction", function_name="K2_SetActorLocation")
        r = self.call("blueprint_actions", "ue_connect_blueprint_pins",
                      asset_path=self._bp_path, graph_name="EventGraph",
                      source_node=event, source_pin="then",
                      target_node=setter, target_pin="execute")
        self.assertSuccess(r)

    # ── build whole graph ───────────────────────────────────────────────────────

    def test_build_blueprint_graph(self):
        self._skip_if_no_bp()
        structure = {
            "nodes": [
                {"id": "evt", "type": "Event", "event_name": "ReceiveBeginPlay"},
                {"id": "loc", "type": "CallFunction", "function_name": "K2_GetActorLocation"},
            ],
            "connections": [],
        }
        r = self.call("blueprint_actions", "ue_build_blueprint_graph",
                      asset_path=self._bp_path, graph_name="EventGraph",
                      graph_structure=structure)
        self.assertSuccess(r)

    # ── component property ──────────────────────────────────────────────────────

    def test_set_component_property(self):
        self._skip_if_no_bp()
        added = self.call("blueprint_actions", "ue_add_component_to_blueprint",
                          asset_path=self._bp_path,
                          component_class_path="/Script/Engine.PointLightComponent",
                          component_name="PropLightComp")
        self.assertSuccess(added)
        compiled = self.call("blueprint_actions", "ue_compile_blueprint",
                             asset_path=self._bp_path)
        self.assertSuccess(compiled)
        blueprint = unreal.EditorAssetLibrary.load_asset(self._bp_path)
        self.assertTrue(unreal.EditorAssetLibrary.save_loaded_asset(blueprint))
        self.assertFalse(self._is_blueprint_package_dirty())
        r = self.call("blueprint_actions", "ue_set_component_property",
                      asset_path=self._bp_path, component_name="PropLightComp",
                      property_name="Intensity", value="5000.0")
        self.assertSuccess(r)
        self.assertNotEqual(self._compile_status(), "UpToDate")
        self.assertTrue(self._is_blueprint_package_dirty())

    def test_set_component_property_rejects_unsafe_properties_before_mutation(self):
        self._skip_if_no_bp()
        added = self.call("blueprint_actions", "ue_add_component_to_blueprint",
                          asset_path=self._bp_path,
                          component_class_path="/Script/Engine.PointLightComponent",
                          component_name="SafePropertyComp")
        self.assertSuccess(added)
        compiled = self.call("blueprint_actions", "ue_compile_blueprint",
                             asset_path=self._bp_path)
        self.assertSuccess(compiled)
        blueprint = unreal.EditorAssetLibrary.load_asset(self._bp_path)
        self.assertTrue(unreal.EditorAssetLibrary.save_loaded_asset(blueprint))
        self.assertFalse(self._is_blueprint_package_dirty())

        for property_name, value in (
            ("CachedMaxDrawDistance", "1000"),
            ("bIsBeingMovedByEditor", "true"),
            ("OnComponentActivated", "()"),
            ("DefinitelyMissingProperty", "1"),
        ):
            with self.subTest(property_name=property_name):
                rejected = self.call(
                    "blueprint_actions",
                    "ue_set_component_property",
                    asset_path=self._bp_path,
                    component_name="SafePropertyComp",
                    property_name=property_name,
                    value=value,
                )
                self.assertFalse(rejected.get("success"), rejected)
                self.assertFalse(self._is_blueprint_package_dirty())

    def test_create_blueprint(self):
        import unreal
        from UnrealMCPython.tests.base import TEST_ROOT
        path = f"{TEST_ROOT}/MCP_CreatedBP"
        self.delete_asset(path)
        try:
            r = self.call("blueprint_actions", "ue_create_blueprint",
                          asset_path=path, parent_class_path="/Script/Engine.Actor")
            self.assertSuccess(r)
            self.assertTrue(unreal.EditorAssetLibrary.does_asset_exist(path))
        finally:
            self.delete_asset(path)

    def test_create_blueprint_bad_parent(self):
        from UnrealMCPython.tests.base import TEST_ROOT
        r = self.call("blueprint_actions", "ue_create_blueprint",
                      asset_path=f"{TEST_ROOT}/MCP_BadBP", parent_class_path="/Script/Engine.NopeXYZ")
        self.assertFalse(r.get("success"))

    def test_add_variable(self):
        self._skip_if_no_bp()
        r = self.call("blueprint_actions", "ue_add_variable",
                      asset_path=self._bp_path, variable_name="MyFloatVar", variable_type="float")
        self.assertSuccess(r)
        self.assertEqual(r["variable_name"], "MyFloatVar")
        self.assertEqual(r["variable_type"], "real")
        self.assertEqual(
            r["next_actions"][0]["action"], "compile_blueprint"
        )
        self.assertNotEqual(self._compile_status(), "UpToDate")
        self.assertTrue(self._is_blueprint_package_dirty())
        variables = self.call("blueprint_actions", "ue_list_blueprint_variables",
                              asset_path=self._bp_path)
        self.assertSuccess(variables)

    def test_add_variable_bad_type(self):
        self._skip_if_no_bp()
        r = self.call("blueprint_actions", "ue_add_variable",
                      asset_path=self._bp_path, variable_name="X", variable_type="notatype")
        self.assertFalse(r.get("success"))

    def test_set_variable_flags(self):
        self._skip_if_no_bp()
        self.call("blueprint_actions", "ue_add_variable",
                  asset_path=self._bp_path, variable_name="FlagVar", variable_type="bool")
        r = self.call("blueprint_actions", "ue_set_variable_flags",
                      asset_path=self._bp_path, variable_name="FlagVar",
                      instance_editable=True, expose_on_spawn=True)
        self.assertSuccess(r)
        self.assertEqual(
            r["applied"],
            {"instance_editable": True, "expose_on_spawn": True},
        )
        self.assertEqual(r["variable_name"], "FlagVar")
        self.assertEqual(
            r["next_actions"][0]["action"], "compile_blueprint"
        )
        self.assertNotEqual(self._compile_status(), "UpToDate")
        self.assertTrue(self._is_blueprint_package_dirty())

    def test_set_variable_flags_none(self):
        self._skip_if_no_bp()
        r = self.call("blueprint_actions", "ue_set_variable_flags",
                      asset_path=self._bp_path, variable_name="X")
        self.assertFalse(r.get("success"))
