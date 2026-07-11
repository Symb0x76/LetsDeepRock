"""资产操作工具（资产的创建、查找、属性读取）

本模块提供 UE 编辑器中资产级别的操作工具：
- 创建 DataAsset（如武器数据资产）
- 创建并填充 DataTable
- 搜索资产
- 读取资产属性
"""

import json
from typing import TYPE_CHECKING

from mcp.server import Server

if TYPE_CHECKING:
    from ..server import UnrealConnection


def register(server: "FastMCP", connection: "UnrealConnection"):
    """向 MCP Server 注册资产操作相关的工具"""

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
        """生成 UE Python 中查找类的代码片段

        尝试多种常见模块路径来加载类。
        """
        return f'''
# --- 尝试查找类: {class_name} ---
_parent_class = None
_cls_name = "{class_name}"
_paths_to_try = [
    _cls_name,                                        # 直接类名（已加载时可用）
    "/Script/Engine." + _cls_name,                    # 引擎类
    "/Script/CoreUObject." + _cls_name,              # 核心类
    "/Script/LetsDeepRock." + _cls_name,             # 项目 C++ 类
    "/Script/NavigationSystem." + _cls_name,         # 导航类
    "/Script/AIModule." + _cls_name,                 # AI 类
    "/Script/GameplayAbilities." + _cls_name,        # GAS 类
    "/Script/EnhancedInput." + _cls_name,            # 输入类
    "/Script/UMG." + _cls_name,                      # UMG 类
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
    async def create_data_asset(
        asset_name: str,
        package_path: str,
        parent_class: str,
        properties: dict | None = None,
    ) -> str:
        """创建 DataAsset（如武器数据资产、角色属性资产等）。

        在指定路径下创建一个指定父类的 DataAsset，并可选择性设置属性值。

        Args:
            asset_name: 资产名称（如 "DA_Shotgun"、"DA_DefaultWeapon"）
            package_path: 保存路径（如 "/Game/Variant_Weapon/Weapons"）
            parent_class: 父类名（如 "WeaponDataAsset"、"CharacterAttributeDataAsset"）
            properties: 可选的属性键值对字典，创建后立即设置（如 {"Damage": 50, "FireRate": 0.5}）

        Returns:
            操作结果（含成功/失败信息和资产路径）
        """
        code = f'''
import unreal
import json

try:
{_build_lookup_code(parent_class)}

    # 创建 DataAssetFactory
    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", _parent_class)

    # 创建资产
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    new_asset = asset_tools.create_asset(
        "{asset_name}",
        "{package_path}",
        unreal.DataAsset,
        factory,
    )

    if new_asset is None:
        raise RuntimeError("create_asset 返回 None，可能路径不存在或名称冲突")

    # 设置属性
    _properties = {json.dumps(properties) if properties else "{}"}
    if _properties:
        _errors = []
        for _key, _value in _properties.items():
            try:
                new_asset.set_editor_property(_key, _value)
            except Exception as _e:
                _errors.append(f"  [{_key}]: {{_e}}")

    # 保存资产
    unreal.EditorAssetLibrary.save_loaded_asset(new_asset)

    _msg = f"DataAsset {{'{asset_name}'}} 创建成功"
    if _errors:
        _msg += "\\n属性设置警告:\\n" + "\\n".join(_errors)

    print(json.dumps({{"success": True, "asset_path": "{package_path}/{asset_name}", "message": _msg}}))

except Exception as _ex:
    print(json.dumps({{"success": False, "error": str(_ex)}}))
'''
        result = await connection.execute_python(code)
        return _unwrap(result)

    @server.tool()
    async def create_data_table(
        asset_name: str,
        package_path: str,
        row_struct_name: str,
        rows: dict | None = None,
    ) -> str:
        """创建 DataTable 并填充行数据。

        在指定路径下创建一个指定行结构类型的 DataTable，并可通过 rows 参数预填充数据。

        Args:
            asset_name: 表名（如 "DT_CharacterAttributes"）
            package_path: 保存路径（如 "/Game/Variant_Weapon/Data"）
            row_struct_name: 行结构名（如 "CharacterAttributeRow"、"WeaponStatsRow"）
            rows: 可选的 {行名: {字段: 值}} 字典，用于预填充表数据

        Returns:
            操作结果（含成功/失败信息和表路径）
        """
        code = f'''
import unreal
import json

try:
{_build_lookup_code(row_struct_name)}

    # 创建 DataTable
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    new_dt = asset_tools.create_asset(
        "{asset_name}",
        "{package_path}",
        unreal.DataTable,
        unreal.DataTableFactory(),
    )

    if new_dt is None:
        raise RuntimeError("create_asset 返回 None，可能路径不存在或名称冲突")

    # 设置行结构类型
    # DataTable 的 RowStruct 属性在创建时通过 factory 设置，也可后续修改
    new_dt.set_editor_property("row_struct", _parent_class)

    # 保存以便结构生效
    unreal.EditorAssetLibrary.save_loaded_asset(new_dt)

    # 填充行数据（通过 CSV 字符串导入）
    _rows = {json.dumps(rows) if rows else "{{}}"}
    _filled_count = 0
    _fill_errors = []

    if _rows:
        # 获取行结构的所有字段名（用于构建 CSV 表头）
        _struct_fields = []
        try:
            for _prop in _parent_class.get_editor_property("children"):
                try:
                    _field_name = _prop.get_editor_property("name_private")
                    _struct_fields.append(str(_field_name))
                except:
                    pass
        except:
            pass

        # 如果没有获取到字段名，从第一行数据推断
        if not _struct_fields and _rows:
            _first_row = next(iter(_rows.values()))
            _struct_fields = list(_first_row.keys()) if isinstance(_first_row, dict) else []

        if _struct_fields:
            # 构建 CSV 字符串：第一列是行名（---），后续是字段
            _csv_lines = ["---," + ",".join([str(f) for f in _struct_fields])]

            for _row_name, _row_data in _rows.items():
                if isinstance(_row_data, dict):
                    _values = []
                    for _f in _struct_fields:
                        _v = _row_data.get(_f, "")
                        # 特殊处理布尔值
                        if isinstance(_v, bool):
                            _values.append("true" if _v else "false")
                        elif _v is None:
                            _values.append("")
                        else:
                            _values.append(str(_v))
                    _csv_lines.append(str(_row_name) + "," + ",".join(_values))
                else:
                    _fill_errors.append(f"  [{{_row_name}}]: 行数据不是字典类型")

            _csv_string = "\\n".join(_csv_lines)

            # 通过 CSV 字符串填充 DataTable
            try:
                import io
                unreal.DataTableFunctionLibrary.fill_data_table_from_csv_string(new_dt, _csv_string)
                _filled_count = len(_rows)
            except AttributeError:
                # 回退：尝试直接通过 CSV 文件路径导入
                try:
                    _temp_csv_path = unreal.Paths.project_intermediate_dir() + "/__temp_dt_import.csv"
                    # 无法写文件，跳过
                    _fill_errors.append("  CSV 导入 API 不可用（DataTableFunctionLibrary.fill_data_table_from_csv_string 不存在）")
                except:
                    _fill_errors.append("  CSV 导入 API 不可用")
        else:
            _fill_errors.append("  无法获取行结构字段名，跳过数据填充")

    # 再次保存（包含填充的数据）
    unreal.EditorAssetLibrary.save_loaded_asset(new_dt)

    _msg = f"DataTable {{'{asset_name}'}} 创建成功（行结构: {row_struct_name}）"
    if _filled_count > 0:
        _msg += f"，已填充 {{_filled_count}} 行"

    print(json.dumps({{"success": True, "asset_path": "{package_path}/{asset_name}", "message": _msg, "rows_filled": _filled_count, "fill_errors": _fill_errors}}))

except Exception as _ex:
    print(json.dumps({{"success": False, "error": str(_ex)}}))
'''
        result = await connection.execute_python(code)
        return _unwrap(result)

    @server.tool()
    async def search_assets(
        class_names: list[str] | None = None,
        package_paths: list[str] | None = None,
        search_string: str = "",
    ) -> str:
        """在 UE 资产注册表中搜索资产。

        通过 Asset Registry 按类名、路径或关键词搜索资产。

        Args:
            class_names: 过滤的类名列表（如 ["DataAsset", "Blueprint"]），None 表示不过滤
            package_paths: 搜索的包路径列表（如 ["/Game/Data"]），None 表示搜全部
            search_string: 搜索关键词，在资产名称中匹配（空字符串表示不过滤）

        Returns:
            JSON 格式的搜索结果列表
        """
        code = f'''
import unreal
import json

try:
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.wait_for_completion()

    # 构建过滤器
    filter_obj = unreal.ARFilter()

    _cls_list = {json.dumps(class_names) if class_names else "[]"}
    if _cls_list:
        _class_objects = []
        for _cn in _cls_list:
            _found = unreal.load_class(None, _cn)
            if _found is None:
                _found = unreal.load_class(None, "/Script/Engine." + _cn)
            if _found is None:
                _found = unreal.load_class(None, "/Script/CoreUObject." + _cn)
            if _found is not None:
                _class_objects.append(_found)

        if _class_objects:
            filter_obj.set_editor_property("class_names", _class_objects)
            filter_obj.set_editor_property("recursive_classes", True)

    _pkg_paths = {json.dumps(package_paths) if package_paths else "[]"}
    if _pkg_paths:
        filter_obj.set_editor_property("package_paths", _pkg_paths)
        filter_obj.set_editor_property("recursive_paths", True)

    # 执行搜索
    assets = registry.get_assets(filter_obj)

    # 关键词过滤
    _search = "{search_string}".lower()
    _results = []
    _limit = 100  # 最多返回 100 条

    for _asset in assets:
        if len(_results) >= _limit:
            break
        _name = str(_asset.asset_name)
        _path = str(_asset.package_name)
        _cls = str(_asset.asset_class_path.asset_name) if hasattr(_asset, "asset_class_path") else str(_asset.asset_class)

        if _search and _search not in _name.lower() and _search not in _path.lower():
            continue

        _results.append({{
            "asset_name": _name,
            "package_path": _path,
            "asset_class": _cls,
            "full_path": _path + "/" + _name,
        }})

    _total = len(_results)
    print(json.dumps({{"success": True, "count": _total, "assets": _results}}))

except Exception as _ex:
    print(json.dumps({{"success": False, "error": str(_ex)}}))
'''
        result = await connection.execute_python(code)
        return _unwrap(result)

    @server.tool()
    async def get_asset_properties(
        asset_path: str,
        property_names: list[str] | None = None,
    ) -> str:
        """获取资产的属性值。

        加载指定资产并读取其编辑器属性。

        Args:
            asset_path: 资产完整路径（如 "/Game/Data/DA_DefaultWeapon"）
            property_names: 要读取的属性名列表，None 表示返回所有可通过 get_editor_property 读取的属性

        Returns:
            JSON 格式的属性键值对
        """
        code = f'''
import unreal
import json

try:
    asset = unreal.EditorAssetLibrary.load_asset("{asset_path}")
    if asset is None:
        raise RuntimeError("无法加载资产: {asset_path}")

    _properties = {{}}
    _target_names = {json.dumps(property_names)} if property_names is not None else None

    if _target_names:
        # 读取指定属性
        _errors = []
        for _name in _target_names:
            try:
                _val = asset.get_editor_property(_name)
                # 尝试将值转为可序列化格式
                if hasattr(_val, "get_name"):
                    _properties[_name] = _val.get_name()
                elif hasattr(_val, "__str__"):
                    _properties[_name] = str(_val)
                else:
                    _properties[_name] = repr(_val)
            except Exception as _e:
                _errors.append(f"  [{{_name}}]: {{_e}}")
                _properties[_name] = "[无法读取]"

        if _errors:
            print(json.dumps({{"success": True, "asset_path": "{asset_path}", "properties": _properties, "warnings": _errors}}))
        else:
            print(json.dumps({{"success": True, "asset_path": "{asset_path}", "properties": _properties}}))
    else:
        # 读取所有可读属性（通过 dir 探测）
        _errors = []
        _all_attrs = dir(asset)
        for _attr in _all_attrs:
            if _attr.startswith("_"):
                continue
            if _attr in ("get_editor_property", "set_editor_property", "get_class", "get_outer", "get_name", "rename"):
                continue
            try:
                # 跳过方法和内部属性
                _val = getattr(asset, _attr, None)
                if callable(_val):
                    continue
                # 尝试通过 get_editor_property 读取
                _editor_val = asset.get_editor_property(_attr)
                if hasattr(_editor_val, "get_name"):
                    _properties[_attr] = _editor_val.get_name()
                elif hasattr(_editor_val, "__str__"):
                    _properties[_attr] = str(_editor_val)
                else:
                    _properties[_attr] = repr(_editor_val)
            except:
                pass  # 跳过无法读取的属性

        print(json.dumps({{"success": True, "asset_path": "{asset_path}", "property_count": len(_properties), "properties": _properties}}))

except Exception as _ex:
    print(json.dumps({{"success": False, "error": str(_ex)}}))
'''
        result = await connection.execute_python(code)
        return _unwrap(result)
