# -*- coding: utf-8 -*-
"""
LetsDeepRock Excel 导出工具 - tkinter GUI

策划友好的可视化导出界面，支持目录选择、文件勾选、分批导出。
双击 .pyw 文件直接运行（无控制台窗口）。

依赖: excel_exporter.py（同目录）
"""

import os
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

# 确保能导入同目录的核心模块
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from excel_exporter import (
    LogCollector,
    export_all,
    export_csv,
    parse_xlsx,
    validate_data,
)


# ============================================================================
# 常量
# ============================================================================

WINDOW_TITLE = "LetsDeepRock Excel 导出工具"
WINDOW_WIDTH = 740
WINDOW_HEIGHT = 560

# 默认输入目录（相对于 Tools/ExcelExporter/ → ../../Config/Excel/）
DEFAULT_INPUT_DIR = os.path.normpath(
    os.path.join(SCRIPT_DIR, "..", "..", "Config", "Excel")
)


# ============================================================================
# GUI 应用
# ============================================================================

class ExcelExporterApp:
    """主应用窗口"""

    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title(WINDOW_TITLE)
        self.root.geometry(f"{WINDOW_WIDTH}x{WINDOW_HEIGHT}")
        self.root.minsize(600, 400)

        # 图标（Windows 下可用默认图标）
        try:
            self.root.iconbitmap(default="")
        except Exception:
            pass

        # 状态
        self._exporting = False
        self._file_checkboxes: dict = {}  # {filename: tk.BooleanVar}

        self._build_ui()
        self._refresh_file_list()

    # ------------------------------------------------------------------
    # UI 构建
    # ------------------------------------------------------------------

    def _build_ui(self):
        """构建所有 UI 组件"""
        # 主容器（内边距）
        main_frame = ttk.Frame(self.root, padding="10")
        main_frame.pack(fill=tk.BOTH, expand=True)

        # ---- 第 1 行：目录选择 ----
        dir_frame = ttk.LabelFrame(main_frame, text="目录设置", padding="8")
        dir_frame.pack(fill=tk.X, pady=(0, 8))

        # 输入目录
        ttk.Label(dir_frame, text="输入目录:").grid(row=0, column=0, sticky=tk.W, padx=(0, 5))
        self._input_var = tk.StringVar(value=DEFAULT_INPUT_DIR)
        ttk.Entry(dir_frame, textvariable=self._input_var).grid(
            row=0, column=1, sticky=tk.EW, padx=(0, 5)
        )
        ttk.Button(dir_frame, text="浏览...", width=8,
                   command=self._browse_input).grid(row=0, column=2)

        # 输出目录
        ttk.Label(dir_frame, text="输出目录:").grid(row=1, column=0, sticky=tk.W, padx=(0, 5), pady=(6, 0))
        self._output_var = tk.StringVar(value=DEFAULT_INPUT_DIR)
        ttk.Entry(dir_frame, textvariable=self._output_var).grid(
            row=1, column=1, sticky=tk.EW, padx=(0, 5), pady=(6, 0)
        )
        ttk.Button(dir_frame, text="浏览...", width=8,
                   command=self._browse_output).grid(row=1, column=2, pady=(6, 0))

        dir_frame.columnconfigure(1, weight=1)

        # ---- 第 2 行：文件列表 ----
        list_frame = ttk.LabelFrame(main_frame, text="文件列表", padding="8")
        list_frame.pack(fill=tk.BOTH, expand=True, pady=(0, 8))

        # 工具栏：全选 / 取消 / 刷新
        toolbar = ttk.Frame(list_frame)
        toolbar.pack(fill=tk.X, pady=(0, 4))

        self._file_count_label = ttk.Label(toolbar, text="共 0 个文件")
        self._file_count_label.pack(side=tk.LEFT)

        ttk.Button(toolbar, text="全选", width=6,
                   command=lambda: self._set_all_checked(True)).pack(side=tk.RIGHT, padx=(2, 0))
        ttk.Button(toolbar, text="取消全选", width=8,
                   command=lambda: self._set_all_checked(False)).pack(side=tk.RIGHT, padx=(2, 0))
        ttk.Button(toolbar, text="刷新", width=6,
                   command=self._refresh_file_list).pack(side=tk.RIGHT)

        # 可滚动的勾选列表
        list_container = ttk.Frame(list_frame)
        list_container.pack(fill=tk.BOTH, expand=True)

        self._list_canvas = tk.Canvas(list_container, highlightthickness=0)
        scrollbar = ttk.Scrollbar(list_container, orient=tk.VERTICAL,
                                  command=self._list_canvas.yview)
        self._list_inner = ttk.Frame(self._list_canvas)

        self._list_inner.bind("<Configure>",
                              lambda e: self._list_canvas.configure(
                                  scrollregion=self._list_canvas.bbox("all")
                              ))

        self._list_canvas.create_window((0, 0), window=self._list_inner, anchor=tk.NW)
        self._list_canvas.configure(yscrollcommand=scrollbar.set)

        self._list_canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        # 鼠标滚轮支持
        self._list_canvas.bind("<Enter>", self._bind_mousewheel)
        self._list_canvas.bind("<Leave>", self._unbind_mousewheel)

        # ---- 第 3 行：操作按钮 ----
        btn_frame = ttk.Frame(main_frame)
        btn_frame.pack(fill=tk.X, pady=(0, 8))

        self._btn_export_all = ttk.Button(
            btn_frame, text="■ 全部导出", width=16,
            command=self._on_export_all
        )
        self._btn_export_all.pack(side=tk.LEFT, padx=(0, 5))

        self._btn_export_selected = ttk.Button(
            btn_frame, text="▶ 导出选中", width=16,
            command=self._on_export_selected
        )
        self._btn_export_selected.pack(side=tk.LEFT)

        # ---- 第 4 行：日志输出 ----
        log_frame = ttk.LabelFrame(main_frame, text="导出日志", padding="4")
        log_frame.pack(fill=tk.BOTH, expand=True)

        self._log_text = tk.Text(
            log_frame, height=8, wrap=tk.WORD,
            font=("Consolas", 9),
            state=tk.DISABLED,
        )
        log_scrollbar = ttk.Scrollbar(log_frame, orient=tk.VERTICAL,
                                      command=self._log_text.yview)
        self._log_text.configure(yscrollcommand=log_scrollbar.set)

        self._log_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        log_scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        # 日志颜色标签
        self._log_text.tag_configure("INFO", foreground="#333333")
        self._log_text.tag_configure("WARN", foreground="#D2691E")
        self._log_text.tag_configure("ERROR", foreground="#CC0000")
        self._log_text.tag_configure("SUCCESS", foreground="#008000")

    # ------------------------------------------------------------------
    # 鼠标滚轮
    # ------------------------------------------------------------------

    def _bind_mousewheel(self, _event):
        self._list_canvas.bind_all("<MouseWheel>", self._on_mousewheel)

    def _unbind_mousewheel(self, _event):
        self._list_canvas.unbind_all("<MouseWheel>")

    def _on_mousewheel(self, event):
        self._list_canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")

    # ------------------------------------------------------------------
    # 目录浏览
    # ------------------------------------------------------------------

    def _browse_input(self):
        path = filedialog.askdirectory(title="选择输入目录（Excel 文件所在目录）")
        if path:
            self._input_var.set(path)
            self._output_var.set(path)
            self._refresh_file_list()

    def _browse_output(self):
        path = filedialog.askdirectory(title="选择输出目录（CSV 导出目标目录）")
        if path:
            self._output_var.set(path)

    # ------------------------------------------------------------------
    # 文件列表管理
    # ------------------------------------------------------------------

    def _refresh_file_list(self):
        """刷新文件勾选列表"""
        # 清空
        for widget in self._list_inner.winfo_children():
            widget.destroy()
        self._file_checkboxes.clear()

        input_dir = self._input_var.get()
        if not os.path.isdir(input_dir):
            self._file_count_label.config(text="目录不存在")
            return

        # 列出 .xlsx 文件（排除 Excel 临时文件 ~$）
        xlsx_files = sorted([
            f for f in os.listdir(input_dir)
            if f.endswith(".xlsx") and not f.startswith("~$")
        ])

        self._file_count_label.config(text=f"共 {len(xlsx_files)} 个文件")

        if not xlsx_files:
            ttk.Label(self._list_inner, text="（无 .xlsx 文件）",
                      foreground="gray").pack(anchor=tk.W, pady=4)
            return

        for fname in xlsx_files:
            var = tk.BooleanVar(value=True)
            cb = ttk.Checkbutton(
                self._list_inner, text=fname, variable=var,
            )
            cb.pack(anchor=tk.W)
            self._file_checkboxes[fname] = var

    def _set_all_checked(self, checked: bool):
        """全选或取消全选"""
        for var in self._file_checkboxes.values():
            var.set(checked)

    def _get_checked_files(self) -> list:
        """获取勾选的文件名列表"""
        return [fname for fname, var in self._file_checkboxes.items() if var.get()]

    def _get_selected_files(self) -> list:
        """获取要导出的文件列表（"导出选中"用勾选列表，"全部导出"用全部文件）"""
        return self._get_checked_files()

    # ------------------------------------------------------------------
    # 日志输出
    # ------------------------------------------------------------------

    def _log(self, msg: str, level: str = "INFO"):
        """向日志框追加一行"""
        self._log_text.configure(state=tk.NORMAL)
        self._log_text.insert(tk.END, msg + "\n", level)
        self._log_text.see(tk.END)
        self._log_text.configure(state=tk.DISABLED)
        self.root.update_idletasks()

    def _clear_log(self):
        """清空日志"""
        self._log_text.configure(state=tk.NORMAL)
        self._log_text.delete("1.0", tk.END)
        self._log_text.configure(state=tk.DISABLED)

    # ------------------------------------------------------------------
    # 导出操作
    # ------------------------------------------------------------------

    def _set_buttons_state(self, enabled: bool):
        """设置按钮启用/禁用状态"""
        state = tk.NORMAL if enabled else tk.DISABLED
        self._btn_export_all.configure(state=state)
        self._btn_export_selected.configure(state=state)

    def _on_export_all(self):
        """全部导出按钮回调"""
        if self._exporting:
            messagebox.showwarning("提示", "正在导出中，请等待完成")
            return

        input_dir = self._input_var.get()
        if not os.path.isdir(input_dir):
            messagebox.showerror("错误", f"输入目录不存在:\n{input_dir}")
            return

        self._clear_log()
        self._log("=" * 52, "INFO")
        self._log("  LetsDeepRock Excel → CSV 导出工具", "INFO")
        self._log("=" * 52, "INFO")
        self._log(f"输入目录: {input_dir}", "INFO")
        self._log(f"输出目录: {self._output_var.get()}", "INFO")
        self._log("", "INFO")

        self._exporting = True
        self._set_buttons_state(False)

        thread = threading.Thread(target=self._do_export_all, daemon=True)
        thread.start()

    def _on_export_selected(self):
        """导出选中按钮回调"""
        if self._exporting:
            messagebox.showwarning("提示", "正在导出中，请等待完成")
            return

        selected = self._get_checked_files()
        if not selected:
            messagebox.showinfo("提示", "没有勾选任何文件")
            return

        input_dir = self._input_var.get()
        if not os.path.isdir(input_dir):
            messagebox.showerror("错误", f"输入目录不存在:\n{input_dir}")
            return

        self._clear_log()
        self._log("=" * 52, "INFO")
        self._log("  LetsDeepRock Excel → CSV 导出工具（仅导出选中）", "INFO")
        self._log("=" * 52, "INFO")
        self._log(f"输入目录: {input_dir}", "INFO")
        self._log(f"输出目录: {self._output_var.get()}", "INFO")
        self._log(f"选中文件: {len(selected)} 个", "INFO")
        self._log("", "INFO")

        self._exporting = True
        self._set_buttons_state(False)

        thread = threading.Thread(target=self._do_export_selected, args=(selected,), daemon=True)
        thread.start()

    # ---- 后台导出线程 ----

    def _do_export_all(self):
        """后台：批量导出全部"""
        input_dir = self._input_var.get()
        output_dir = self._output_var.get()

        result = export_all(input_dir, output_dir)

        self.root.after(0, self._on_export_done, result, input_dir)

    def _do_export_selected(self, selected_files: list):
        """后台：只导出勾选的文件"""
        input_dir = self._input_var.get()
        output_dir = self._output_var.get()

        total = len(selected_files)
        success = 0
        failed = 0
        results = []

        for fname in selected_files:
            filepath = os.path.join(input_dir, fname)
            csv_name = os.path.splitext(fname)[0] + ".csv"
            csv_path = os.path.join(output_dir, csv_name)

            try:
                data = parse_xlsx(filepath)
                validation_errors = validate_data(data)
                export_csv(data, csv_path)

                if validation_errors:
                    success += 1
                    results.append({
                        "file": fname,
                        "success": True,
                        "message": f"导出成功（{len(validation_errors)} 个类型警告）",
                        "errors": validation_errors,
                    })
                else:
                    success += 1
                    results.append({
                        "file": fname,
                        "success": True,
                        "message": "导出成功",
                        "errors": [],
                    })
            except Exception as e:
                failed += 1
                results.append({
                    "file": fname,
                    "success": False,
                    "message": f"导出失败: {e}",
                    "errors": [str(e)],
                })

        result = {
            "total": total,
            "success": success,
            "failed": failed,
            "results": results,
        }

        self.root.after(0, self._on_export_done, result, input_dir)

    def _on_export_done(self, result: dict, input_dir: str):
        """导出完成回调（UI 线程）"""
        self._exporting = False
        self._set_buttons_state(True)

        # 输出日志
        for r in result["results"]:
            status_icon = "[OK]" if r["success"] else "[FAIL]"
            level = "SUCCESS" if r["success"] else "ERROR"
            self._log(f"{status_icon} {r['file']}: {r['message']}", level)
            for err in r.get("errors", []):
                self._log(f"      {err}", "WARN")

        self._log("", "INFO")
        self._log("-" * 52, "INFO")
        self._log(f"总计: {result['total']}  成功: {result['success']}  失败: {result['failed']}", "INFO")
        self._log("-" * 52, "INFO")

        # 刷新文件列表（可能文件有变化）
        # 保持之前输入目录
        if self._input_var.get() == input_dir:
            self._refresh_file_list()

        # 弹窗提示
        if result["failed"] == 0 and result["success"] > 0:
            messagebox.showinfo(
                "导出完成",
                f"全部 {result['success']} 个文件导出成功！"
            )
        elif result["failed"] > 0 and result["success"] > 0:
            messagebox.showwarning(
                "导出完成",
                f"成功 {result['success']} 个，失败 {result['failed']} 个。\n详见日志。"
            )
        elif result["failed"] > 0 and result["success"] == 0:
            messagebox.showerror(
                "导出失败",
                f"全部 {result['failed']} 个文件导出失败。\n详见日志。"
            )
        elif result["total"] == 0:
            messagebox.showinfo("提示", "没有找到可导出的 .xlsx 文件")


# ============================================================================
# 入口
# ============================================================================

def main():
    root = tk.Tk()
    app = ExcelExporterApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
