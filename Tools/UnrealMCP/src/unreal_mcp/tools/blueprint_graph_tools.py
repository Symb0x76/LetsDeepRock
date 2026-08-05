"""Blueprint 图表只读检查与搜索工具。"""

import json
from typing import TYPE_CHECKING

from ._helpers import unwrap_result

if TYPE_CHECKING:
    from ..server import UnrealConnection


_EDITOR_MODULE_ERROR = "Editor 模块未加载/需要重新编译并重启编辑器"


def _error(message: str) -> str:
    return json.dumps({"success": False, "error": message}, ensure_ascii=False)


def _validate_game_path(value, name: str) -> str | None:
    if not isinstance(value, str) or not value.strip():
        return f"{name} 必须是非空字符串"
    if value != value.strip() or not value.startswith("/Game"):
        return f"{name} 必须以 /Game 开头且不能包含首尾空白"
    return None


def _validate_string_list(value, name: str, require_non_empty: bool) -> str | None:
    if not isinstance(value, list):
        return f"{name} 必须是列表"
    if require_non_empty and not value:
        return f"{name} 必须是非空列表"
    for index, item in enumerate(value):
        if not isinstance(item, str) or not item.strip():
            return f"{name}[{index}] 必须是非空字符串"
        if item != item.strip():
            return f"{name}[{index}] 不能包含首尾空白"
    return None


def _bridge_code(method_name: str, arguments: str) -> str:
    """生成仅调用 C++ Editor 桥接库的 UE Python。"""
    return f'''import json
import unreal

try:
    _library = getattr(unreal, "LetsDeepRockBlueprintGraphLibrary", None)
    _method = getattr(_library, "{method_name}", None) if _library is not None else None
    if _method is None:
        print(json.dumps({{"success": False, "error": {_EDITOR_MODULE_ERROR!r}}}, ensure_ascii=False))
    else:
        _result = unreal.LetsDeepRockBlueprintGraphLibrary.{method_name}({arguments})
        print(_result)
except Exception as _ex:
    print(json.dumps({{"success": False, "error": str(_ex)}}, ensure_ascii=False))
'''


def register(server: "FastMCP", connection: "UnrealConnection"):
    """注册 Blueprint 图表只读工具。"""

    @server.tool()
    async def inspect_blueprint_graph(
        asset_path: str,
        graph_name: str | None = None,
        include_pins: bool = True,
    ) -> str:
        """检查一个 Blueprint 的指定图表或全部图表，不修改资产。"""
        error = _validate_game_path(asset_path, "asset_path")
        if error:
            return _error(error)
        if graph_name is not None:
            if not isinstance(graph_name, str) or not graph_name.strip():
                return _error("graph_name 必须是非空字符串或 null")
            if graph_name != graph_name.strip():
                return _error("graph_name 不能包含首尾空白")
        if not isinstance(include_pins, bool):
            return _error("include_pins 必须是 bool")

        code = _bridge_code(
            "inspect_blueprint_graph_json",
            f"{asset_path!r}, {(graph_name or '')!r}, {include_pins!r}",
        )
        return unwrap_result(await connection.execute_python(code))

    @server.tool()
    async def search_blueprint_nodes(
        paths: list[str],
        keywords: list[str],
        node_classes: list[str] | None = None,
    ) -> str:
        """在指定 Content 路径中按关键词和可选节点类搜索 Blueprint 节点。"""
        error = _validate_string_list(paths, "paths", require_non_empty=True)
        if error:
            return _error(error)
        for index, path in enumerate(paths):
            error = _validate_game_path(path, f"paths[{index}]")
            if error:
                return _error(error)
        error = _validate_string_list(keywords, "keywords", require_non_empty=True)
        if error:
            return _error(error)
        if node_classes is not None:
            error = _validate_string_list(node_classes, "node_classes", require_non_empty=False)
            if error:
                return _error(error)

        code = _bridge_code(
            "search_blueprint_nodes_json",
            f"{paths!r}, {keywords!r}, {(node_classes or [])!r}",
        )
        return unwrap_result(await connection.execute_python(code))
