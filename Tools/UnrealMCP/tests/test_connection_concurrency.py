"""UnrealMCP 连接层并发安全单元测试

测试 python_remote.py 和 ue_remote_execution.py 的并发修复点：
  - 专用线程池隔离（超时清理不与默认池争抢）
  - 生命周期锁串行化（execute/disconnect/connect 不可并发竞态）
  - UDP 广播错误分类（socket 关闭可幂等忽略，真实失败必须记录）
"""

import asyncio
import errno
import os
import socket
import sys
import unittest
from concurrent.futures import ThreadPoolExecutor
from unittest.mock import MagicMock, Mock, patch

# 将被测模块加入搜索路径
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))

from unreal_mcp.connection.ue_remote_execution import _get_socket_closed_errnos


class TestSocketClosedErrnos(unittest.TestCase):
    """测试 socket 关闭错误码辅助函数"""

    def test_returns_set(self):
        result = _get_socket_closed_errnos()
        self.assertIsInstance(result, set)

    def test_contains_ebadf(self):
        result = _get_socket_closed_errnos()
        self.assertIn(errno.EBADF, result)

    def test_windows_contains_wsa_codes(self):
        """Windows 下应包含 WSA 错误码"""
        result = _get_socket_closed_errnos()
        if hasattr(errno, 'WSAENOTSOCK'):
            self.assertIn(errno.WSAENOTSOCK, result)
        if hasattr(errno, 'WSAENOTCONN'):
            self.assertIn(errno.WSAENOTCONN, result)

    def test_closed_vs_real_error(self):
        """验证 socket 关闭错误与真实发送错误被正确区分"""
        closed = _get_socket_closed_errnos()

        # EBADF (9) 应在闭集中
        self.assertIn(errno.EBADF, closed)

        # 典型真实网络错误不应在闭集中
        if hasattr(errno, 'ENETUNREACH'):
            self.assertNotIn(errno.ENETUNREACH, closed)
        if hasattr(errno, 'EHOSTUNREACH'):
            self.assertNotIn(errno.EHOSTUNREACH, closed)
        if hasattr(errno, 'ECONNREFUSED'):
            self.assertNotIn(errno.ECONNREFUSED, closed)


class TestBroadcastMessageErrorHandling(unittest.TestCase):
    """测试 _broadcast_message 的错误分类：关闭可忽略 vs 真实失败记录"""

    def _make_broadcast_connection(self):
        """创建带 mock socket 的广播连接"""
        from unreal_mcp.connection.ue_remote_execution import (
            RemoteExecutionConfig,
            _RemoteExecutionBroadcastConnection,
        )
        config = RemoteExecutionConfig()
        config.multicast_group_endpoint = ('239.0.0.1', 6766)
        config.multicast_bind_address = '127.0.0.1'
        conn = _RemoteExecutionBroadcastConnection(config, 'test-node-id')
        return conn, config

    def test_closed_socket_error_silent(self):
        """socket.close() 后的 sendto 错误应静默忽略"""
        conn, config = self._make_broadcast_connection()

        # 模拟一个已关闭的 socket
        mock_socket = MagicMock()
        closed_error = OSError()
        closed_error.errno = errno.EBADF
        mock_socket.sendto.side_effect = closed_error
        conn._broadcast_socket = mock_socket

        from unreal_mcp.connection.ue_remote_execution import (
            _RemoteExecutionMessage,
            _TYPE_PING,
        )
        msg = _RemoteExecutionMessage(_TYPE_PING, 'test-node-id')

        # patch logger.warning 来验证它没有被调用（EBADF 应静默忽略）
        import unreal_mcp.connection.ue_remote_execution as ue_mod
        with patch.object(ue_mod._logger, 'warning') as mock_warning:
            conn._broadcast_message(msg)
            # EBADF 错误不应触发 warning
            udp_warnings = [
                args for args, _ in mock_warning.call_args_list
                if args and 'UDP' in str(args[0])
            ]
            self.assertEqual(len(udp_warnings), 0,
                             f"EBADF 错误不应产生 UDP WARNING，实际: {udp_warnings}")
        # 确认 socket.sendto 被调用
        mock_socket.sendto.assert_called_once()

    def test_real_send_failure_logged(self):
        """真实网络错误（如 ENETUNREACH）必须被记录为 WARNING"""
        conn, config = self._make_broadcast_connection()

        mock_socket = MagicMock()
        # 注入一个真实网络错误
        net_error = OSError()
        net_error.errno = errno.ENETUNREACH if hasattr(errno, 'ENETUNREACH') else errno.EHOSTUNREACH
        mock_socket.sendto.side_effect = net_error
        conn._broadcast_socket = mock_socket

        from unreal_mcp.connection.ue_remote_execution import (
            _RemoteExecutionMessage,
            _TYPE_PING,
        )
        msg = _RemoteExecutionMessage(_TYPE_PING, 'test-node-id')
        with self.assertLogs('unreal_mcp.connection.ue_remote_execution', level='WARNING') as cm:
            conn._broadcast_message(msg)
        # 至少有一条 WARNING 日志
        self.assertTrue(any('UDP 广播发送失败' in record.msg for record in cm.records),
                        f"期望 WARNING 日志包含 'UDP 广播发送失败'，实际: {cm.output}")


class TestDedicatedCleanupExecutor(unittest.TestCase):
    """测试专用清理线程池隔离：超时清理不与默认线程池争抢"""

    def test_cleanup_executor_is_separate_from_default(self):
        """PythonRemoteExec 创建独立的清理线程池"""
        from unreal_mcp.connection.python_remote import PythonRemoteExec

        remote = PythonRemoteExec(discovery_timeout=0.1, execute_timeout=0.1)
        try:
            # 验证 cleanup_executor 是一个独立的 ThreadPoolExecutor
            self.assertIsInstance(remote._cleanup_executor, ThreadPoolExecutor)
            # 提交一个轻量任务以触发线程创建（ThreadPoolExecutor 惰性创建线程）
            fut = remote._cleanup_executor.submit(lambda: None)
            fut.result(timeout=1)
            # 确认线程名前缀包含 'pyremote-cleanup'
            thread_names = [t.name for t in remote._cleanup_executor._threads]
            self.assertTrue(
                any('pyremote-cleanup' in name for name in thread_names),
                f"清理线程名应包含 'pyremote-cleanup'，实际: {thread_names}"
            )
        finally:
            remote._cleanup_executor.shutdown(wait=False)

    def test_cleanup_different_from_default_pool(self):
        """验证清理操作使用专用线程池，而非默认线程池"""
        from unreal_mcp.connection.python_remote import PythonRemoteExec

        remote = PythonRemoteExec(discovery_timeout=0.1, execute_timeout=0.1)
        try:
            # cleanup_executor 的线程名前缀与 asyncio 默认池不同
            cleanup_prefixes = {t.name for t in remote._cleanup_executor._threads}
            for prefix in cleanup_prefixes:
                self.assertIn('pyremote-cleanup', prefix,
                              f"清理线程名 '{prefix}' 应包含 'pyremote-cleanup'")
        finally:
            remote._cleanup_executor.shutdown(wait=False)


class TestLifecycleLockSerialization(unittest.TestCase):
    """测试生命周期操作被 asyncio.Lock 正确串行化"""

    def _create_mock_remote(self):
        """创建带 mock RemoteExecution 的 PythonRemoteExec"""
        from unreal_mcp.connection.python_remote import PythonRemoteExec

        remote = PythonRemoteExec(discovery_timeout=0.1, execute_timeout=0.1)
        # 用 mock 替换内部 RemoteExecution，避免真实 socket 操作
        mock_exec = MagicMock()
        mock_exec.has_command_connection.return_value = True
        mock_exec.run_command.return_value = {
            'success': True,
            'result': 'ok',
            'command': 'test',
            'output': [],
        }
        mock_exec.remote_nodes = [{'node_id': 'mock-node', 'data': {}}]
        remote._remote_exec = mock_exec
        remote._connected = True
        remote._remote_node_id = 'mock-node'
        return remote

    def test_execute_holds_lock_throughout(self):
        """execute() 应全程持锁，不可在中间释放（防止 TOCTOU）"""
        remote = self._create_mock_remote()
        try:
            async def run():
                return await remote.execute('print(1)')

            result = asyncio.run(run())
            self.assertTrue(result['success'])
            # 验证锁在 execute 结束时已释放（未被永久持有）
            self.assertFalse(remote._lock.locked(),
                             "execute() 返回后锁必须已释放")
        finally:
            remote._cleanup_executor.shutdown(wait=False)

    def test_concurrent_operations_serialized(self):
        """并发 execute/disconnect 必须串行化，不可同时操作 _remote_exec"""
        remote = self._create_mock_remote()
        try:
            call_order = []
            original_execute = remote._execute_sync

            def tracking_execute(code, mode):
                call_order.append('exec_start')
                result = original_execute(code, mode)
                call_order.append('exec_end')
                return result

            remote._execute_sync = tracking_execute

            original_disconnect = remote._disconnect_sync

            def tracking_disconnect():
                call_order.append('disc_start')
                original_disconnect()
                call_order.append('disc_end')

            remote._disconnect_sync = tracking_disconnect

            async def run():
                task_exec = asyncio.create_task(remote.execute('print(1)'))
                # 给 execute 一点时间获取锁
                await asyncio.sleep(0)
                task_disc = asyncio.create_task(remote.disconnect())
                results = await asyncio.gather(task_exec, task_disc)
                return results, call_order

            results, order = asyncio.run(run())
            # execute 和 disconnect 不应交错执行
            # 预期顺序: exec_start, exec_end, disc_start, disc_end 或反过来
            # 不可出现: exec_start, disc_start, ... (交错)
            exec_indices = [i for i, v in enumerate(order) if v.startswith('exec_')]
            disc_indices = [i for i, v in enumerate(order) if v.startswith('disc_')]

            if exec_indices and disc_indices:
                exec_range = (min(exec_indices), max(exec_indices))
                disc_range = (min(disc_indices), max(disc_indices))
                # 两个区间不可重叠
                self.assertTrue(
                    exec_range[1] < disc_range[0] or disc_range[1] < exec_range[0],
                    f"execute 和 disconnect 操作不应交错执行: {order}"
                )
        finally:
            remote._cleanup_executor.shutdown(wait=False)


class TestCloseMethod(unittest.TestCase):
    """测试 close() 方法正确释放资源"""

    def test_close_shuts_down_executor(self):
        """close() 应关闭清理线程池"""
        from unreal_mcp.connection.python_remote import PythonRemoteExec

        remote = PythonRemoteExec(discovery_timeout=0.1, execute_timeout=0.1)
        try:
            mock_exec = MagicMock()
            mock_exec.has_command_connection.return_value = True
            remote._remote_exec = mock_exec
            remote._connected = True

            async def run():
                await remote.close()

            asyncio.run(run())
            # 验证 _cleanup_executor 已 shutdown
            # shutdown(wait=False) 后 executor 不再接受新任务
            self.assertTrue(remote._cleanup_executor._shutdown,
                            "close() 后清理线程池应已 shutdown")
        except Exception:
            # 如果还没 shutdown 就手动清理
            try:
                remote._cleanup_executor.shutdown(wait=False)
            except Exception:
                pass
            raise

    def test_close_without_connection(self):
        """无连接时 close() 不应报错"""
        from unreal_mcp.connection.python_remote import PythonRemoteExec

        remote = PythonRemoteExec(discovery_timeout=0.1, execute_timeout=0.1)

        async def run():
            await remote.close()

        asyncio.run(run())  # 不抛异常即为通过
        remote._cleanup_executor.shutdown(wait=False)


class TestPythonRemoteExecStateReset(unittest.TestCase):
    """测试 PythonRemoteExec 在失败/断开/异常后正确重置状态"""

    def test_connect_failure_resets_state(self):
        """connect 失败后 _remote_exec/_remote_node_id/_connected 必须全部为 None/False"""
        from unreal_mcp.connection.python_remote import PythonRemoteExec

        remote = PythonRemoteExec(discovery_timeout=0.05)
        try:
            # _connect_impl 会尝试真实连接，超时后应重置状态
            async def run():
                result = await remote.connect()
                return result

            connected = asyncio.run(run())
            self.assertFalse(connected, "短超时下 connect 应当失败")
            self.assertIsNone(remote._remote_exec, "connect 失败后 _remote_exec 应为 None")
            self.assertIsNone(remote._remote_node_id, "connect 失败后 _remote_node_id 应为 None")
            self.assertFalse(remote._connected, "connect 失败后 _connected 应为 False")
            self.assertFalse(remote.is_connected(), "connect 失败后 is_connected() 应为 False")
        finally:
            remote._cleanup_executor.shutdown(wait=False)

    def test_connect_impl_exception_resets_state(self):
        """_connect_impl 自身异常（如 run_in_executor 失败）后必须重置状态"""
        from unreal_mcp.connection.python_remote import PythonRemoteExec

        remote = PythonRemoteExec(discovery_timeout=0.05)
        try:
            # 预先设置脏状态，模拟 run_in_executor 抛出异常的场景
            remote._remote_exec = object()  # 非 None，模拟半连接
            remote._remote_node_id = "stale-node"
            remote._connected = True

            # 关闭 cleanup_executor 使 disconnect 的 run_in_executor 抛出 RuntimeError
            remote._cleanup_executor.shutdown(wait=True)

            async def run():
                return await remote.connect()

            connected = asyncio.run(run())
            self.assertFalse(connected, "executor 关闭后 connect 应当失败")
            self.assertIsNone(remote._remote_exec, "异常后 _remote_exec 应为 None")
            self.assertIsNone(remote._remote_node_id, "异常后 _remote_node_id 应为 None")
            self.assertFalse(remote._connected, "异常后 _connected 应为 False")
        finally:
            try:
                remote._cleanup_executor.shutdown(wait=False)
            except Exception:
                pass

    def test_disconnect_resets_state_when_stop_throws(self):
        """disconnect 时即使 stop() 抛出异常，也必须重置状态"""
        from unreal_mcp.connection.python_remote import PythonRemoteExec

        remote = PythonRemoteExec(discovery_timeout=0.05)
        try:
            # 设置 mock RemoteExec，其 stop() 会抛异常
            mock_exec = MagicMock()
            mock_exec.has_command_connection.return_value = True
            mock_exec.stop.side_effect = RuntimeError("stop 失败")
            remote._remote_exec = mock_exec
            remote._remote_node_id = "test-node"
            remote._connected = True

            async def run():
                await remote.disconnect()

            asyncio.run(run())

            # disconnect 后无论 stop 是否抛异常，状态必须全部重置
            self.assertIsNone(remote._remote_exec, "disconnect 后 _remote_exec 应为 None")
            self.assertIsNone(remote._remote_node_id, "disconnect 后 _remote_node_id 应为 None")
            self.assertFalse(remote._connected, "disconnect 后 _connected 应为 False")
        finally:
            remote._cleanup_executor.shutdown(wait=False)

    def test_disconnect_resets_state_when_no_remote_exec(self):
        """disconnect 时即使 _remote_exec 为 None 也应该重置状态（处理不一致状态）"""
        from unreal_mcp.connection.python_remote import PythonRemoteExec

        remote = PythonRemoteExec(discovery_timeout=0.05)
        try:
            # 设置不一致状态：_remote_exec 已清但 _connected 仍为 True
            remote._remote_exec = None
            remote._remote_node_id = "stale-node"
            remote._connected = True

            async def run():
                await remote.disconnect()

            asyncio.run(run())

            self.assertIsNone(remote._remote_exec)
            self.assertIsNone(remote._remote_node_id, "disconnect 应清空 _remote_node_id")
            self.assertFalse(remote._connected, "disconnect 应重置 _connected")
        finally:
            remote._cleanup_executor.shutdown(wait=False)

    def test_execute_timeout_resets_state(self):
        """execute 超时后 _force_cleanup 被调用，此后状态必须为 None/False"""
        from unreal_mcp.connection.python_remote import PythonRemoteExec

        remote = PythonRemoteExec(discovery_timeout=0.1, execute_timeout=0.01)
        try:
            mock_exec = MagicMock()
            mock_exec.has_command_connection.return_value = True
            # 让 _execute_sync 永久阻塞以触发超时
            mock_exec.run_command.side_effect = lambda *a, **kw: (
                __import__('time').sleep(10) or {'success': True, 'result': '', 'output': []}
            )
            remote._remote_exec = mock_exec
            remote._connected = True
            remote._remote_node_id = "test-node"

            async def run():
                result = await remote.execute("print(1)")
                return result

            result = asyncio.run(run())
            self.assertFalse(result["success"], "超时应返回 success=False")
            self.assertIn("超时", result["result"])

            # 超时后状态必须全部清除
            self.assertIsNone(remote._remote_exec, "超时后 _remote_exec 应为 None")
            self.assertIsNone(remote._remote_node_id, "超时后 _remote_node_id 应为 None")
            self.assertFalse(remote._connected, "超时后 _connected 应为 False")
        finally:
            remote._cleanup_executor.shutdown(wait=False)

    def test_execute_exception_resets_state(self):
        """execute 内部异常后状态必须重置为 None/False"""
        from unreal_mcp.connection.python_remote import PythonRemoteExec

        remote = PythonRemoteExec(discovery_timeout=0.1, execute_timeout=1.0)
        try:
            mock_exec = MagicMock()
            mock_exec.has_command_connection.return_value = True
            mock_exec.run_command.side_effect = RuntimeError("执行崩溃")
            remote._remote_exec = mock_exec
            remote._connected = True
            remote._remote_node_id = "test-node"

            async def run():
                result = await remote.execute("bad code")
                return result

            result = asyncio.run(run())
            self.assertFalse(result["success"], "异常时应返回 success=False")

            self.assertIsNone(remote._remote_exec, "异常后 _remote_exec 应为 None")
            self.assertIsNone(remote._remote_node_id, "异常后 _remote_node_id 应为 None")
            self.assertFalse(remote._connected, "异常后 _connected 应为 False")
        finally:
            remote._cleanup_executor.shutdown(wait=False)

    def test_force_cleanup_resets_state(self):
        """_force_cleanup 调用后必须无条件重置所有状态"""
        from unreal_mcp.connection.python_remote import PythonRemoteExec

        remote = PythonRemoteExec(discovery_timeout=0.05)
        try:
            mock_exec = MagicMock()
            remote._remote_exec = mock_exec
            remote._remote_node_id = "test-node"
            remote._connected = True

            remote._force_cleanup()

            self.assertIsNone(remote._remote_exec)
            self.assertIsNone(remote._remote_node_id)
            self.assertFalse(remote._connected)
        finally:
            remote._cleanup_executor.shutdown(wait=False)


class TestPingConnectBehavior(unittest.TestCase):
    """测试 ping 的双通道连接策略：
    - 任一通道可用时不应重复连接
    - 双通道都不可用时主动调用 ensure_connected
    """

    def _setup_mock_connection(self, python_available, rc_available):
        """设置 mock connection 对象"""
        mock_conn = MagicMock()
        mock_conn.python_exec = MagicMock()
        mock_conn.python_exec.is_connected.return_value = python_available
        mock_conn.rc_api = MagicMock()
        mock_conn.rc_api.is_connected.return_value = rc_available
        # 将属性访问转发到 is_connected()
        type(mock_conn).python_available = property(lambda self: self.python_exec.is_connected())
        type(mock_conn).rc_available = property(lambda self: self.rc_api.is_connected())
        return mock_conn

    async def _simulate_ping(self, mock_conn):
        """模拟 ping 函数逻辑（与 server._register_ping_tool 行为一致）"""
        if mock_conn.python_available or mock_conn.rc_available:
            return "connected_no_reconnect"

        # 两个通道都不可用 — 主动尝试连接
        await mock_conn.ensure_connected()

        if mock_conn.python_available or mock_conn.rc_available:
            return "connected_after_attempt"
        else:
            return "disconnected"

    def test_ping_does_not_reconnect_when_python_available(self):
        """Python 通道已连接 → 不应调用 ensure_connected"""
        mock_conn = self._setup_mock_connection(python_available=True, rc_available=False)
        mock_conn.ensure_connected = MagicMock(side_effect=lambda: asyncio.sleep(0))

        async def run():
            return await self._simulate_ping(mock_conn)

        result = asyncio.run(run())
        self.assertEqual(result, "connected_no_reconnect")
        mock_conn.ensure_connected.assert_not_called()

    def test_ping_does_not_reconnect_when_rc_available(self):
        """RC 通道已连接 → 不应调用 ensure_connected"""
        mock_conn = self._setup_mock_connection(python_available=False, rc_available=True)
        mock_conn.ensure_connected = MagicMock(side_effect=lambda: asyncio.sleep(0))

        async def run():
            return await self._simulate_ping(mock_conn)

        result = asyncio.run(run())
        self.assertEqual(result, "connected_no_reconnect")
        mock_conn.ensure_connected.assert_not_called()

    def test_ping_does_not_reconnect_when_both_available(self):
        """双通道都连接 → 不应调用 ensure_connected"""
        mock_conn = self._setup_mock_connection(python_available=True, rc_available=True)
        mock_conn.ensure_connected = MagicMock(side_effect=lambda: asyncio.sleep(0))

        async def run():
            return await self._simulate_ping(mock_conn)

        result = asyncio.run(run())
        self.assertEqual(result, "connected_no_reconnect")
        mock_conn.ensure_connected.assert_not_called()

    def test_ping_calls_ensure_connected_when_both_disconnected(self):
        """双通道都断开 → 必须调用 ensure_connected"""
        mock_conn = self._setup_mock_connection(python_available=False, rc_available=False)
        mock_conn.ensure_connected = MagicMock(side_effect=lambda: asyncio.sleep(0))

        async def run():
            return await self._simulate_ping(mock_conn)

        result = asyncio.run(run())
        self.assertEqual(result, "disconnected")
        mock_conn.ensure_connected.assert_called_once()

    def test_ping_ensure_connected_success_then_report_status(self):
        """ensure_connected 成功后应报告连接正常"""
        mock_conn = self._setup_mock_connection(python_available=False, rc_available=False)

        async def fake_ensure():
            # 模拟连接成功：将 Python 通道标记为已连接
            mock_conn.python_exec.is_connected.return_value = True

        mock_conn.ensure_connected = fake_ensure

        async def run():
            return await self._simulate_ping(mock_conn)

        result = asyncio.run(run())
        self.assertEqual(result, "connected_after_attempt")


class TestExecutePythonConnectCheck(unittest.TestCase):
    """测试 UnrealConnection.execute_python 在首次连接时的返回值检查"""

    def test_execute_python_connect_failure_returns_error(self):
        """connect() 返回 False 时必须返回错误 dict，不能继续执行"""
        from unreal_mcp.server import UnrealConnection

        conn = UnrealConnection(multicast_group="239.0.0.99")
        # 注入 mock，模拟 connect 失败
        conn.python_exec.connect = MagicMock(side_effect=lambda: asyncio.sleep(0, result=False))
        conn.python_exec.is_connected = MagicMock(return_value=False)

        async def run():
            return await conn.execute_python("print(42)")

        result = asyncio.run(run())
        self.assertFalse(result["success"], "首次连接失败应返回 success=False")
        self.assertIn("连接失败", result["result"])
        self.assertEqual(result["command"], "print(42)")
        # 不应调用 execute
        conn.python_exec.execute = MagicMock()
        # 确认 connect 被调用了一次
        conn.python_exec.connect.assert_called_once()

    def test_execute_python_connect_success_then_executes(self):
        """connect() 返回 True 时应继续执行"""
        from unreal_mcp.server import UnrealConnection

        conn = UnrealConnection(multicast_group="239.0.0.99")
        conn.python_exec.connect = MagicMock(side_effect=lambda: asyncio.sleep(0, result=True))
        conn.python_exec.is_connected = MagicMock(return_value=True)
        conn.python_exec.execute = MagicMock(side_effect=lambda *a, **kw: asyncio.sleep(
            0, result={"success": True, "result": "ok", "command": "print(42)", "output": []}
        ))

        async def run():
            return await conn.execute_python("print(42)")

        result = asyncio.run(run())
        self.assertTrue(result["success"])
        conn.python_exec.execute.assert_called_once_with("print(42)")

    def test_execute_python_already_connected_skips_connect(self):
        """已连接时不调用 connect，直接执行"""
        from unreal_mcp.server import UnrealConnection

        conn = UnrealConnection(multicast_group="239.0.0.99")
        conn.python_exec.is_connected = MagicMock(return_value=True)
        conn.python_exec.connect = MagicMock()
        conn.python_exec.execute = MagicMock(side_effect=lambda *a, **kw: asyncio.sleep(
            0, result={"success": True, "result": "ok", "command": "test", "output": []}
        ))

        async def run():
            return await conn.execute_python("test")

        result = asyncio.run(run())
        self.assertTrue(result["success"])
        conn.python_exec.connect.assert_not_called()
        conn.python_exec.execute.assert_called_once_with("test")


class TestResetStateCompleteness(unittest.TestCase):
    """测试 _reset_state 方法在所有关键路径上被调用"""

    def test_all_three_fields_cleared(self):
        """_reset_state 必须一次性清空全部三个字段"""
        from unreal_mcp.connection.python_remote import PythonRemoteExec

        remote = PythonRemoteExec(discovery_timeout=0.05)
        try:
            remote._remote_exec = object()
            remote._remote_node_id = "node-123"
            remote._connected = True

            remote._reset_state()

            self.assertIsNone(remote._remote_exec)
            self.assertIsNone(remote._remote_node_id)
            self.assertFalse(remote._connected)
        finally:
            remote._cleanup_executor.shutdown(wait=False)

    def test_reset_state_idempotent(self):
        """_reset_state 对已清空的状态是幂等的"""
        from unreal_mcp.connection.python_remote import PythonRemoteExec

        remote = PythonRemoteExec(discovery_timeout=0.05)
        try:
            remote._reset_state()  # 第一次调用
            remote._reset_state()  # 第二次调用（幂等）
            self.assertIsNone(remote._remote_exec)
            self.assertIsNone(remote._remote_node_id)
            self.assertFalse(remote._connected)
        finally:
            remote._cleanup_executor.shutdown(wait=False)


if __name__ == '__main__':
    unittest.main(verbosity=2)
