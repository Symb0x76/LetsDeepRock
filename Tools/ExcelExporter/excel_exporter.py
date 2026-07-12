# -*- coding: utf-8 -*-
"""
LetsDeepRock Excel → CSV 导出工具 - 核心模块

解析 UE5 策划配置表 Excel (.xlsx)，导出为 UE DataTable 兼容的 CSV 格式。
支持 3 行表头格式（元数据 / 类型 / 字段名），自动类型转换和校验。

用法:
    # CLI 单文件导出
    python excel_exporter.py "DT_EnemyConfig.xlsx"

    # CLI 目录批量导出
    python excel_exporter.py "Config/Excel" --output "Config/Excel"

    # Python API
    from excel_exporter import parse_xlsx, export_csv, export_all
    data = parse_xlsx("DT_EnemyConfig.xlsx")
    export_csv(data, "DT_EnemyConfig.csv")
"""

import os
import re
import sys
from typing import Any, Dict, List, Optional, Tuple

import openpyxl


# ============================================================================
# 类型映射
# ============================================================================

# UE 类型 → 校验正则 / 说明
TYPE_VALIDATORS: Dict[str, str] = {
    "int32":    r"^-?\d+$",
    "float":    r"^-?\d+(\.\d+)?(e[+-]?\d+)?$",
    "bool":     r"^(True|False|true|false|0|1)$",
    "FName":    r".+",          # 任意非空文本
    "FText":    r".+",
    "FString":  r".*",          # 允许空
    "FGameplayTag":         r"^[\w\.\-]+$",
    "FGameplayTagContainer": r"^([\w\.\-]+\s*,\s*)*[\w\.\-]+$",
    # FVector: 支持 (X=1,Y=2,Z=3) 或裸数字 1,2,3
    "FVector":              r"^(\(?X\s*=\s*-?\d+(\.\d+)?\s*,\s*Y\s*=\s*-?\d+(\.\d+)?\s*,\s*Z\s*=\s*-?\d+(\.\d+)?\)?)|(-?\d+(\.\d+)?\s*,\s*-?\d+(\.\d+)?\s*,\s*-?\d+(\.\d+)?)$",
    "FVector2D":            r"^(\(?X\s*=\s*-?\d+(\.\d+)?\s*,\s*Y\s*=\s*-?\d+(\.\d+)?\)?)|(-?\d+(\.\d+)?\s*,\s*-?\d+(\.\d+)?)$",
    "FRotator":             r"^(\(?P\s*=\s*-?\d+(\.\d+)?\s*,\s*Y\s*=\s*-?\d+(\.\d+)?\s*,\s*R\s*=\s*-?\d+(\.\d+)?\)?)|(-?\d+(\.\d+)?\s*,\s*-?\d+(\.\d+)?\s*,\s*-?\d+(\.\d+)?)$",
}


def format_csv_value(value: Any, ue_type: str) -> str:
    """
    根据 UE 类型将 Excel 单元格值格式化为 CSV 输出字符串。

    Args:
        value: 单元格原始值（None 或字符串/数字/布尔）
        ue_type: UE 类型标记（如 "int32"、"FText" 等）

    Returns:
        格式化后的 CSV 字符串
    """
    if value is None:
        value = ""

    s = str(value).strip()

    ue_type_lower = ue_type.strip().lower()

    # 数值类型
    if ue_type_lower in ("int32", "int64", "int8", "uint8", "uint16", "uint32", "uint64"):
        if s == "":
            return "0"
        try:
            return str(int(float(s)))
        except (ValueError, TypeError):
            return s

    if ue_type_lower == "float":
        if s == "":
            return "0.0"
        try:
            f = float(s)
            return f"{f:g}"
        except (ValueError, TypeError):
            return s

    # 布尔类型
    if ue_type_lower == "bool":
        s_lower = s.lower()
        if s_lower in ("true", "1"):
            return "True"
        elif s_lower in ("false", "0", ""):
            return "False"
        else:
            return s  # 保留原样，让校验环节报错

    # 文本类型：_csv_quote 在 export_csv 中统一处理引号包裹
    if ue_type_lower in ("ftext", "fstring"):
        return s

    # FName：直接输出
    if ue_type_lower == "fname":
        return s

    # FGameplayTag：格式化为 (TagName="xxx.yyy")
    if ue_type_lower == "fgameplaytag":
        if s:
            return f'(TagName="{s}")'
        return ""

    # FGameplayTagContainer：格式化为 (GameplayTags=((TagName="A"),(TagName="B")))
    if ue_type_lower == "fgameplaytagcontainer":
        if not s:
            return ""
        # 支持逗号或分号分隔
        tags = re.split(r"[;,，；]", s)
        tag_parts = []
        for t in tags:
            t = t.strip()
            if t:
                tag_parts.append(f'(TagName="{t}")')
        if tag_parts:
            return f'(GameplayTags=({",".join(tag_parts)}))'
        return ""

    # FVector / FVector2D / FRotator：格式化为 (X=1.0,Y=2.0,Z=3.0)
    if ue_type_lower in ("fvector", "fvector2d", "frotator"):
        if not s:
            return ""
        # 先尝试解析逗号分隔的裸数字 "100,200,300"
        parts = [p.strip() for p in s.split(",")]
        if len(parts) >= 2 and all(
            re.match(r"^-?\d+(\.\d+)?$", p) for p in parts
        ):
            if ue_type_lower == "fvector2d":
                return f"(X={parts[0]},Y={parts[1]})"
            elif ue_type_lower == "frotator":
                z = parts[2] if len(parts) > 2 else "0.0"
                return f"(P={parts[0]},Y={parts[1]},R={z})"
            else:  # fvector
                z = parts[2] if len(parts) > 2 else "0.0"
                return f"(X={parts[0]},Y={parts[1]},Z={z})"
        # 已经带格式的直接返回
        return s

    # 未知类型：原样输出
    return s


# ============================================================================
# 解析
# ============================================================================

def parse_xlsx(filepath: str) -> dict:
    """
    解析 Excel 文件，返回结构化数据。

    Args:
        filepath: .xlsx 文件路径

    Returns:
        {
            "source": str,           # 源文件路径
            "metadata": dict,         # 从第 1 行 # 注释解析的元数据
            "column_types": [str],    # 每列的类型标记
            "column_names": [str],    # 每列的字段名
            "rows": [[str]],          # 数据行列表
            "errors": [str],          # 解析过程中的警告/错误
        }

    Raises:
        FileNotFoundError: 文件不存在
        ValueError: 文件格式错误
    """
    result: dict = {
        "source": filepath,
        "metadata": {},
        "column_types": [],
        "column_names": [],
        "rows": [],
        "errors": [],
    }

    if not os.path.isfile(filepath):
        raise FileNotFoundError(f"文件不存在: {filepath}")

    wb = openpyxl.load_workbook(filepath, data_only=True, read_only=True)
    ws = wb.active

    if ws.max_row < 2:
        wb.close()
        result["errors"].append("工作表为空或行数不足")
        return result

    # 收集所有有效行（跳过空行和注释列，但保留位置信息）
    raw_rows: List[List[Optional[Any]]] = []
    for row in ws.iter_rows(min_row=1, max_row=ws.max_row, max_col=ws.max_column or 1):
        values = [cell.value for cell in row]
        # 跳过全空行
        if all(v is None for v in values):
            continue
        raw_rows.append(values)

    wb.close()

    if len(raw_rows) < 2:
        result["errors"].append("有效数据行不足（至少需要表头行 + 1 行数据）")
        return result

    # 确定表头起始位置
    # 如果第 1 行第 1 列以 # 开头 → 有元数据行，表头从第 2 行开始
    first_cell = raw_rows[0][0] if raw_rows[0] else None
    first_cell_str = str(first_cell).strip() if first_cell is not None else ""

    if first_cell_str.startswith("#"):
        # 有元数据行
        metadata_row = raw_rows[0]
        type_row = raw_rows[1] if len(raw_rows) > 1 else []
        name_row = raw_rows[2] if len(raw_rows) > 2 else []
        # 第4行：中文描述行（可选，自动检测并跳过）
        # 判断标准：该行第一列不像有效 RowName（非空的标识符），
        # 或第一列以 # 开头，则为描述行
        desc_row_idx = 3
        if desc_row_idx < len(raw_rows):
            desc_first = raw_rows[desc_row_idx][0] if raw_rows[desc_row_idx] else None
            desc_first_str = str(desc_first).strip() if desc_first is not None else ""
            # 描述行特征：第一列为空、以#开头、或包含中文字符（非有效FName）
            if (not desc_first_str
                or desc_first_str.startswith("#")
                or re.search(r"[\u4e00-\u9fff]", desc_first_str)):
                data_start = 4
            else:
                data_start = 3
        else:
            data_start = 3
        # 解析元数据
        for cell in metadata_row:
            if cell is None:
                continue
            s = str(cell).strip()
            if s.startswith("#"):
                # 解析 #KEY=VALUE 格式
                inner = s[1:].strip()
                if "=" in inner:
                    key, _, val = inner.partition("=")
                    key = key.strip()
                    val = val.strip()
                    if key:
                        result["metadata"][key] = val
    else:
        # 无元数据行
        type_row = raw_rows[0]
        name_row = raw_rows[1] if len(raw_rows) > 1 else []
        # 第3行：可能是描述行（可选）
        desc_row_idx = 2
        if desc_row_idx < len(raw_rows):
            desc_first = raw_rows[desc_row_idx][0] if raw_rows[desc_row_idx] else None
            desc_first_str = str(desc_first).strip() if desc_first is not None else ""
            if (not desc_first_str
                or desc_first_str.startswith("#")
                or re.search(r"[\u4e00-\u9fff]", desc_first_str)):
                data_start = 3
            else:
                data_start = 2
        else:
            data_start = 2

    # 确定有效列数（去掉以 # 开头的注释列，去掉全空的尾列）
    max_col = max(
        len(type_row) if type_row else 0,
        len(name_row) if name_row else 0,
    )

    valid_cols: List[int] = []  # 有效列的原始索引
    column_types: List[str] = []
    column_names: List[str] = []

    for i in range(max_col):
        # 检查是否注释列
        name_val = name_row[i] if i < len(name_row) else None
        type_val = type_row[i] if i < len(type_row) else None

        name_str = str(name_val).strip() if name_val is not None else ""
        type_str = str(type_val).strip() if type_val is not None else ""

        # 跳过以 # 开头的列（字段名或类型行任一以 # 开头即为注释列）
        # 策划可用此机制添加备注列，如字段名写 "#备注" 或 "#Comment"
        if name_str.startswith("#") or type_str.startswith("#"):
            continue
        # 跳过类型和名字都为空的列
        if not type_str and not name_str:
            continue

        valid_cols.append(i)
        column_types.append(type_str)
        column_names.append(name_str)

    if not column_names:
        result["errors"].append("未找到有效的字段名列")
        return result

    # 处理字段名：确保唯一性（openpyxl 合并单元格可能导致重复）
    seen: Dict[str, int] = {}
    final_names: List[str] = []
    for name in column_names:
        if name in seen:
            seen[name] += 1
            final_names.append(f"{name}_{seen[name]}")
        else:
            seen[name] = 0
            final_names.append(name)

    result["column_types"] = column_types
    result["column_names"] = final_names
    result["rows"] = []

    # 解析数据行
    for row_idx in range(data_start, len(raw_rows)):
        row_data = raw_rows[row_idx]
        # 检查是否注释行（第一列以 # 开头）
        first_val = row_data[0] if row_data else None
        first_str = str(first_val).strip() if first_val is not None else ""
        if first_str.startswith("#"):
            continue

        csv_row: List[str] = []
        for ci, oi in enumerate(valid_cols):
            cell_val = row_data[oi] if oi < len(row_data) else None
            cell_str = str(cell_val) if cell_val is not None else ""
            csv_row.append(cell_str)

        # 跳过全空数据行
        if all(c == "" for c in csv_row):
            continue

        result["rows"].append(csv_row)

    return result


# ============================================================================
# 校验
# ============================================================================

def validate_data(parsed_data: dict) -> List[str]:
    """
    校验解析后的数据。

    Args:
        parsed_data: parse_xlsx 的返回值

    Returns:
        错误列表，每个元素如 "Row 1, Col 'MaxHealth': 期望 int32，实际 'abc'"
    """
    errors: List[str] = []
    column_types = parsed_data.get("column_types", [])
    column_names = parsed_data.get("column_names", [])
    rows = parsed_data.get("rows", [])

    for row_idx, row in enumerate(rows):
        row_name = row[0] if row else "?"
        for col_idx in range(1, len(column_names)):  # 跳过 RowName
            if col_idx >= len(row):
                continue
            cell_val = row[col_idx]
            ue_type = column_types[col_idx] if col_idx < len(column_types) else ""
            col_name = column_names[col_idx]

            # 空值跳过（允许可选字段为空）
            if not cell_val or cell_val.strip() == "":
                continue

            # 获取校验正则
            pattern = TYPE_VALIDATORS.get(ue_type)
            if pattern is None:
                continue  # 未知类型不校验

            if not re.match(pattern, cell_val.strip()):
                errors.append(
                    f"RowName='{row_name}', Col='{col_name}': 期望类型 {ue_type}，实际值 '{cell_val}'"
                )

    return errors


# ============================================================================
# CSV 导出
# ============================================================================

def export_csv(parsed_data: dict, output_path: str, encoding: str = "utf-8-sig") -> str:
    """
    将解析后的数据导出为 UE DataTable 兼容的 CSV 文件。

    Args:
        parsed_data: parse_xlsx 的返回值
        output_path: 输出 CSV 文件路径
        encoding: 编码，默认 utf-8-sig（带 BOM）

    Returns:
        输出文件路径
    """
    column_names = parsed_data.get("column_names", [])
    column_types = parsed_data.get("column_types", [])
    rows = parsed_data.get("rows", [])

    # 确保输出目录存在
    out_dir = os.path.dirname(output_path)
    if out_dir and not os.path.exists(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    def _csv_quote(value: str, ue_type: str) -> str:
        """
        对单个 CSV 字段值进行引号处理。
        - FText/FString 始终用双引号包裹
        - 其他类型：仅当含逗号、引号、换行时才包裹
        - FGameplayTag / FVector 等已由 format_csv_value 处理好格式，
          但本函数负责最外层的 CSV 引号包裹
        """
        s = str(value)
        ue_type_lower = ue_type.lower().strip()

        if ue_type_lower in ("ftext", "fstring"):
            # 始终双引号包裹，内部引号按 CSV 标准加倍
            return '"' + s.replace('"', '""') + '"'

        # 其他类型：仅当含特殊字符时包裹
        if ',' in s or '"' in s or '\n' in s or '\r' in s:
            return '"' + s.replace('"', '""') + '"'
        return s

    with open(output_path, "w", encoding=encoding, newline="") as f:
        # 表头行：第一列用 --- 占位（UE 规范）
        header = ["---"] + list(column_names[1:])
        f.write(",".join(header) + "\n")

        # 数据行
        for row in rows:
            csv_parts = []
            for ci in range(len(column_names)):
                cell_val = row[ci] if ci < len(row) else ""
                ue_type = column_types[ci] if ci < len(column_types) else ""

                # 先做类型格式化
                formatted = format_csv_value(cell_val, ue_type)
                # 再做 CSV 引号处理
                quoted = _csv_quote(formatted, ue_type)
                csv_parts.append(quoted)

            f.write(",".join(csv_parts) + "\n")

    return output_path


# ============================================================================
# 批量导出
# ============================================================================

def export_all(input_dir: str, output_dir: str) -> dict:
    """
    批量导出目录下所有 .xlsx 文件。

    Args:
        input_dir: 输入目录
        output_dir: 输出目录

    Returns:
        {
            "total": int,
            "success": int,
            "failed": int,
            "results": [{"file": str, "success": bool, "message": str, "errors": [str]}]
        }
    """
    input_dir = os.path.abspath(input_dir)
    output_dir = os.path.abspath(output_dir)

    if not os.path.isdir(input_dir):
        return {
            "total": 0,
            "success": 0,
            "failed": 1,
            "results": [{"file": input_dir, "success": False, "message": f"输入目录不存在: {input_dir}", "errors": []}],
        }

    xlsx_files = sorted([
        f for f in os.listdir(input_dir)
        if f.endswith(".xlsx") and not f.startswith("~$")  # 跳过 Excel 临时文件
    ])

    total = len(xlsx_files)
    success = 0
    failed = 0
    results: List[dict] = []

    for xlsx_file in xlsx_files:
        filepath = os.path.join(input_dir, xlsx_file)
        csv_name = os.path.splitext(xlsx_file)[0] + ".csv"
        csv_path = os.path.join(output_dir, csv_name)

        try:
            data = parse_xlsx(filepath)
            validation_errors = validate_data(data)
            export_csv(data, csv_path)

            if validation_errors:
                success += 1
                results.append({
                    "file": xlsx_file,
                    "success": True,
                    "message": f"导出成功（{len(validation_errors)} 个类型警告）",
                    "errors": validation_errors,
                })
            else:
                success += 1
                results.append({
                    "file": xlsx_file,
                    "success": True,
                    "message": "导出成功",
                    "errors": [],
                })
        except Exception as e:
            failed += 1
            results.append({
                "file": xlsx_file,
                "success": False,
                "message": f"导出失败: {e}",
                "errors": [str(e)],
            })

    return {
        "total": total,
        "success": success,
        "failed": failed,
        "results": results,
    }


# ============================================================================
# 日志辅助
# ============================================================================

class LogCollector:
    """简易日志收集器，用于 GUI 显示"""

    def __init__(self):
        self.logs: List[Tuple[str, str]] = []  # [(level, message)]

    def info(self, msg: str):
        self.logs.append(("INFO", msg))

    def warn(self, msg: str):
        self.logs.append(("WARN", msg))

    def error(self, msg: str):
        self.logs.append(("ERROR", msg))

    def clear(self):
        self.logs.clear()

    def get_all(self) -> List[Tuple[str, str]]:
        return self.logs


# ============================================================================
# CLI 入口
# ============================================================================

def _print_banner():
    print("=" * 52)
    print("  LetsDeepRock Excel → CSV 导出工具")
    print("=" * 52)


def _main():
    import argparse

    parser = argparse.ArgumentParser(
        description="LetsDeepRock Excel → CSV 导出工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python excel_exporter.py DT_EnemyConfig.xlsx
  python excel_exporter.py Config/Excel --output Config/Excel
  python excel_exporter.py Config/Excel -o ./Output
        """,
    )
    parser.add_argument(
        "target",
        help="输入：单个 .xlsx 文件 或 目录路径",
    )
    parser.add_argument(
        "--output", "-o",
        default=None,
        help="输出目录（单文件模式默认输出到同目录，目录模式默认输出到输入目录）",
    )
    parser.add_argument(
        "--no-validate",
        action="store_true",
        help="跳过类型校验",
    )

    args = parser.parse_args()
    target = os.path.abspath(args.target)

    if not os.path.exists(target):
        _print_banner()
        print(f"\n[错误] 路径不存在: {target}")
        sys.exit(1)

    _print_banner()

    if os.path.isfile(target):
        # 单文件模式
        if not target.endswith(".xlsx"):
            print(f"\n[错误] 不是 .xlsx 文件: {target}")
            sys.exit(1)

        output_dir = args.output or os.path.dirname(target)
        csv_name = os.path.splitext(os.path.basename(target))[0] + ".csv"
        output_path = os.path.join(os.path.abspath(output_dir), csv_name)

        print(f"\n输入文件: {target}")
        print(f"输出文件: {output_path}")

        try:
            data = parse_xlsx(target)
        except Exception as e:
            print(f"\n[错误] 解析失败: {e}")
            sys.exit(1)

        if data["errors"]:
            print("\n[解析警告]:")
            for err in data["errors"]:
                print(f"  - {err}")

        if not args.no_validate:
            validation_errors = validate_data(data)
            if validation_errors:
                print(f"\n[类型校验] {len(validation_errors)} 个问题:")
                for err in validation_errors:
                    print(f"  !! {err}")
            else:
                print("\n[类型校验] 全部通过")

        try:
            export_csv(data, output_path)
            print(f"\n[完成] 已导出 → {output_path}")
        except Exception as e:
            print(f"\n[错误] 导出失败: {e}")
            sys.exit(1)

    else:
        # 目录模式
        output_dir = args.output or target
        print(f"\n输入目录: {target}")
        print(f"输出目录: {os.path.abspath(output_dir)}")
        print(f"\n正在扫描 .xlsx 文件...")

        result = export_all(target, output_dir)

        print(f"\n扫描到 {result['total']} 个文件")
        print(f"成功: {result['success']}  失败: {result['failed']}")
        print("-" * 52)

        for r in result["results"]:
            status = "[OK]" if r["success"] else "[FAIL]"
            print(f"  {status} {r['file']}: {r['message']}")
            if r["errors"]:
                for err in r["errors"]:
                    print(f"         {err}")

        print("-" * 52)
        print(f"\n导出完成！")

    sys.exit(0)


if __name__ == "__main__":
    _main()
