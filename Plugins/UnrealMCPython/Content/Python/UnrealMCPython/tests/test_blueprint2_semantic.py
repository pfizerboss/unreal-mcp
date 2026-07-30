"""In-editor acceptance coverage for semantic Blueprint graph actions."""

import unittest

from UnrealMCPython import blueprint_actions


class TestBlueprint2SemanticSurface(unittest.TestCase):

    def test_semantic_action_wrappers_are_callable(self):
        actions = (
            "suggest_blueprint_nodes_for_connection",
            "add_blueprint_connected_action_node",
            "insert_blueprint_action_node",
            "preview_blueprint_action_replacement",
            "replace_blueprint_node_with_action",
        )

        for action in actions:
            wrapper = getattr(blueprint_actions, "ue_" + action)
            with self.subTest(action=action):
                self.assertTrue(callable(wrapper))
