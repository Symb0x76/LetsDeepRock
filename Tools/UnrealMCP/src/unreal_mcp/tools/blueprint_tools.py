"""蓝图操作工具（蓝图创建、编译、组件添加、Class Defaults 设置）

本模块提供 UE 编辑器中 Blueprint 级别的操作工具：
- 创建 Blueprint 类
- 设置 Class Defaults 属性
- 添加组件到 Blueprint
- 编译 Blueprint
"""

import json
from typing import TYPE_CHECKING

from mcp.server import Server

if TYPE_CHECKING:
    from ..server import UnrealConnection


def register(server: "FastMCP", connection: "UnrealConnection"):
    """向 MCP Server 注册蓝图操作相关的工具"""

    # ================================================================
    #  通用辅助
    # ================================================================

    def _unwrap(result) -> str:
        """将 execute_python 返回的 dict 解包为字符串结果"""
        if isinstance(result, dict):
            if result.get("success"):
                return result.get("result", "操作完成")
            else:
                return f"错误: {result.get('result', '未知错误')}"
        return str(result)

    def _build_lookup_code(class_name: str) -> str:
        """生成 UE Python 中查找类的代码片段"""
        return f'''
# --- 尝试查找类: {class_name} ---
_parent_class = None
_cls_name = "{class_name}"
_paths_to_try = [
    _cls_name,
    "/Script/Engine." + _cls_name,
    "/Script/CoreUObject." + _cls_name,
    "/Script/LetsDeepRock." + _cls_name,
    "/Script/AIModule." + _cls_name,
    "/Script/GameplayAbilities." + _cls_name,
    "/Script/EnhancedInput." + _cls_name,
    "/Script/UMG." + _cls_name,
    "/Script/NavigationSystem." + _cls_name,
    "/Script/GameplayTasks." + _cls_name,
]
for _path in _paths_to_try:
    try:
        _parent_class = unreal.load_class(None, _path)
        if _parent_class is not None:
            break
    except:
        pass

if _parent_class is None:
    raise RuntimeError("无法找到类: " + _cls_name + "（尝试路径: " + str(_paths_to_try) + "）")
'''

    # ================================================================
    #  工具定义
    # ================================================================

    @server.tool()
    async def create_blueprint(
        asset_name: str,
        package_path: str,
        parent_class: str,
    ) -> str:
        """创建 Blueprint 类资产。

        在指定路径下创建一个指定父类的 Blueprint。

        Args:
            asset_name: Blueprint 名称（如 "BP_EnemyMelee"、"BP_WeaponBase"）
            package_path: 保存路径（如 "/Game/Variant_Weapon/Blueprints"）
            parent_class: 父类名（如 "Character"、"WeaponNPC"、"GameplayEffect"、"EnemyAIController"）

        Returns:
            操作结果（含成功/失败信息和 BP 路径）
        """
        code = f'''
import unreal
import json

try:
{_build_lookup_code(parent_class)}

    # 创建 BlueprintFactory
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", _parent_class)

    # 创建 Blueprint 资产
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    new_bp = asset_tools.create_asset(
        "{asset_name}",
        "{package_path}",
        unreal.Blueprint,
        factory,
    )

    if new_bp is None:
        raise RuntimeError("create_asset 返回 None，可能路径不存在或名称冲突")

    # 保存
    unreal.EditorAssetLibrary.save_loaded_asset(new_bp)

    # 获取父类名称用于日志
    _parent_name = _parent_class.get_name()

    print(json.dumps({{
        "success": True,
        "asset_path": "{package_path}/{asset_name}",
        "parent_class": _parent_name,
        "message": f"Blueprint {{'{asset_name}'}} 创建成功（父类: {{_parent_name}}）",
    }}))

except Exception as _ex:
    print(json.dumps({{"success": False, "error": str(_ex)}}))
'''
        result = await connection.execute_python(code)
        return _unwrap(result)

    @server.tool()
    async def set_blueprint_defaults(
        asset_path: str,
        properties: dict,
    ) -> str:
        """设置 Blueprint 的 Class Defaults 属性值。

        加载 Blueprint 并通过其 CDO（Class Default Object）设置默认属性。

        Args:
            asset_path: Blueprint 资产路径（如 "/Game/Blueprints/BP_EnemyMelee"）
            properties: {属性名: 值} 字典，键值对应 Blueprint Class Defaults 面板中的属性

        Returns:
            操作结果（含设置了多少个属性）
        """
        code = f'''
import unreal
import json

try:
    bp = unreal.EditorAssetLibrary.load_asset("{asset_path}")
    if bp is None:
        raise RuntimeError("无法加载 Blueprint: {asset_path}")

    if not isinstance(bp, unreal.Blueprint):
        raise RuntimeError("指定资产不是 Blueprint 类型")

    # 获取生成的类
    _generated_class = bp.generated_class()
    if _generated_class is None:
        raise RuntimeError("Blueprint 未生成类，请先编译")

    # 获取 CDO
    cdo = unreal.get_default_object(_generated_class)
    if cdo is None:
        raise RuntimeError("无法获取 CDO")

    _properties = {json.dumps(properties)}
    _set_count = 0
    _errors = []

    for _key, _value in _properties.items():
        try:
            cdo.set_editor_property(_key, _value)
            _set_count += 1
        except Exception as _e:
            _errors.append(f"  [{{_key}}]: {{_e}}")

    # 编译并保存
    unreal.KismetSystemLibrary.compile_blueprint(bp)
    unreal.EditorAssetLibrary.save_loaded_asset(bp)

    _msg = f"已设置 {{_set_count}}/{{len(_properties)}} 个属性"
    if _errors:
        _msg += "\\n警告:\\n" + "\\n".join(_errors)

    print(json.dumps({{
        "success": True,
        "asset_path": "{asset_path}",
        "set_count": _set_count,
        "message": _msg,
    }}))

except Exception as _ex:
    print(json.dumps({{"success": False, "error": str(_ex)}}))
'''
        result = await connection.execute_python(code)
        return _unwrap(result)

    @server.tool()
    async def add_component_to_blueprint(
        asset_path: str,
        component_class: str,
        component_name: str | None = None,
    ) -> str:
        """给 Blueprint 添加组件。

        通过 SimpleConstructionScript（SCS）向 Blueprint 添加一个新组件节点。

        Args:
            asset_path: Blueprint 资产路径（如 "/Game/Blueprints/BP_EnemyMelee"）
            component_class: 组件类名（如 "AttributeRegenComponent"、"AIPerceptionStimuliSourceComponent"、"SkeletalMeshComponent"）
            component_name: 可选的组件显示名称，默认使用类名

        Returns:
            操作结果（含成功/失败信息）
        """
        code = f'''
import unreal
import json

try:
    bp = unreal.EditorAssetLibrary.load_asset("{asset_path}")
    if bp is None:
        raise RuntimeError("无法加载 Blueprint: {asset_path}")

    if not isinstance(bp, unreal.Blueprint):
        raise RuntimeError("指定资产不是 Blueprint 类型")

{_build_lookup_code(component_class)}

    # 确定组件名称
    _comp_name = "{component_name}" if "{component_name}" else _cls_name.split(".")[-1]
    # 确保名称唯一
    _base_name = _comp_name
    _scs = bp.get_editor_property("simple_construction_script")
    _existing_names = set()
    if _scs is not None:
        try:
            _all_nodes = _scs.get_all_nodes()
            for _node in _all_nodes:
                try:
                    _existing_names.add(str(_node.get_editor_property("component_template").get_name()))
                except:
                    pass
        except:
            pass

    _counter = 1
    while _comp_name in _existing_names:
        _comp_name = f"{{_base_name}}_{{_counter}}"
        _counter += 1

    # 创建组件模板对象
    _comp_template = unreal.new_object(_parent_class, bp, name=_comp_name)
    if _comp_template is None:
        raise RuntimeError(f"无法创建组件模板: {{_cls_name}}")

    # 获取或创建 SimpleConstructionScript
    _scs = bp.get_editor_property("simple_construction_script")
    if _scs is None:
        _scs = unreal.SimpleConstructionScript()
        bp.set_editor_property("simple_construction_script", _scs)

    # 创建 SCS 节点并添加组件模板
    _scs_node = _scs.create_node(_comp_template, _comp_name)
    if _scs_node is None:
        raise RuntimeError("SCS create_node 返回 None")

    _scs.add_node(_scs_node)

    # 编译并保存
    unreal.KismetSystemLibrary.compile_blueprint(bp)
    unreal.EditorAssetLibrary.save_loaded_asset(bp)

    print(json.dumps({{
        "success": True,
        "asset_path": "{asset_path}",
        "component_class": _cls_name,
        "component_name": _comp_name,
        "message": f"组件 {{_cls_name}}（{{_comp_name}}）已添加到 Blueprint",
    }}))

except Exception as _ex:
    print(json.dumps({{"success": False, "error": str(_ex)}}))
'''
        result = await connection.execute_python(code)
        return _unwrap(result)

    @server.tool()
    async def compile_blueprint(
        asset_path: str,
    ) -> str:
        """编译 Blueprint 资产。

        加载并编译指定的 Blueprint，编译后自动保存。

        Args:
            asset_path: Blueprint 资产路径（如 "/Game/Blueprints/BP_EnemyMelee"）

        Returns:
            操作结果（编译成功/失败信息）
        """
        code = f'''
import unreal
import json

try:
    bp = unreal.EditorAssetLibrary.load_asset("{asset_path}")
    if bp is None:
        raise RuntimeError("无法加载 Blueprint: {asset_path}")

    if not isinstance(bp, unreal.Blueprint):
        raise RuntimeError("指定资产不是 Blueprint 类型")

    # 执行编译
    unreal.KismetSystemLibrary.compile_blueprint(bp)

    # 保存
    unreal.EditorAssetLibrary.save_loaded_asset(bp)

    _bp_name = bp.get_name()
    print(json.dumps({{
        "success": True,
        "asset_path": "{asset_path}",
        "blueprint_name": _bp_name,
        "message": f"Blueprint {{_bp_name}} 编译成功",
    }}))

except Exception as _ex:
    print(json.dumps({{"success": False, "error": str(_ex)}}))
'''
        result = await connection.execute_python(code)
        return _unwrap(result)
