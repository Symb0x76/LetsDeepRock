"""Unreal MCP Server 主入口 — 通过 stdio 传输的 MCP 协议服务器，连接 UE 编辑器
架构：
    OpenCode → stdio → MCP Server (本文件) → PythonRemoteExec / RemoteControlAPI → UE Editor
"""

import asyncio
import logging
import sys

from mcp.server.fastmcp import FastMCP

from .connection import PythonRemoteExec, RemoteControlAPI
from .tools import (
    asset_tools,
    blueprint_graph_tools,
    blueprint_tools,
    gameplay_tools,
    input_tools,
    level_tools,
    utility_tools,
)

# 日志输出到 stderr（stdout 被 MCP 协议占用）
logger = logging.getLogger("unreal_mcp")

# 所有需要注册的工具模块
TOOL_MODULES = [
    asset_tools,
    blueprint_graph_tools,
    blueprint_tools,
    gameplay_tools,
    input_tools,
    level_tools,
    utility_tools,
]


class UnrealConnection:
    """UE 编辑器连接管理器

    封装双通道通信：
    - PythonRemoteExec：通过 UDP 多播远程执行 Python 代码（功能更全，优先使用）
    - RemoteControlAPI：通过 HTTP 调用 UE Remote Control 插件（作为补充）
    """

    def __init__(
        self,
        ue_host: str = "127.0.0.1",
        rc_port: int = 30010,
        multicast_group: str = "239.0.0.1",
        multicast_port: int = 6766,
    ):
        self.ue_host = ue_host
        self.rc_port = rc_port
        self.multicast_group = multicast_group
        self.multicast_port = multicast_port

        # 双通道连接实例
        self.python_exec = PythonRemoteExec(
            multicast_group=multicast_group,
            multicast_port=multicast_port,
        )
        self.rc_api = RemoteControlAPI(
            base_url=f"http://{ue_host}:{rc_port}",
        )

    # ---- 通道状态查询 ----

    @property
    def python_available(self) -> bool:
        """Python Remote Exec 通道是否可用"""
        return self.python_exec.is_connected()

    @property
    def rc_available(self) -> bool:
        """Remote Control API 通道是否可用"""
        return self.rc_api.is_connected()

    # ---- 连接管理 ----

    async def ensure_connected(self) -> bool:
        """确保至少一个通信通道可用。

        并行尝试连接两个通道，只要有一个成功即返回 True。
        优先使用 Python Remote Exec，RC API 作为后备。
        """
        results = await asyncio.gather(
            self.python_exec.connect(),
            self.rc_api.connect(),
            return_exceptions=True,
        )

        python_ok = isinstance(results[0], bool) and results[0]
        rc_ok = not isinstance(results[1], Exception)  # connect() 返回 dict 表示成功

        if python_ok:
            logger.info("Python Remote Exec 连接成功")
        else:
            logger.warning("Python Remote Exec 连接失败: %s", results[0])

        if rc_ok:
            logger.info("Remote Control API 连接成功 (HTTP %s)", self.rc_api.base_url)
        else:
            logger.warning("Remote Control API 连接失败: %s", results[1])

        if not python_ok and not rc_ok:
            logger.error("所有通信通道均不可用！请确认 UE 编辑器已启动并启用了相应插件。")
            return False

        return True

    async def disconnect(self):
        """断开所有连接"""
        await asyncio.gather(
            self.python_exec.disconnect(),
            self.rc_api.close(),
            return_exceptions=True,
        )
        logger.info("已断开所有 UE 连接")

    # ---- 核心通信接口 ----

    async def execute_python(self, code: str):
        """通过 Python Remote Exec 在 UE 编辑器中执行 Python 代码。

        这是主要的命令通道，功能最全面，可以调用任何 UE Python API。

        Args:
            code: 要执行的 Python 代码字符串

        Returns:
            UE 编辑器返回的执行结果（dict 或 str）
        """
        if not self.python_exec.is_connected():
            connected = await self.python_exec.connect()
            if not connected:
                return {
                    "success": False,
                    "result": "未连接到 UE 编辑器，Python Remote Exec 连接失败",
                    "command": code,
                    "output": [],
                }
        return await self.python_exec.execute(code)

    async def rc_call(
        self,
        object_path: str,
        function_name: str,
        parameters: dict | None = None,
    ) -> dict:
        """通过 Remote Control API 调用 UE 对象上的函数。"""
        if not self.rc_api.is_connected():
            await self.rc_api.connect()
        return await self.rc_api.call_function(object_path, function_name, parameters)

    async def rc_get_property(self, object_path: str, property_name: str) -> dict:
        """通过 Remote Control API 读取 UE 对象属性"""
        if not self.rc_api.is_connected():
            await self.rc_api.connect()
        return await self.rc_api.get_property(object_path, property_name)

    async def rc_set_property(self, object_path: str, property_name: str, value) -> dict:
        """通过 Remote Control API 设置 UE 对象属性"""
        if not self.rc_api.is_connected():
            await self.rc_api.connect()
        return await self.rc_api.set_property(object_path, property_name, value)


# ---- 全局 MCP Server 实例和 Connection 实例 ----
# FastMCP 需要在模块级别声明（装饰器注册用）

mcp = FastMCP("unreal-mcp")
connection = UnrealConnection()


def register_tools():
    """向 MCP Server 注册所有工具模块"""
    for module in TOOL_MODULES:
        try:
            module.register(mcp, connection)
            logger.debug("已注册工具模块: %s", module.__name__)
        except Exception as e:
            logger.error("注册工具模块 %s 失败: %s", module.__name__, e)

    # 注册 ping 工具
    _register_ping_tool()


def _register_ping_tool():
    """注册 ping 工具 — 测试与 UE 编辑器的连接状态"""

    @mcp.tool()
    async def ping() -> str:
        """测试与 UE 编辑器的连接状态。

        返回当前双通道（Python Remote Exec + Remote Control API）的连接状况，
        用于验证 MCP Server 是否能正常与 UE 编辑器通信。

        如果两个通道均未连接，会主动尝试一次连接后再报告状态；
        如果已有任一通道可用，不会重复连接。
        """
        # 已有任一通道可用，不重复连接
        if connection.python_available or connection.rc_available:
            python_status = "已连接" if connection.python_available else "未连接"
            rc_status = "已连接" if connection.rc_available else "未连接"
            return (
                "Unreal MCP Server 连接正常\n"
                f"  - Python Remote Exec: {python_status}\n"
                f"  - Remote Control API: {rc_status}"
            )

        # 两个通道都不可用 — 主动尝试连接
        await connection.ensure_connected()

        python_status = "已连接" if connection.python_available else "未连接"
        rc_status = "已连接" if connection.rc_available else "未连接"

        if connection.python_available or connection.rc_available:
            return (
                "Unreal MCP Server 连接成功\n"
                f"  - Python Remote Exec: {python_status}\n"
                f"  - Remote Control API: {rc_status}"
            )
        else:
            return (
                "Unreal MCP Server 未连接到 UE 编辑器\n"
                f"  - Python Remote Exec: {python_status}\n"
                f"  - Remote Control API: {rc_status}\n"
                "请确保 UE 编辑器已启动并启用了 Python Remote Exec 和 Remote Control 插件。"
            )


# 模块加载时注册所有工具
register_tools()


def main():
    """MCP Server 主入口"""
    # 日志配置（输出到 stderr，避免干扰 MCP 协议通信）
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
        stream=sys.stderr,
    )

    logger.info("Unreal MCP Server v0.1.0 启动中...")

    # 通过 FastMCP 的 stdio 运行模式启动
    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
