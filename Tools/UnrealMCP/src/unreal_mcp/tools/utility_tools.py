"""通用工具（Python 代码执行、控制台命令、资产保存、编辑器状态、资产列表）"""

import json
from typing import TYPE_CHECKING

from ._helpers import unwrap_result

if TYPE_CHECKING:
    from ..server import UnrealConnection


def register(server, connection: "UnrealConnection"):
    """向 MCP Server 注册通用工具"""

    # ================================================================
    #  1. execute_python — 在 UE 编辑器中执行任意 Python 代码
    # ================================================================
    @server.tool()
    async def execute_python(code: str) -> str:
        """在 UE 编辑器中执行任意 Python 代码（万能逃生口）。

        当其他工具无法满足需求时，使用此工具直接调用 UE Python API。
        适合临时查询、调试或执行不在预设工具范围内的操作。

        Args:
            code: 要在 UE 编辑器中执行的 Python 代码字符串

        Returns:
            UE 编辑器返回的执行结果字符串
        """
        result = await connection.execute_python(code)
        return unwrap_result(result)

    # ================================================================
    #  2. execute_console_command — 执行 UE 控制台命令
    # ================================================================
    @server.tool()
    async def execute_console_command(command: str) -> str:
        """执行 UE 控制台命令。

        可以执行任意 Unreal Engine 控制台命令，如 "stat fps" 开关帧率显示、
        "showdebug abilitysystem" 显示 GAS 调试信息等。

        Args:
            command: 控制台命令字符串（如 "stat fps", "showdebug abilitysystem"）

        Returns:
            执行结果字符串
        """
        code = f'''import unreal
import json

command = {repr(command)}
try:
    unreal.SystemLibrary.execute_console_command(None, command)
    print(json.dumps({{"success": True, "message": f"控制台命令已执行: {{command}}"}}))
except Exception as e:
    print(json.dumps({{"success": False, "message": f"执行控制台命令时出错: {{e}}"}}))
'''
        result = await connection.execute_python(code)
        return unwrap_result(result)

    # ================================================================
    #  3. save_asset — 保存指定资产或保存全部
    # ================================================================
    @server.tool()
    async def save_asset(asset_path: str | None = None) -> str:
        """保存指定资产，或在未指定时保存所有脏资产。

        Args:
            asset_path: 资产路径（如 "/Game/Maps/MainLevel"），为 None 时保存全部脏资产

        Returns:
            执行结果字符串
        """
        code = f'''import unreal
import json

asset_path = {repr(asset_path) if asset_path else 'None'}

try:
    if asset_path:
        # 保存指定资产
        editor_asset_subsystem = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
        result = editor_asset_subsystem.save_asset(asset_path)
        if result:
            print(json.dumps({{"success": True, "message": f"资产已保存: {{asset_path}}"}}))
        else:
            print(json.dumps({{"success": False, "message": f"保存资产失败: {{asset_path}}"}}))
    else:
        # 保存所有脏包
        unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
        print(json.dumps({{"success": True, "message": "所有脏资产已保存"}}))
except Exception as e:
    print(json.dumps({{"success": False, "message": f"保存资产时出错: {{e}}"}}))
'''
        result = await connection.execute_python(code)
        return unwrap_result(result)

    # ================================================================
    #  4. get_editor_status — 获取编辑器当前状态
    # ================================================================
    @server.tool()
    async def get_editor_status() -> str:
        """获取 UE 编辑器当前状态。

        返回当前打开的关卡名称、PIE（Play In Editor）运行状态、
        项目名称等编辑器基本信息。

        Returns:
            JSON 格式的编辑器状态字符串
        """
        code = '''import unreal
import json
import os

editor_subsystem = None
world = None
game_world = None
try:
    editor_subsystem = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    world = editor_subsystem.get_editor_world()
    game_world = editor_subsystem.get_game_world()

    # 尝试获取关卡路径
    current_level_path = "Unknown"
    try:
        if world:
            current_level_path = world.get_path_name()
    except Exception:
        current_level_path = str(world.get_name()) if world else "None"

    result = {
        "success": True,
        "current_level": current_level_path,
        "current_level_name": str(world.get_name()) if world else "None",
        "is_playing": game_world is not None,
        "project_name": os.path.splitext(os.path.basename(str(unreal.Paths.get_project_file_path())))[0],
        "engine_version": str(unreal.SystemLibrary.get_engine_version()),
        "has_world": world is not None,
    }
    print(json.dumps(result, default=str))
except Exception as e:
    print(json.dumps({"success": False, "message": str(e)}))
finally:
    del game_world
    del world
    del editor_subsystem
'''
        result = await connection.execute_python(code)
        return unwrap_result(result)

    # ================================================================
    #  5. list_assets_in_path — 列出指定路径下的所有资产
    # ================================================================
    @server.tool()
    async def list_assets_in_path(
        package_path: str = "/Game/",
        recursive: bool = True,
        class_filter: str | None = None,
    ) -> str:
        """列出指定 Content Browser 路径下的所有资产。

        Args:
            package_path: 资产路径（如 "/Game/Variant_Weapon/"），默认为 "/Game/"（根目录）
            recursive: 是否递归搜索子目录，默认 True
            class_filter: 可选的类名过滤（如 "Blueprint", "StaticMesh", "Material"），默认不过滤

        Returns:
            JSON 格式的资产列表字符串
        """
        # 解析 class_filter
        filter_code = ""
        if class_filter:
            filter_code = f'''
    # 按类名过滤
    filtered = []
    for asset in assets:
        asset_class = str(asset.asset_class_path.asset_name)
        if {repr(class_filter)} in asset_class:
            filtered.append(asset)
    assets = filtered
'''

        code = f'''import unreal
import json

package_path = {repr(package_path)}
recursive = {repr(recursive)}

try:
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    assets = registry.get_assets_by_path(package_path, recursive=recursive)
{filter_code}
    result = {{
        "success": True,
        "count": len(assets),
        "package_path": package_path,
        "recursive": recursive,
        "assets": []
    }}

    for asset in assets:
        try:
            asset_info = {{
                "asset_name": str(asset.asset_name),
                "package_name": str(asset.package_name),
                "asset_class": str(asset.asset_class_path.asset_name),
                "package_path": str(asset.package_path),
            }}
            result["assets"].append(asset_info)
        except Exception as e:
            # 跳过无法解析的资产
            pass

    print(json.dumps(result, ensure_ascii=False, indent=2))
except Exception as e:
    print(json.dumps({{"success": False, "message": f"列出资产失败: {{e}}"}}))
'''
        result = await connection.execute_python(code)
        return unwrap_result(result)
