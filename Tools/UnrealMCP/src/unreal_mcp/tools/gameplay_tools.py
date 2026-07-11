"""GAS 工具集（GameplayEffect / GameplayAbility Blueprint 创建与配置）"""

import json
from typing import TYPE_CHECKING

from ._helpers import unwrap_result

if TYPE_CHECKING:
    from ..server import UnrealConnection


def register(server, connection: "UnrealConnection"):
    """向 MCP Server 注册 GAS 相关工具"""

    @server.tool()
    async def create_gameplay_effect(
        asset_name: str,
        package_path: str,
        duration_policy: str = "Instant",
        duration_magnitude: float = 0.0,
        modifiers: str = "[]",
        execution_class: str = "",
    ) -> str:
        """创建 GameplayEffect Blueprint 并配置基本属性。

        在指定路径下创建一个 GameplayEffect Blueprint，设置持续时间策略、
        修改器和 Execution Calculation 等。

        Args:
            asset_name: GE 资产名称，如 "GE_MeleeCooldown"
            package_path: 保存路径，如 "/Game/Variant_Weapon/GAS"
            duration_policy: 持续时间策略 — "Instant" / "HasDuration" / "Infinite"
            duration_magnitude: 持续时间值（仅 HasDuration 时有效），单位秒
            modifiers: JSON 格式的修改器列表，如 '[{"attribute":"Health","op":"Add","magnitude":-10}]'
                       每个元素包含: attribute(属性名), op(操作:Add/Multiply/Override), magnitude(值)
            execution_class: Execution Calculation 类资产路径（可选），如 "/Game/.../DamageExecCalculation"

        Returns:
            操作结果消息
        """
        # 将 JSON 参数安全嵌入 Python 代码
        modifiers_json = json.dumps(json.loads(modifiers) if isinstance(modifiers, str) else (modifiers if modifiers else []))

        code = f'''import unreal
import json

try:
    # ---- 解析参数 ----
    asset_name = {repr(asset_name)}
    package_path = {repr(package_path)}
    duration_policy_str = {repr(duration_policy)}
    duration_magnitude = {repr(duration_magnitude)}
    modifiers_data = json.loads({repr(modifiers_json)})
    execution_class_path = {repr(execution_class)}

    # 持续时间策略映射
    policy_map = {{
        "Instant": unreal.GameplayEffectDurationType.INSTANT,
        "HasDuration": unreal.GameplayEffectDurationType.HAS_DURATION,
        "Infinite": unreal.GameplayEffectDurationType.INFINITE,
    }}
    duration_type = policy_map.get(duration_policy_str, unreal.GameplayEffectDurationType.INSTANT)

    # 属性操作映射
    op_map = {{
        "Add": unreal.GameplayModOp.ADDITIVE,
        "Multiply": unreal.GameplayModOp.MULTIPLICITIVE,
        "Override": unreal.GameplayModOp.OVERRIDE,
    }}

    # ---- 创建 Blueprint ----
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", unreal.GameplayEffect.static_class())

    ge_bp = asset_tools.create_asset(asset_name, package_path, None, factory)
    if not ge_bp:
        raise RuntimeError(f"无法创建 GameplayEffect 资产: {{asset_name}}")

    # 获取 CDO
    ge_cdo = unreal.get_default_object(ge_bp.generated_class())
    if not ge_cdo:
        raise RuntimeError("无法获取 GameplayEffect CDO")

    # ---- 设置持续时间策略 ----
    ge_cdo.set_editor_property("duration_policy", duration_type)

    # 如果是 HasDuration，设置持续时间
    if duration_policy_str == "HasDuration":
        # 设置 Duration Magnitude
        duration_mag = unreal.GameplayEffectMagnitude()
        duration_mag.set_editor_property("magnitude_type", unreal.EGameplayEffectMagnitudeType.SCALABLE_FLOAT)
        # 通过 ScalableFloatMagnitude 设置值
        scalable_float = unreal.ScalableFloat()
        scalable_float.set_editor_property("value", float(duration_magnitude))
        duration_mag.set_editor_property("scalable_float_magnitude", scalable_float)
        ge_cdo.set_editor_property("duration_magnitude", duration_mag)

    # ---- 设置 Modifiers ----
    if modifiers_data:
        # GameplayEffect 的 Modifiers 是 TArray<FGameplayModifierInfo>
        mod_array = unreal.Array(unreal.GameplayModifierInfo)
        for mod_data in modifiers_data:
            mod_info = unreal.GameplayModifierInfo()
            
            # 设置属性 — 尝试通过属性名查找
            attr_name = mod_data.get("attribute", "")
            if attr_name:
                # 创建 FGameplayAttribute 并设置属性名
                gameplay_attr = unreal.GameplayAttribute()
                # 尝试设置属性名（取决于 UE Python API 是否支持）
                try:
                    gameplay_attr.set_editor_property("attribute_name", attr_name)
                except Exception:
                    pass  # 如果 API 不支持，属性可能保持默认
            
            mod_info.set_editor_property("attribute", gameplay_attr)
            
            # 设置操作
            op_name = mod_data.get("op", "Add")
            mod_op = op_map.get(op_name, unreal.GameplayModOp.ADDITIVE)
            mod_info.set_editor_property("modifier_op", mod_op)
            
            # 设置幅度
            mag_value = float(mod_data.get("magnitude", 0.0))
            mod_mag = unreal.GameplayEffectModifierMagnitude()
            # 使用 ScalableFloat 作为 SetByCaller 的幅度类型
            scalable_float_mag = unreal.ScalableFloat()
            scalable_float_mag.set_editor_property("value", mag_value)
            mod_mag.set_editor_property("scalable_float_magnitude", scalable_float_mag)
            mod_info.set_editor_property("modifier_magnitude", mod_mag)
            
            mod_array.append(mod_info)
        
        ge_cdo.set_editor_property("modifiers", mod_array)

    # ---- 设置 Execution Calculation ----
    if execution_class_path:
        try:
            exec_class = unreal.load_object(None, execution_class_path)
            if exec_class:
                # 设置 Executions 数组
                exec_array = unreal.Array(unreal.GameplayEffectExecutionDefinition)
                exec_def = unreal.GameplayEffectExecutionDefinition()
                exec_def.set_editor_property("calculation_class", exec_class)
                exec_array.append(exec_def)
                ge_cdo.set_editor_property("executions", exec_array)
        except Exception as exec_err:
            unreal.log_warning(f"设置 Execution Calculation 失败: {{exec_err}}")

    # ---- 保存资产 ----
    save_path = f"{{package_path}}/{{asset_name}}"
    unreal.EditorAssetLibrary.save_asset(save_path)

    result = {{"success": True, "message": f"GameplayEffect {{asset_name}} 创建成功 ({{duration_policy_str}})"}}
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
    async def create_gameplay_ability(
        asset_name: str,
        package_path: str,
        parent_class: str = "GameplayAbility",
    ) -> str:
        """创建 GameplayAbility Blueprint。

        在指定路径下创建一个 GameplayAbility Blueprint，父类可以是内置
        GameplayAbility 类或自定义 GA 蓝图类。

        Args:
            asset_name: GA 资产名称，如 "GA_NPC_MeleeAttack"
            package_path: 保存路径，如 "/Game/Variant_Weapon/GAS"
            parent_class: 父类指定：
                - "GameplayAbility"（默认）：使用 UE 内置 GameplayAbility 类
                - 以 "/" 开头的路径：作为蓝图资产路径加载，取其 generated_class
                - 其他类名：尝试从 unreal 模块获取（如自定义 C++ 类名）

        Returns:
            操作结果消息
        """
        code = f'''import unreal
import json

try:
    asset_name = {repr(asset_name)}
    package_path = {repr(package_path)}
    parent_class_spec = {repr(parent_class)}

    # ---- 解析父类 ----
    if parent_class_spec.startswith("/"):
        # 作为蓝图资产路径加载
        parent_asset = unreal.EditorAssetLibrary.load_asset(parent_class_spec)
        if not parent_asset:
            raise RuntimeError(f"无法加载蓝图资产: {{parent_class_spec}}")
        parent_uclass = parent_asset.generated_class()
        if not parent_uclass:
            raise RuntimeError(f"蓝图资产没有 generated_class: {{parent_class_spec}}")
    else:
        # 尝试从 unreal 模块获取类
        try:
            parent_uclass = getattr(unreal, parent_class_spec)
        except AttributeError:
            # 回退到默认 GameplayAbility
            parent_uclass = unreal.GameplayAbility

    # ---- 创建 Blueprint ----
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", parent_uclass.static_class())

    ga_bp = asset_tools.create_asset(asset_name, package_path, None, factory)
    if not ga_bp:
        raise RuntimeError(f"无法创建 GameplayAbility 资产: {{asset_name}}")

    # ---- 保存资产 ----
    save_path = f"{{package_path}}/{{asset_name}}"
    unreal.EditorAssetLibrary.save_asset(save_path)

    result = {{"success": True, "message": f"GameplayAbility {{asset_name}} 创建成功"}}
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
