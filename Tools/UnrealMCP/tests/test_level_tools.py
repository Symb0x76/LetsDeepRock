"""关卡切换与编辑器状态 MCP 工具单元测试。"""

import asyncio
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from unreal_mcp.tools import level_tools, utility_tools


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


class TestLevelTools(unittest.TestCase):
    def setUp(self):
        self.server = FakeServer()
        self.connection = FakeConnection()
        level_tools.register(self.server, self.connection)
        self.open_level = self.server.tools["open_level"]
        self.get_status = self.server.tools["get_open_level_status"]

    def run_async(self, coroutine):
        return asyncio.run(coroutine)

    def test_open_level_schedules_post_tick_before_load(self):
        result = self.run_async(self.open_level("/Game/Maps/TestLevel"))
        self.assertEqual(result, "bridge result")
        code = self.connection.codes[0]
        register_at = code.index("register_slate_post_tick_callback")
        load_at = code.index(".load_level(_target)")
        self.assertLess(register_at, load_at)
        self.assertIn("unregister_slate_post_tick_callback", code)
        self.assertIn("unreal.LevelEditorSubsystem", code)
        self.assertNotIn("save_dirty_packages", code)

    def test_invalid_paths_do_not_connect(self):
        for path in ("Content/Maps/Test", "/Engine/Maps/Test", "/Game/", "", None):
            result = self.run_async(self.open_level(path))
            self.assertFalse(json.loads(result)["success"])
        self.assertEqual(self.connection.codes, [])

    def test_object_path_is_normalized(self):
        self.run_async(self.open_level("/Game/Maps/TestLevel.TestLevel"))
        code = self.connection.codes[0]
        self.assertIn("target = '/Game/Maps/TestLevel'", code)
        self.assertNotIn("target = '/Game/Maps/TestLevel.TestLevel'", code)

    def test_status_does_not_persist_world_wrapper(self):
        self.run_async(self.get_status())
        code = self.connection.codes[0]
        self.assertIn("world = editor_subsystem.get_editor_world()", code)
        self.assertIn("del world", code)
        self.assertNotIn("runtime.world", code)
        self.assertNotIn("state[\"world\"]", code)


class TestUtilityStatus(unittest.TestCase):
    def test_ue58_project_name_and_world_cleanup(self):
        server = FakeServer()
        connection = FakeConnection()
        utility_tools.register(server, connection)
        asyncio.run(server.tools["get_editor_status"]())
        code = connection.codes[0]
        self.assertIn("Paths.get_project_file_path", code)
        self.assertNotIn("SystemLibrary.get_project_name", code)
        self.assertIn("UnrealEditorSubsystem", code)
        self.assertIn("del world", code)
        self.assertIn("del game_world", code)


if __name__ == "__main__":
    unittest.main()
