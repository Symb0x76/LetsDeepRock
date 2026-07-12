"""Enhanced Input 工具集（Input Action 创建、IMC 按键绑定配置）"""

import json
from typing import TYPE_CHECKING

from ._helpers import unwrap_result

if TYPE_CHECKING:
    from ..server import UnrealConnection


def register(server, connection: "UnrealConnection"):
    """向 MCP Server 注册 Enhanced Input 相关工具"""

    @server.tool()
    async def create_input_action(
        asset_name: str,
        package_path: str,
        value_type: str = "Bool",
    ) -> str:
        """创建 Enhanced Input Action 资产。

        在指定路径下创建一个 Input Action 资产，用于 Enhanced Input 系统。

        Args:
            asset_name: IA 资产名称，如 "IA_SwitchWeapon"
            package_path: 保存路径，如 "/Game/Variant_Weapon/Input"
            value_type: 值类型 — "Bool" / "Axis1D" / "Axis2D" / "Axis3D"

        Returns:
            操作结果消息
        """
        # 值类型映射
        value_type_map = {
            "Bool": "unreal.InputActionValueType.BOOLEAN",
            "Axis1D": "unreal.InputActionValueType.AXIS_1D",
            "Axis2D": "unreal.InputActionValueType.AXIS_2D",
            "Axis3D": "unreal.InputActionValueType.AXIS_3D",
        }
        value_type_enum = value_type_map.get(value_type, "unreal.InputActionValueType.BOOLEAN")

        code = f'''import unreal
import json

try:
    asset_name = {repr(asset_name)}
    package_path = {repr(package_path)}
    target_value_type = {value_type_enum}

    # ---- 创建 Input Action 资产 ----
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.InputActionFactory()
    
    ia = asset_tools.create_asset(asset_name, package_path, None, factory)
    if not ia:
        raise RuntimeError(f"无法创建 Input Action 资产: {{asset_name}}")

    # ---- 设置值类型 ----
    ia.set_editor_property("value_type", target_value_type)

    # ---- 保存资产 ----
    save_path = f"{{package_path}}/{{asset_name}}"
    unreal.EditorAssetLibrary.save_asset(save_path)

    result = {{"success": True, "message": f"Input Action {{asset_name}} 创建成功 ({{value_type}})"}}
    print(json.dumps(result, ensure_ascii=False))

except Exception as e:
    import traceback
    error_msg = f"{{str(e)}}\\n{{traceback.format_exc()}}"
    unreal.log_error(error_msg)
    result = {{"success": False, "message": error_msg}}
    print(json.dumps(result, ensure_ascii=False))
'''

        result = await connection.execute_python(code)
        return unwrap_result(result)

    @server.tool()
    async def add_imc_mapping(
        imc_path: str,
        input_action_path: str,
        key_name: str,
        modifiers: str = "[]",
        triggers: str = "[]",
    ) -> str:
        """在 Input Mapping Context 中添加按键绑定。

        加载指定 IMC 资产和 Input Action 资产，在 IMC 的 Mappings 数组中
        追加一条按键映射。

        Args:
            imc_path: IMC 资产路径，如 "/Game/Variant_Weapon/Input/IMC_Weapon"
            input_action_path: Input Action 资产路径，如 "/Game/Variant_Weapon/Input/IA_Fire"
            key_name: 按键名称，如 "LeftMouseButton"、"RightMouseButton"、"E"、"SpaceBar"、"MouseWheelAxis"
            modifiers: JSON 格式的输入修饰器列表（可选），如 '["DeadZone"]'
            triggers: JSON 格式的触发器列表（可选），如 '["Pressed","Released"]'

        Returns:
            操作结果消息
        """
        # 解析 JSON 参数
        try:
            modifiers_list = json.loads(modifiers) if isinstance(modifiers, str) else (modifiers if modifiers else [])
        except json.JSONDecodeError:
            modifiers_list = []

        try:
            triggers_list = json.loads(triggers) if isinstance(triggers, str) else (triggers if triggers else [])
        except json.JSONDecodeError:
            triggers_list = []

        modifiers_json = json.dumps(modifiers_list)
        triggers_json = json.dumps(triggers_list)

        code = f'''import unreal
import json

try:
    imc_path = {repr(imc_path)}
    input_action_path = {repr(input_action_path)}
    key_name = {repr(key_name)}
    modifiers_list = json.loads({repr(modifiers_json)})
    triggers_list = json.loads({repr(triggers_json)})

    # ---- 加载资产 ----
    imc = unreal.EditorAssetLibrary.load_asset(imc_path)
    if not imc:
        raise RuntimeError(f"无法加载 IMC 资产: {{imc_path}}")

    ia = unreal.EditorAssetLibrary.load_asset(input_action_path)
    if not ia:
        raise RuntimeError(f"无法加载 Input Action 资产: {{input_action_path}}")

    # ---- 创建 Input Key ----
    try:
        input_key = unreal.InputKey(key_name)
    except Exception:
        # 回退：尝试通过 Key 结构体名称设置
        input_key = unreal.Key()
        input_key.set_editor_property("key_name", key_name)

    # ---- 创建 Enhanced Action Key Mapping ----
    mapping = unreal.EnhancedActionKeyMapping()
    mapping.set_editor_property("action", ia)
    mapping.set_editor_property("key", input_key)

    # ---- 获取现有 Mappings 数组 ----
    # IMC 的 Mappings 属性是 TArray<FEnhancedActionKeyMapping>
    try:
        existing_mappings = imc.get_editor_property("mappings")
    except Exception:
        existing_mappings = unreal.Array(unreal.EnhancedActionKeyMapping)

    if existing_mappings is None:
        new_mappings = unreal.Array(unreal.EnhancedActionKeyMapping)
    else:
        new_mappings = existing_mappings

    new_mappings.append(mapping)
    imc.set_editor_property("mappings", new_mappings)

    # ---- 保存 IMC ----
    unreal.EditorAssetLibrary.save_asset(imc_path)

    result = {{"success": True, "message": f"已将 {{input_action_path}} ({{key_name}}) 添加到 {{imc_path}}"}}
    print(json.dumps(result, ensure_ascii=False))

except Exception as e:
    import traceback
    error_msg = f"{{str(e)}}\\n{{traceback.format_exc()}}"
    unreal.log_error(error_msg)
    result = {{"success": False, "message": error_msg}}
    print(json.dumps(result, ensure_ascii=False))
'''

        result = await connection.execute_python(code)
        return unwrap_result(result)

    @server.tool()
    async def remove_imc_mapping(
        imc_path: str,
        input_action_path: str,
    ) -> str:
        """从 Input Mapping Context 中移除指定 Input Action 的所有绑定。

        Args:
            imc_path: IMC 资产路径，如 "/Game/Variant_Weapon/Input/IMC_Weapon"
            input_action_path: 要移除的 Input Action 资产路径

        Returns:
            操作结果消息
        """
        code = f'''import unreal
import json

try:
    imc_path = {repr(imc_path)}
    input_action_path = {repr(input_action_path)}

    # ---- 加载资产 ----
    imc = unreal.EditorAssetLibrary.load_asset(imc_path)
    if not imc:
        raise RuntimeError(f"无法加载 IMC 资产: {{imc_path}}")

    ia = unreal.EditorAssetLibrary.load_asset(input_action_path)
    if not ia:
        raise RuntimeError(f"无法加载 Input Action 资产: {{input_action_path}}")

    # ---- 获取现有 Mappings 并过滤 ----
    existing_mappings = imc.get_editor_property("mappings")
    
    if existing_mappings is None or len(existing_mappings) == 0:
        result = {{"success": True, "message": f"IMC 中没有映射到 {{input_action_path}} 的条目"}}
        print(json.dumps(result, ensure_ascii=False))
    else:
        new_mappings = unreal.Array(unreal.EnhancedActionKeyMapping)
        removed_count = 0
        for i in range(len(existing_mappings)):
            mapping = existing_mappings[i]
            mapped_action = mapping.get_editor_property("action")
            if mapped_action and mapped_action.get_path_name() == ia.get_path_name():
                removed_count += 1
            else:
                new_mappings.append(mapping)
        
        imc.set_editor_property("mappings", new_mappings)
        unreal.EditorAssetLibrary.save_asset(imc_path)
        
        result = {{"success": True, "message": f"已从 IMC 移除 {{removed_count}} 条 {{input_action_path}} 绑定"}}
        print(json.dumps(result, ensure_ascii=False))

except Exception as e:
    import traceback
    error_msg = f"{{str(e)}}\\n{{traceback.format_exc()}}"
    unreal.log_error(error_msg)
    result = {{"success": False, "message": error_msg}}
    print(json.dumps(result, ensure_ascii=False))
'''

        result = await connection.execute_python(code)
        return unwrap_result(result)
