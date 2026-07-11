"""UE Python Remote Execution 连接客户端

通过 UDP 组播发现 Unreal Editor 实例，通过 TCP 反向连接执行 Python 代码。

协议简述：
  1. UDP 组播发现：向 239.0.0.1:6766 发送 ping，编辑器回复 pong
  2. TCP 反向连接：客户端打开监听端口，发送 open_connection 通知编辑器连接
  3. 消息帧：4 字节大端长度头 + UTF-8 JSON 负载
"""

import asyncio
import json
import logging
import socket
import struct
import uuid
from typing import Any, Dict, Optional

logger = logging.getLogger(__name__)


class PythonRemoteExec:
    """UE Python Remote Execution 异步客户端

    使用 asyncio 实现与 Unreal Editor 内置 Python Remote Execution 插件的通信。

    典型用法::

        async with PythonRemoteExec() as remote:
            if remote.is_connected():
                result = await remote.execute("import unreal; print(unreal.get_editor_subsystem(unreal.EditorAssetSubsystem))")
                print(result)

    如果不使用上下文管理器::

        remote = PythonRemoteExec()
        await remote.connect()
        result = await remote.execute("print('hello')")
        await remote.disconnect()
    """

    # ---- 协议常量 ----
    MAGIC = "ue_python_remote"
    VERSION = 1

    # ---- 超时默认值 ----
    DEFAULT_DISCOVERY_TIMEOUT = 5.0   # 秒
    DEFAULT_EXECUTE_TIMEOUT = 30.0    # 秒

    # ---- 网络默认值 ----
    DEFAULT_MULTICAST_GROUP = "239.0.0.1"
    DEFAULT_MULTICAST_PORT = 6766
    DEFAULT_TCP_PORT = 6776

    def __init__(
        self,
        multicast_group: str = DEFAULT_MULTICAST_GROUP,
        multicast_port: int = DEFAULT_MULTICAST_PORT,
        tcp_port: int = DEFAULT_TCP_PORT,
        node_id: Optional[str] = None,
        discovery_timeout: float = DEFAULT_DISCOVERY_TIMEOUT,
        execute_timeout: float = DEFAULT_EXECUTE_TIMEOUT,
    ):
        """初始化客户端

        Args:
            multicast_group: UDP 组播地址
            multicast_port: UDP 组播端口
            tcp_port: 本地 TCP 监听端口（编辑器会主动连接回来）
            node_id: 本客户端节点标识，默认自动生成 UUID4
            discovery_timeout: 编辑器发现超时（秒）
            execute_timeout: 命令执行超时（秒）
        """
        self.multicast_group = multicast_group
        self.multicast_port = multicast_port
        self.tcp_port = tcp_port
        self.node_id = node_id or str(uuid.uuid4())
        self.discovery_timeout = discovery_timeout
        self.execute_timeout = execute_timeout

        # ---- 运行时状态 ----
        self._tcp_server: Optional[asyncio.AbstractServer] = None
        self._reader: Optional[asyncio.StreamReader] = None
        self._writer: Optional[asyncio.StreamWriter] = None
        self._ue_node_id: Optional[str] = None
        self._connected: bool = False

        # 编辑器信息（由 ping / pong 获取）
        self.editor_info: Dict[str, Any] = {}

    # ================================================================
    #  消息帧编解码
    # ================================================================

    @staticmethod
    def _encode_message(msg: dict) -> bytes:
        """将消息字典编码为带长度头的字节流

        帧格式：| 4 字节大端长度 | UTF-8 JSON 负载 |

        Args:
            msg: 消息字典

        Returns:
            编码后的字节串
        """
        json_bytes = json.dumps(msg, ensure_ascii=False).encode("utf-8")
        header = struct.pack(">I", len(json_bytes))
        return header + json_bytes

    @staticmethod
    def _decode_message(data: bytes) -> dict:
        """从带长度头的字节流中解码一条完整消息

        注意：调用方需保证 ``data`` 的首 4 字节为有效长度头，且长度与数据匹配。
        本方法不处理 TCP 粘包场景——粘包在 ``_recv_tcp_message`` 中处理。

        Args:
            data: 带 4 字节大端长度头的完整消息字节串

        Returns:
            解码后的消息字典

        Raises:
            ValueError: 数据不足以解析长度头或长度不匹配
        """
        if len(data) < 4:
            raise ValueError("消息太短，无法解析长度头（至少需要 4 字节）")
        payload_len = struct.unpack(">I", data[:4])[0]
        json_bytes = data[4 : 4 + payload_len]
        return json.loads(json_bytes.decode("utf-8"))

    def _build_message(self, msg_type: str, data: Optional[dict] = None) -> dict:
        """构建符合协议的标准消息结构

        Args:
            msg_type: 消息类型（ping / pong / command / command_result 等）
            data:   消息负载

        Returns:
            标准消息字典
        """
        return {
            "version": self.VERSION,
            "magic": self.MAGIC,
            "source": self.node_id,
            "type": msg_type,
            "data": data if data is not None else {},
        }

    # ================================================================
    #  UDP 组播发现
    # ================================================================

    async def _discover(self) -> bool:
        """通过 UDP 组播发现 UE 编辑器实例

        向组播组发送 ping，在同一个 socket 上等待编辑器的 pong 应答。
        pong 是单播回复，会直接发送回我们绑定的端口。

        Returns:
            True 表示成功发现并获取编辑器信息
        """
        loop = asyncio.get_running_loop()

        # 创建 UDP socket（同时用于发送组播和接收单播应答）
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", 0))  # 绑定随机端口
        sock.setblocking(False)

        try:
            # 发送 ping
            ping_msg = self._build_message("ping")
            data = self._encode_message(ping_msg)
            await loop.sock_sendto(
                sock, data, (self.multicast_group, self.multicast_port)
            )
            logger.debug(
                "已发送 UDP ping → %s:%d",
                self.multicast_group,
                self.multicast_port,
            )

            # 等待 pong（同一 socket）
            future = loop.sock_recv(sock, 65536)
            try:
                raw, addr = await asyncio.wait_for(
                    future, timeout=self.discovery_timeout
                )
            except asyncio.TimeoutError:
                logger.warning(
                    "UDP 发现超时（%.1fs）。请确认 UE 编辑器已启动且 Python Remote Execution 插件已启用。",
                    self.discovery_timeout,
                )
                return False

            response = self._decode_message(raw)

            # 校验响应
            if response.get("type") != "pong":
                logger.warning(
                    "收到非预期响应类型: %s（期望 pong）", response.get("type")
                )
                return False

            self._ue_node_id = response.get("source", "")
            self.editor_info = response.get("data", {})

            logger.info(
                "发现 UE 编辑器: node_id=%s 项目=%s 版本=%s",
                self._ue_node_id,
                self.editor_info.get("project_name", "?"),
                self.editor_info.get("engine_version", "?"),
            )
            return True

        except Exception as exc:
            logger.error("UDP 发现过程中出错: %s", exc)
            return False
        finally:
            sock.close()

    # ================================================================
    #  TCP 反向连接（编辑器作为客户端主动连接我们）
    # ================================================================

    async def _handle_tcp_client(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ):
        """asyncio.start_server 的回调，处理编辑器的 TCP 反向连接

        编辑器在收到 open_connection 后会作为 TCP 客户端连接本监听端口，
        此回调在连接建立时触发。
        """
        addr = writer.get_extra_info("peername")
        logger.info("UE 编辑器已通过 TCP 连接: %s:%d", addr[0], addr[1])

        self._reader = reader
        self._writer = writer
        self._connected = True

    async def _start_tcp_server(self) -> bool:
        """在本机启动 TCP 监听，等待编辑器连接

        Returns:
            True 表示监听启动成功
        """
        try:
            self._tcp_server = await asyncio.start_server(
                self._handle_tcp_client,
                host="0.0.0.0",
                port=self.tcp_port,
            )
            logger.info("TCP 监听已启动: 0.0.0.0:%d", self.tcp_port)
            return True
        except OSError as exc:
            logger.error("无法在端口 %d 启动 TCP 监听: %s", self.tcp_port, exc)
            return False

    async def _send_open_connection(self):
        """通过 UDP 组播发送 open_connection，通知编辑器连接本 TCP 端口"""
        open_msg = self._build_message(
            "open_connection", {"port": self.tcp_port}
        )

        # 使用独立 socket 发送（不需要接收响应，TCP 连接本身就是确认）
        loop = asyncio.get_running_loop()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
        sock.setblocking(False)

        try:
            data = self._encode_message(open_msg)
            await loop.sock_sendto(
                sock, data, (self.multicast_group, self.multicast_port)
            )
            logger.info(
                "已发送 open_connection → 期望编辑器连接端口 %d", self.tcp_port
            )
        finally:
            sock.close()

    # ================================================================
    #  TCP 消息收发
    # ================================================================

    async def _recv_tcp_message(self, timeout: Optional[float] = None) -> Optional[dict]:
        """从 TCP 流中读取一条完整消息

        处理粘包：先读 4 字节长度头，再按长度读取负载。

        Args:
            timeout: 超时时间（秒），默认使用 self.execute_timeout

        Returns:
            解析后的消息字典；超时或连接中断返回 None
        """
        if self._reader is None:
            return None

        timeout = timeout if timeout is not None else self.execute_timeout

        try:
            # 读取 4 字节大端长度头
            header = await asyncio.wait_for(
                self._reader.readexactly(4), timeout=timeout
            )
            payload_len = struct.unpack(">I", header)[0]

            # 读取负载
            body = await asyncio.wait_for(
                self._reader.readexactly(payload_len), timeout=timeout
            )

            return json.loads(body.decode("utf-8"))
        except asyncio.TimeoutError:
            logger.error("TCP 接收超时（%.1fs）", timeout)
            return None
        except asyncio.IncompleteReadError as exc:
            logger.error("TCP 连接中断（对方可能已关闭）: %s", exc)
            self._connected = False
            return None
        except (ConnectionError, OSError) as exc:
            logger.error("TCP 连接异常: %s", exc)
            self._connected = False
            return None

    async def _send_tcp_message(self, msg: dict) -> bool:
        """通过 TCP 连接发送一条消息

        Args:
            msg: 消息字典

        Returns:
            True 表示发送成功
        """
        if self._writer is None:
            return False

        try:
            data = self._encode_message(msg)
            self._writer.write(data)
            await self._writer.drain()
            return True
        except (ConnectionError, OSError) as exc:
            logger.error("TCP 发送失败: %s", exc)
            self._connected = False
            return False

    # ================================================================
    #  公共 API
    # ================================================================

    async def connect(self) -> bool:
        """执行完整连接流程：发现 → 监听 → 握手

        流程：
            1. UDP 组播 ping 发现编辑器
            2. 本机启动 TCP 监听端口
            3. 发送 open_connection 通知编辑器
            4. 阻塞等待编辑器的 TCP 反向连接

        Returns:
            True 表示连接成功建立
        """
        if self._connected:
            logger.info("已经连接，跳过重复 connect()")
            return True

        # 1. UDP 发现
        logger.info(
            "正在发现 UE 编辑器 (%s:%d) ...",
            self.multicast_group,
            self.multicast_port,
        )
        if not await self._discover():
            return False

        # 2. 启动 TCP 监听
        if not await self._start_tcp_server():
            self._ue_node_id = None
            self.editor_info = {}
            return False

        # 3. 发送 open_connection
        await self._send_open_connection()

        # 4. 等待编辑器建立 TCP 连接
        logger.info(
            "等待编辑器连接 TCP 端口 %d（超时 %.1fs）...",
            self.tcp_port,
            self.discovery_timeout,
        )
        loop = asyncio.get_running_loop()
        deadline = loop.time() + self.discovery_timeout
        while not self._connected:
            if loop.time() > deadline:
                logger.error("等待编辑器 TCP 连接超时")
                await self._cleanup_tcp()
                return False
            await asyncio.sleep(0.1)

        logger.info("Python Remote Exec 连接已建立 ✓")
        return True

    async def disconnect(self):
        """断开与编辑器的连接，释放所有资源"""
        # 通过 TCP 发送 close_connection（如果仍连通）
        if self._connected and self._writer is not None:
            try:
                close_msg = self._build_message("close_connection")
                await self._send_tcp_message(close_msg)
            except Exception:
                pass

        # 清理 TCP
        await self._cleanup_tcp()

        # 通过 UDP 发送 close_connection 作为最终通知
        try:
            loop = asyncio.get_running_loop()
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
            sock.setblocking(False)
            close_msg = self._build_message("close_connection")
            data = self._encode_message(close_msg)
            await loop.sock_sendto(
                sock, data, (self.multicast_group, self.multicast_port)
            )
            sock.close()
        except Exception:
            pass

        self._connected = False
        self._ue_node_id = None
        self.editor_info = {}
        logger.info("Python Remote Exec 已断开")

    async def execute(self, code: str, mode: str = "ExecuteStatement") -> dict:
        """在 UE 编辑器中执行 Python 代码

        Args:
            code: Python 代码字符串（或文件路径，取决于 mode）
            mode: 执行模式，可选值：
                - ``"ExecuteStatement"``（默认）：将 code 作为语句执行
                - ``"ExecuteFile"``：将 code 作为 .py 文件路径执行
                - ``"EvaluateStatement"``：将 code 作为表达式求值，返回结果

        Returns:
            执行结果字典，固定包含以下字段::

                {
                    "success": True / False,
                    "result": "...",     # stdout 输出或错误信息
                    "output": [...]      # 日志行列表
                }
        """
        if not await self._ensure_connected():
            return {
                "success": False,
                "result": "未连接到 UE 编辑器",
                "output": [],
            }

        # 映射 exec_mode → command 字段
        if mode == "ExecuteFile":
            command = "execute_file"
        else:
            command = "execute_statement"

        data: Dict[str, Any] = {
            "command": command,
            "unattended": True,
            "exec_mode": mode,
            "command_string": code,
        }

        cmd_msg = self._build_message("command", data)

        if not await self._send_tcp_message(cmd_msg):
            return {
                "success": False,
                "result": "命令发送失败（连接可能已断开）",
                "output": [],
            }

        # 等待编辑器返回 command_result
        response = await self._recv_tcp_message(timeout=self.execute_timeout)
        if response is None:
            return {
                "success": False,
                "result": "命令执行超时或连接中断",
                "output": [],
            }

        # 编辑器返回的响应结构：{..., "type": "command_result", "data": {...}}
        result_data = response.get("data")
        if not isinstance(result_data, dict):
            return {
                "success": False,
                "result": f"无效的响应格式: {response}",
                "output": [],
            }

        return result_data

    async def evaluate(self, expression: str) -> str:
        """执行 Python 表达式并返回求值结果

        这是 ``execute(code, mode="EvaluateStatement")`` 的便捷包装。

        Args:
            expression: Python 表达式（如 ``"2 + 3"`` 或 ``"unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)"``）

        Returns:
            表达式求值结果的字符串表示；失败时返回 ``"[错误] ..."`` 前缀的错误描述
        """
        result = await self.execute(expression, mode="EvaluateStatement")
        if result.get("success"):
            return str(result.get("result", ""))
        return f"[错误] {result.get('result', '未知错误')}"

    def is_connected(self) -> bool:
        """检查是否与编辑器保持连接

        Returns:
            True 表示连接正常
        """
        return (
            self._connected
            and self._reader is not None
            and self._writer is not None
        )

    # ================================================================
    #  内部辅助
    # ================================================================

    async def _ensure_connected(self) -> bool:
        """确保连接有效；若已断开则自动尝试重连

        Returns:
            True 表示连接可用
        """
        if self.is_connected():
            return True

        logger.info("检测到连接断开，将自动重连...")
        await self._cleanup_tcp()
        self._connected = False
        return await self.connect()

    async def _cleanup_tcp(self):
        """释放 TCP 相关资源（writer + server）"""
        # 关闭 writer
        if self._writer is not None:
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except Exception:
                pass
            self._writer = None

        self._reader = None

        # 关闭 server
        if self._tcp_server is not None:
            self._tcp_server.close()
            await self._tcp_server.wait_closed()
            self._tcp_server = None

    # ================================================================
    #  异步上下文管理器
    # ================================================================

    async def __aenter__(self) -> "PythonRemoteExec":
        """进入异步上下文：自动调用 connect()"""
        await self.connect()
        return self

    async def __aexit__(self, exc_type, exc_val, exc_tb) -> None:
        """退出异步上下文：自动调用 disconnect()"""
        await self.disconnect()

    def __repr__(self) -> str:
        status = "已连接" if self.is_connected() else "未连接"
        return (
            f"<PythonRemoteExec node_id={self.node_id[:8]}... "
            f"ue_node_id={self._ue_node_id} status={status}>"
        )
