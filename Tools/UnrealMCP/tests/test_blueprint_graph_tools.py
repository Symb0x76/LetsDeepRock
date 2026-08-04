"""Blueprint 图表 MCP 工具单元测试。"""

import asyncio
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from unreal_mcp.tools import blueprint_graph_tools


class FakeServer:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(function):
            self.tools[function.__name__] = function
            return function
        return decorator


class FakeConnection:
    def __init__(self):
        self.codes = []

    async def execute_python(self, code):
        self.codes.append(code)
        return {"success": True, "output": [{"output": "bridge result"}]}


class TestBlueprintGraphTools(unittest.TestCase):
    def setUp(self):
        self.server = FakeServer()
        self.connection = FakeConnection()
        blueprint_graph_tools.register(self.server, self.connection)
        self.inspect = self.server.tools["inspect_blueprint_graph"]
        self.search = self.server.tools["search_blueprint_nodes"]

    def run_async(self, coroutine):
        return asyncio.run(coroutine)

    def test_inspect_defaults_and_bridge_only(self):
        result = self.run_async(self.inspect("/Game/Blueprints/BP_Test"))
        self.assertEqual(result, "bridge result")
        code = self.connection.codes[0]
        self.assertIn("unreal.LetsDeepRockBlueprintGraphLibrary.inspect_blueprint_graph_json('/Game/Blueprints/BP_Test', '', True)", code)
        self.assertIn("Editor 模块未加载/需要重新编译并重启编辑器", code)
        self.assertNotIn("EdGraph", code)
        self.assertNotIn("compile_blueprint", code)
        self.assertNotIn("save_", code)
        self.assertNotIn("set_editor_property", code)

    def test_inspect_uses_repr_for_escaping(self):
        self.run_async(self.inspect("/Game/BP_'Quoted", "Graph'Name", False))
        code = self.connection.codes[0]
        self.assertIn('"/Game/BP_\'Quoted"', code)
        self.assertIn('"Graph\'Name"', code)
        self.assertIn(", False)", code)

    def test_invalid_inspect_parameters_do_not_connect(self):
        cases = [
            ("Bad/Path", None, True),
            ("/Game/Test", "   ", True),
            ("/Game/Test", None, 1),
        ]
        for asset_path, graph_name, include_pins in cases:
            result = self.run_async(self.inspect(asset_path, graph_name, include_pins))
            self.assertFalse(json.loads(result)["success"])
        self.assertEqual(self.connection.codes, [])

    def test_search_parameters_and_bridge(self):
        result = self.run_async(self.search(
            ["/Game/AI", "/Game/Weapons"],
            ["Damage", "Branch"],
            ["K2Node_CallFunction"],
        ))
        self.assertEqual(result, "bridge result")
        code = self.connection.codes[0]
        self.assertIn("search_blueprint_nodes_json", code)
        self.assertIn("['/Game/AI', '/Game/Weapons']", code)
        self.assertIn("['Damage', 'Branch']", code)
        self.assertIn("['K2Node_CallFunction']", code)

    def test_search_none_node_classes_becomes_empty_array(self):
        self.run_async(self.search(["/Game/AI"], ["Damage"]))
        code = self.connection.codes[0]
        self.assertIn("search_blueprint_nodes_json(['/Game/AI'], ['Damage'], [])", code)

    def test_invalid_search_parameters_do_not_connect(self):
        cases = [
            ([], ["Damage"], None),
            (["/Content"], ["Damage"], None),
            (["/Game"], [], None),
            (["/Game"], [""], None),
            (["/Game"], ["Damage"], "K2Node"),
            (["/Game"], ["Damage"], [" "]),
        ]
        for paths, keywords, node_classes in cases:
            result = self.run_async(self.search(paths, keywords, node_classes))
            self.assertFalse(json.loads(result)["success"])
        self.assertEqual(self.connection.codes, [])


if __name__ == "__main__":
    unittest.main()
