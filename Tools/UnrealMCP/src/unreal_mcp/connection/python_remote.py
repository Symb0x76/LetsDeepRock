"""UE Python Remote Exec 客户端 — 基于 UE 官方 remote_execution.py 的异步包装

通过 UDP 多播发现 UE 编辑器实例，建立 TCP 命令连接后执行 Python 代码。
底层协议实现直接复用 UE 5.8 官方提供的 remote_execution.py 模块。
"""

import asyncio
import logging
from concurrent.futures import ThreadPoolExecutor
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
        multicast_bind_address: str = "127.0.0.1",
        command_endpoint: tuple = ("127.0.0.1", 6776),
        discovery_timeout: float = 10.0,
        execute_timeout: float = 30.0,
    ):
        """初始化远程执行客户端

        Args:
            multicast_group: UDP 多播组地址
            multicast_port: UDP 多播端口
            multicast_bind_address: 多播绑定地址（127.0.0.1 = 仅本机，需与 UE 编辑器设置一致）
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
        self._lock = asyncio.Lock()
        # 专用线程池：超时清理/断开操作不得与默认线程池争抢线程，避免死锁
        self._cleanup_executor = ThreadPoolExecutor(
            max_workers=1, thread_name_prefix="pyremote-cleanup"
        )

    def is_connected(self) -> bool:
        """检查是否已建立命令连接"""
        return self._connected and self._remote_exec is not None and self._remote_exec.has_command_connection()

    async def connect(self) -> bool:
        """发现并连接 UE 编辑器（加锁，串行化生命周期操作）

        流程：
        1. 先确保已有连接完整断开/清空
        2. 启动 UDP 广播发现编辑器节点
        3. 等待直到发现至少一个节点（或超时）
        4. 与第一个发现的节点建立 TCP 命令连接

        Returns:
            True 连接成功，False 超时或失败
        """
        async with self._lock:
            return await self._connect_impl()

    async def _connect_impl(self) -> bool:
        """内部连接实现（调用者需持有 self._lock）

        _connect_sync 内部已处理旧连接的 stop() 清理，无需前置清理。
        """
        try:
            loop = asyncio.get_event_loop()
            result = await loop.run_in_executor(None, self._connect_sync)
            return result
        except Exception as e:
            logger.error("连接失败: %s", e)
            self._connected = False
            return False

    def _force_cleanup(self):
        """强制清理所有 socket 资源"""
        if self._remote_exec:
            try:
                self._remote_exec.stop()
            except Exception as e:
                logger.debug("强制清理时 stop 出错: %s", e)
            self._remote_exec = None

    def _connect_sync(self) -> bool:
        """同步连接实现（在线程池中执行）

        任意异常（包括 start() 抛出的 socket 错误等）都会安全 stop 并清空，
        返回 False。
        """
        import time

        try:
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
            self._remote_exec.open_command_connection(self._remote_node_id)
            self._connected = True
            logger.info("TCP 命令连接已建立")
            return True
        except Exception as e:
            # 任意异常（包括 start() 抛出的 socket 错误等）都要安全 stop 并清空
            logger.error("_connect_sync 异常: %s", e)
            if self._remote_exec:
                try:
                    self._remote_exec.stop()
                except Exception:
                    pass
            self._remote_exec = None
            self._remote_node_id = None
            self._connected = False
            return False

    async def disconnect(self):
        """断开连接（加锁，串行化生命周期操作）"""
        async with self._lock:
            if self._remote_exec:
                loop = asyncio.get_event_loop()
                await loop.run_in_executor(self._cleanup_executor, self._disconnect_sync)

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
        async with self._lock:
            # 自动重连（全程持锁，消除 TOCTOU 竞态）
            if not self.is_connected():
                connected = await self._connect_impl()
                if not connected:
                    return {
                        "success": False,
                        "result": "未连接到 UE 编辑器，自动重连失败",
                        "command": code,
                        "output": [],
                    }

            # 双重检查（_connect_impl 成功后仍可能因异常导致未连接）
            if not self.is_connected():
                return {
                    "success": False,
                    "result": "未连接到 UE 编辑器",
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
                # 超时后使用专用线程池安全清理，不与默认线程池争抢避免死锁
                logger.warning("执行超时 (%.1fs)，正在清理连接...", self._execute_timeout)
                try:
                    loop = asyncio.get_event_loop()
                    await loop.run_in_executor(self._cleanup_executor, self._force_cleanup)
                except Exception:
                    pass
                self._remote_exec = None
                self._connected = False
                self._remote_node_id = None
                return {
                    "success": False,
                    "result": f"执行超时 ({self._execute_timeout}s)",
                    "command": code,
                    "output": [],
                }
            except Exception as e:
                self._connected = False
                logger.warning("执行异常: %s，正在清理连接...", e)
                try:
                    loop = asyncio.get_event_loop()
                    await loop.run_in_executor(self._cleanup_executor, self._force_cleanup)
                except Exception:
                    pass
                self._remote_exec = None
                self._remote_node_id = None
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

    async def close(self):
        """关闭客户端并释放线程池资源

        调用后本实例不可再用。如需优雅关闭，请先调用 disconnect()。
        """
        async with self._lock:
            if self._remote_exec:
                loop = asyncio.get_event_loop()
                await loop.run_in_executor(self._cleanup_executor, self._disconnect_sync)
        self._cleanup_executor.shutdown(wait=False)

    async def __aenter__(self):
        await self.connect()
        return self

    async def __aexit__(self, *args):
        await self.disconnect()

    def __repr__(self):
        status = "已连接" if self.is_connected() else "未连接"
        return f"<PythonRemoteExec {status}>"
