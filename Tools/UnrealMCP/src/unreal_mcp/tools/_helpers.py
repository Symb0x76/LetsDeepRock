"""工具模块公共辅助函数"""


def unwrap_result(result) -> str:
    """从 execute_python 的返回值中提取有意义的输出。

    UE Python Remote Exec 返回格式:
    {
        "success": bool,
        "result": str,  # print() 返回 "None"，表达式求值返回值
        "output": [{"type": "Info/Warning/Error", "output": "..."}]
    }

    优先提取 output 中的文本（这是 print 的实际输出），
    其次取 result 字段（表达式求值结果）。
    """
    if not isinstance(result, dict):
        return str(result) if result is not None else "执行成功（无输出）"

    if not result.get("success"):
        # 执行失败：提取错误信息
        error_lines = []
        for entry in result.get("output", []):
            if isinstance(entry, dict):
                error_lines.append(entry.get("output", "").rstrip())
            else:
                error_lines.append(str(entry))
        error_text = "\n".join(error_lines) if error_lines else result.get("result", "未知错误")
        return f"执行失败: {error_text}"

    # 执行成功：先看 output 列表（print 的内容）
    output_lines = []
    for entry in result.get("output", []):
        if isinstance(entry, dict):
            text = entry.get("output", "").rstrip()
            if text:
                output_lines.append(text)
        elif isinstance(entry, str) and entry.strip():
            output_lines.append(entry.strip())

    if output_lines:
        return "\n".join(output_lines)

    # output 为空则取 result 字段
    res_val = result.get("result", "")
    if res_val and res_val != "None":
        return res_val

    return "执行成功（无输出）"
