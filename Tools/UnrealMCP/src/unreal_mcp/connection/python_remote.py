"""UE Python Remote Exec 客户端 — 基于 UE 官方 remote_execution.py 的异步包装

通过 UDP 多播发现 UE 编辑器实例，建立 TCP 命令连接后执行 Python 代码。
底层协议实现直接复用 UE 5.8 官方提供的 remote_execution.py 模块。
"""

import asyncio
import logging
from typing import Optional

from .ue_remote_execution import (
    RemoteExecution,
    RemoteExecutionConfig,
    MODE_EXEC_FILE,
    MODE_EXEC_STATEMENT,
    MODE_EVAL_STATEMENT,
)

logger = logging.getLogger("unreal_mcp.python_remote")


class PythonRemoteExec:
    """UE Python 远程执行客户端（异步接口）

    包装 UE 官方 RemoteExecution 类，提供异步方法供 MCP Server 使用。

    使用方式:
        remote = PythonRemoteExec()
        await remote.connect()
        result = await remote.execute("import unreal; print(unreal.get_editor_subsystem(unreal.EditorAssetSubsystem))")
        await remote.disconnect()
    """

    def __init__(
        self,
        multicast_group: str = "239.0.0.1",
        multicast_port: int = 6766,
        multicast_bind_address: str = "0.0.0.0",
        command_endpoint: tuple = ("127.0.0.1", 6776),
        discovery_timeout: float = 5.0,
        execute_timeout: float = 30.0,
    ):
        """初始化远程执行客户端

        Args:
            multicast_group: UDP 多播组地址
            multicast_port: UDP 多播端口
            multicast_bind_address: 多播绑定地址（0.0.0.0 = 所有网卡）
            command_endpoint: TCP 命令连接端点（本地监听地址）
            discovery_timeout: 节点发现超时（秒）
            execute_timeout: 命令执行超时（秒）
        """
        self._config = RemoteExecutionConfig()
        self._config.multicast_group_endpoint = (multicast_group, multicast_port)
        self._config.multicast_bind_address = multicast_bind_address
        self._config.command_endpoint = command_endpoint

        self._discovery_timeout = discovery_timeout
        self._execute_timeout = execute_timeout

        self._remote_exec: Optional[RemoteExecution] = None
        self._remote_node_id: Optional[str] = None
        self._connected = False

    def is_connected(self) -> bool:
        """检查是否已建立命令连接"""
        return self._connected and self._remote_exec is not None and self._remote_exec.has_command_connection()

    async def connect(self) -> bool:
        """发现并连接 UE 编辑器

        流程：
        1. 启动 UDP 广播发现编辑器节点
        2. 等待直到发现至少一个节点（或超时）
        3. 与第一个发现的节点建立 TCP 命令连接

        Returns:
            True 连接成功，False 超时或失败
        """
        try:
            # 在线程池中运行��塞操作（UE 官方实现是同步的）
            loop = asyncio.get_event_loop()
            result = await loop.run_in_executor(None, self._connect_sync)
            return result
        except Exception as e:
            logger.error("连接失败: %s", e)
            self._connected = False
            return False

    def _connect_sync(self) -> bool:
        """同步连接实现（在线程池中执行）"""
        import time

        # 清理旧连接
        if self._remote_exec:
            try:
                self._remote_exec.stop()
            except Exception:
                pass

        # 创建新的远程执行会话
        self._remote_exec = RemoteExecution(self._config)
        self._remote_exec.start()

        # 等待发现节点
        start_time = time.time()
        while time.time() - start_time < self._discovery_timeout:
            nodes = self._remote_exec.remote_nodes
            if nodes:
                # 发现了 UE 编辑器节点
                first_node = nodes[0]
                self._remote_node_id = first_node["node_id"]
                logger.info(
                    "发现 UE 节点: %s (项目: %s)",
                    self._remote_node_id,
                    first_node.get("data", {}).get("project_name", "unknown"),
                )
                break
            time.sleep(0.2)
        else:
            logger.warning("节点发现超时 (%.1fs)，未找到 UE 编辑器", self._discovery_timeout)
            self._remote_exec.stop()
            self._remote_exec = None
            self._connected = False
            return False

        # 建立 TCP 命令连接
        try:
            self._remote_exec.open_command_connection(self._remote_node_id)
            self._connected = True
            logger.info("TCP 命令连接已建立")
            return True
        except Exception as e:
            logger.error("建立 TCP 命令连接失败: %s", e)
            self._remote_exec.stop()
            self._remote_exec = None
            self._connected = False
            return False

    async def disconnect(self):
        """断开连接"""
        if self._remote_exec:
            loop = asyncio.get_event_loop()
            await loop.run_in_executor(None, self._disconnect_sync)

    def _disconnect_sync(self):
        """同步断开实现"""
        if self._remote_exec:
            try:
                self._remote_exec.stop()
            except Exception as e:
                logger.warning("断开连接时出错: %s", e)
            finally:
                self._remote_exec = None
                self._connected = False
                self._remote_node_id = None
                logger.info("已断开 UE 连接")

    async def execute(self, code: str, mode: str = "ExecuteFile") -> dict:
        """在 UE 编辑器中执行 Python 代码

        Args:
            code: 要执行的 Python 代码
            mode: 执行模式
                - "ExecuteFile": 执行多行脚本（默认，最通用）
                - "ExecuteStatement": 执行单条语句并打印结果
                - "EvaluateStatement": 求值表达式并返回结果

        Returns:
            dict: 包含执行结果
                - success (bool): 是否成功
                - result (str): 执行输出/结果
                - command (str): 原始命令
                - output (list): 日志行
        """
        if not self.is_connected():
            # 尝试自动重连
            connected = await self.connect()
            if not connected:
                return {
                    "success": False,
                    "result": "未连接到 UE 编辑器，自动重连失败",
                    "command": code,
                    "output": [],
                }

        try:
            loop = asyncio.get_event_loop()
            result = await asyncio.wait_for(
                loop.run_in_executor(None, self._execute_sync, code, mode),
                timeout=self._execute_timeout,
            )
            return result
        except asyncio.TimeoutError:
            return {
                "success": False,
                "result": f"执行超时 ({self._execute_timeout}s)",
                "command": code,
                "output": [],
            }
        except Exception as e:
            # 连接可能已断开
            self._connected = False
            return {
                "success": False,
                "result": f"执行错误: {str(e)}",
                "command": code,
                "output": [],
            }

    def _execute_sync(self, code: str, mode: str) -> dict:
        """同步执行命令"""
        result = self._remote_exec.run_command(code, unattended=True, exec_mode=mode)
        if result is None:
            return {
                "success": False,
                "result": "未收到执行结果（连接可能已断开）",
                "command": code,
                "output": [],
            }
        return {
            "success": result.get("success", False),
            "result": result.get("result", ""),
            "command": result.get("command", code),
            "output": result.get("output", []),
        }

    async def evaluate(self, expression: str) -> str:
        """执行表达式并返回结果字符串

        Args:
            expression: Python 表达式

        Returns:
            表达式求值结果的字符串表示
        """
        result = await self.execute(expression, mode="EvaluateStatement")
        if result.get("success"):
            return result.get("result", "")
        return f"[错误] {result.get('result', '未知错误')}"

    async def __aenter__(self):
        await self.connect()
        return self

    async def __aexit__(self, *args):
        await self.disconnect()

    def __repr__(self):
        status = "已连接" if self.is_connected() else "未连接"
        return f"<PythonRemoteExec {status}>"
