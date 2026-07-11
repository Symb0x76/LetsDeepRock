"""关卡操作工具（Actor 放置、导航构建、材质创建、Blackboard 创建）"""

import json
from typing import TYPE_CHECKING

from ._helpers import unwrap_result

if TYPE_CHECKING:
    from ..server import UnrealConnection


def register(server, connection: "UnrealConnection"):
    """向 MCP Server 注册关卡操作相关的工具"""

    # ================================================================
    #  1. spawn_actor_in_level — 在当前关卡中放置 Actor
    # ================================================================
    @server.tool()
    async def spawn_actor_in_level(
        actor_class: str,
        location: list[float] | None = None,
        rotation: list[float] | None = None,
        scale: list[float] | None = None,
        label: str | None = None,
    ) -> str:
        """在当前关卡中放置一个 Actor。

        支持 UE 原生 Actor 类名（如 "NavMeshBoundsVolume", "StaticMeshActor"）
        以及 Blueprint 资源路径（如 "/Game/Blueprints/BP_MyActor.BP_MyActor_C"）。

        Args:
            actor_class: Actor 类名或 Blueprint 资源路径（带 _C 后缀）
            location: XYZ 位置，默认 [0, 0, 0]
            rotation: Pitch/Yaw/Roll 旋转角度，默认 [0, 0, 0]
            scale: XYZ 缩放，默认 [1, 1, 1]
            label: 可选的 Actor 标签/名称

        Returns:
            执行结果字符串，包含生成的 Actor 信息
        """
        loc = location if location else [0, 0, 0]
        rot = rotation if rotation else [0, 0, 0]
        scl = scale if scale else [1, 1, 1]

        code = f'''import unreal
import json

actor_class_path = {repr(actor_class)}
spawn_location = unreal.Vector({loc[0]}, {loc[1]}, {loc[2]})
spawn_rotation = unreal.Rotator({rot[0]}, {rot[1]}, {rot[2]})
spawn_scale = unreal.Vector({scl[0]}, {scl[1]}, {scl[2]})
label = {repr(label) if label else 'None'}

editor_actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

# 判断是 Blueprint 路径还是原生类名
if "/" in actor_class_path:
    # Blueprint 路径：使用 load_object 加载生成的类
    try:
        actor_class_obj = unreal.load_object(None, actor_class_path)
        if actor_class_obj is None:
            print(json.dumps({{"success": False, "message": f"无法加载 Blueprint: {{actor_class_path}}"}}))
            raise SystemExit(0)
    except SystemExit:
        raise
    except Exception as e:
        print(json.dumps({{"success": False, "message": f"加载 Blueprint 失败: {{e}}"}}))
        raise SystemExit(0)
else:
    # 原生类名：使用 load_class 加载
    try:
        actor_class_obj = unreal.load_class(None, actor_class_path)
        if actor_class_obj is None:
            # 尝试补充命名空间前缀
            actor_class_obj = unreal.load_class(None, "/Script/Engine." + actor_class_path)
        if actor_class_obj is None:
            print(json.dumps({{"success": False, "message": f"无法找到 Actor 类: {{actor_class_path}}"}}))
            raise SystemExit(0)
    except SystemExit:
        raise
    except Exception as e:
        print(json.dumps({{"success": False, "message": f"加载 Actor 类失败: {{e}}"}}))
        raise SystemExit(0)

# 生成 Actor
try:
    actor = editor_actor_subsystem.spawn_actor_from_class(actor_class_obj, spawn_location, spawn_rotation)
    if actor is None:
        print(json.dumps({{"success": False, "message": "生成 Actor 失败（返回 None）"}}))
        raise SystemExit(0)

    actor.set_actor_scale3d(spawn_scale)

    if label:
        actor.set_actor_label(label)

    result = {{
        "success": True,
        "actor_name": str(actor.get_name()),
        "actor_label": str(actor.get_actor_label()),
        "actor_class": str(actor.get_class().get_name()),
        "location": [spawn_location.x, spawn_location.y, spawn_location.z],
        "rotation": [spawn_rotation.roll, spawn_rotation.pitch, spawn_rotation.yaw],
        "scale": [spawn_scale.x, spawn_scale.y, spawn_scale.z],
    }}
    print(json.dumps(result))
except Exception as e:
    print(json.dumps({{"success": False, "message": f"生成 Actor 时出错: {{e}}"}}))
'''
        result = await connection.execute_python(code)
        return unwrap_result(result)

    # ================================================================
    #  2. build_navigation — 构建 NavMesh 导航网格
    # ================================================================
    @server.tool()
    async def build_navigation() -> str:
        """构建当前关卡中的 NavMesh 导航网格。

        在关卡中放置 NavMeshBoundsVolume 后，调用此工具来生成导航数据。
        使用两种方式确保构建成功：控制台命令 + NavigationSystem API。

        Returns:
            执行结果字符串
        """
        code = '''import unreal
import json

result = {"success": True, "messages": []}

# 方法1：通过控制台命令构建导航
try:
    unreal.SystemLibrary.execute_console_command(None, "RebuildNavigation")
    result["messages"].append("控制台命令 RebuildNavigation 已执行")
except Exception as e:
    result["messages"].append(f"控制台命令执行异常: {e}")

# 方法2：通过 NavigationSystem API
try:
    world = unreal.EditorLevelLibrary.get_editor_world()
    if world:
        nav_sys = unreal.NavigationSystemV1.get_navigation_system(world)
        if nav_sys:
            nav_sys.build()
            result["messages"].append("NavigationSystemV1.build() 已调用")
        else:
            result["messages"].append("未找到 NavigationSystemV1（关卡中可能没有 NavMeshBoundsVolume）")
            result["success"] = False
    else:
        result["messages"].append("无法获取编辑器世界")
        result["success"] = False
except Exception as e:
    result["messages"].append(f"NavigationSystem API 调用异常: {e}")
    result["success"] = False

print(json.dumps(result))
'''
        result = await connection.execute_python(code)
        return unwrap_result(result)

    # ================================================================
    #  3. create_material — 创建简单的基础颜色材质
    # ================================================================
    @server.tool()
    async def create_material(
        asset_name: str,
        package_path: str = "/Game/Materials/",
        base_color: list[float] | None = None,
    ) -> str:
        """创建一个简单的基础颜色材质资产。

        在指定路径下创建一个新的 Material 资产，并设置基础颜色（Base Color）。
        使用 Constant3Vector 节点连接到材质的基础颜色引脚。

        Args:
            asset_name: 材质名称（如 "M_GridTree"），不需要加 .M_ 前缀
            package_path: 保存路径（如 "/Game/Materials/"），默认为 "/Game/Materials/"
            base_color: RGB 颜色值，范围为 0-1，默认 [0.5, 0.5, 0.5]（灰色）

        Returns:
            执行结果字符串，包含创建的材质信息
        """
        color = base_color if base_color else [0.5, 0.5, 0.5]
        # 确保路径以 / 结尾
        if not package_path.endswith("/"):
            package_path += "/"

        code = f'''import unreal
import json

asset_name = {repr(asset_name)}
package_path = {repr(package_path)}
base_color = [{color[0]}, {color[1]}, {color[2]}]

result = {{"success": True, "messages": []}}

try:
    # 创建材质资产
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.MaterialFactoryNew()
    material = asset_tools.create_asset(asset_name, package_path, None, factory)

    if material is None:
        print(json.dumps({{"success": False, "message": "创建材质失败（返回 None），可能名称已存在"}}))
        raise SystemExit(0)

    # 使用 MaterialEditingLibrary 设置基础颜色
    mel = unreal.MaterialEditingLibrary
    # 创建 Constant3Vector 表达式节点
    color_node = mel.create_material_expression(
        material,
        unreal.MaterialExpressionConstant3Vector,
        node_pos_x=-400,
        node_pos_y=0,
    )

    if color_node:
        color_node.set_editor_property("constant", unreal.LinearColor(base_color[0], base_color[1], base_color[2], 1.0))
        # 连接到材质的基础颜色引脚
        mel.connect_material_property(
            color_node,
            "RGB",  # Constant3Vector 的输出引脚
            unreal.MaterialProperty.MP_BASE_COLOR,
        )
        result["messages"].append(f"Base Color 已设置为 RGB({{base_color[0]}}, {{base_color[1]}}, {{base_color[2]}})")

    # 保存资产
    mel.layout_material_expressions(material)
    unreal.EditorAssetLibrary.save_asset(package_path + asset_name)

    result["asset_path"] = package_path + asset_name
    result["asset_name"] = asset_name
    result["messages"].append(f"材质 {{asset_name}} 已创建并保存")

except SystemExit:
    raise
except Exception as e:
    result["success"] = False
    result["messages"].append(f"创建材质时出错: {{e}}")

print(json.dumps(result))
'''
        result = await connection.execute_python(code)
        return unwrap_result(result)

    # ================================================================
    #  4. create_blackboard — 创建 AI Blackboard 资产并添加键
    # ================================================================
    @server.tool()
    async def create_blackboard(
        asset_name: str,
        package_path: str = "/Game/AI/Blackboards/",
        keys: list[dict] | None = None,
    ) -> str:
        """创建 AI Blackboard 资产并添加键。

        在指定路径下创建 BlackboardData 资产，可通过 keys 参数预定义黑板键。
        然后手动在 Behavior Tree 中引用此 Blackboard 资产。

        Args:
            asset_name: Blackboard 名称（如 "BB_Enemy"）
            package_path: 保存路径（如 "/Game/AI/Blackboards/"），默认为 "/Game/AI/Blackboards/"
            keys: 键列表，每个键为 dict，格式：
                {{"name": "TargetActor", "type": "Object", "base_class": "Actor"}},
                {{"name": "PatrolLocation", "type": "Vector"}},
                {{"name": "IsAlerted", "type": "Bool"}}
                支持的 type: Object, Vector, Float, Bool, Int, String, Enum, Class, Name

        Returns:
            执行结果字符串，包含创建的 Blackboard 信息
        """
        if not package_path.endswith("/"):
            package_path += "/"

        bb_keys = keys if keys else []

        # 键类型到 UE 枚举的映射
        type_mapping = {
            "Object": "unreal.BlackboardKeyType.Object",
            "Vector": "unreal.BlackboardKeyType.Vector",
            "Float": "unreal.BlackboardKeyType.Float",
            "Bool": "unreal.BlackboardKeyType.Bool",
            "Int": "unreal.BlackboardKeyType.Int",
            "String": "unreal.BlackboardKeyType.String",
            "Enum": "unreal.BlackboardKeyType.Enum",
            "Class": "unreal.BlackboardKeyType.Class",
            "Name": "unreal.BlackboardKeyType.Name",
        }

        code = f'''import unreal
import json

asset_name = {repr(asset_name)}
package_path = {repr(package_path)}
keys_data = {json.dumps(bb_keys)}
type_mapping = {json.dumps(type_mapping)}

result = {{"success": True, "messages": [], "keys": []}}

try:
    # 创建 BlackboardData 资产
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.BlackboardDataFactory()
    bb = asset_tools.create_asset(asset_name, package_path, None, factory)

    if bb is None:
        print(json.dumps({{"success": False, "message": "创建 Blackboard 失败（返回 None），可能名称已存在"}}))
        raise SystemExit(0)

    result["asset_path"] = package_path + asset_name
    result["messages"].append(f"Blackboard {{asset_name}} 已创建")

    # 添加键
    for key_info in keys_data:
        key_name = key_info.get("name", "")
        key_type = key_info.get("type", "Object")
        base_class = key_info.get("base_class", "Actor")

        if not key_name:
            result["messages"].append(f"跳过：键名称为空")
            continue

        # 查找键类型枚举值
        enum_path = type_mapping.get(key_type)
        if not enum_path:
            result["messages"].append(f"跳过键 '{{key_name}}'：不支持的键类型 '{{key_type}}'，支持的类型: {{', '.join(type_mapping.keys())}}")
            continue

        try:
            # 通过 unreal 枚举获取 KeyType 对象
            key_type_enum = eval(enum_path)

            # 添加键到 Blackboard
            added_key = bb.add_key(key_name, key_type_enum)

            key_result = {{"name": key_name, "type": key_type, "added": True}}

            # 如果键类型是 Object 且指定了 base_class，设置 BaseClass
            if key_type == "Object" and base_class:
                try:
                    class_obj = unreal.load_class(None, "/Script/Engine." + base_class)
                    if class_obj:
                        bb.set_key_class(added_key, class_obj)
                        key_result["base_class"] = base_class
                except Exception as e:
                    result["messages"].append(f"设置键 '{{key_name}}' 的 BaseClass 时出错: {{e}}")

            result["keys"].append(key_result)
            result["messages"].append(f"键 '{{key_name}}' ({{key_type}}) 已添加")

        except Exception as e:
            result["messages"].append(f"添加键 '{{key_name}}' 失败: {{e}}")

    # 保存资产
    unreal.EditorAssetLibrary.save_asset(package_path + asset_name)
    result["messages"].append(f"Blackboard {{asset_name}} 已保存")

except SystemExit:
    raise
except Exception as e:
    result["success"] = False
    result["messages"].append(f"创建 Blackboard 时出错: {{e}}")

print(json.dumps(result))
'''
        result = await connection.execute_python(code)
        return unwrap_result(result)
