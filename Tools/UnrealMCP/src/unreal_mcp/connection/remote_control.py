"""UE Remote Control API HTTP 客户端

通过 HTTP 协议与 Unreal Engine 5.8 的 Remote Control 插件通信，
支持函数调用、属性读写、资产搜索、批量操作等功能。
默认端口 30010，使用 httpx 异步客户端。
"""

import httpx
from typing import Any, Optional


class RemoteControlError(Exception):
    """Remote Control API 请求错误基类"""

    def __init__(self, message: str, status_code: Optional[int] = None, response_body: Optional[str] = None):
        super().__init__(message)
        self.status_code = status_code
        self.response_body = response_body


class ConnectionError(RemoteControlError):
    """连接失败（网络不可达、超时等）"""
    pass


class APIError(RemoteControlError):
    """API 返回了错误响应"""
    pass


class TimeoutError(RemoteControlError):
    """请求超时"""
    pass


class RemoteControlAPI:
    """UE Remote Control API 异步 HTTP 客户端

    使用方式:
        api = RemoteControlAPI()
        await api.connect()
        result = await api.call_function("/Game/BP.BP_C", "MyFunc", {"p": 1})
    """

    # Remote Control API 端点
    ENDPOINT_INFO = "/remote/info"
    ENDPOINT_CALL = "/remote/object/call"
    ENDPOINT_PROPERTY = "/remote/object/property"
    ENDPOINT_DESCRIBE = "/remote/object/describe"
    ENDPOINT_SEARCH = "/remote/search/assets"
    ENDPOINT_BATCH = "/remote/batch"

    def __init__(
        self,
        base_url: str = "http://127.0.0.1:30010",
        timeout: float = 30.0,
    ) -> None:
        """初始化 Remote Control API 客户端

        Args:
            base_url: UE Remote Control API 的根地址，默认 http://127.0.0.1:30010
            timeout: 请求超时秒数，默认 30 秒
        """
        self._base_url = base_url.rstrip("/")
        self._timeout = timeout
        self._connected = False
        # 创建异步 HTTP 客户端，默认超时和连接池复用
        self._client = httpx.AsyncClient(
            base_url=self._base_url,
            timeout=httpx.Timeout(timeout),
            headers={"Content-Type": "application/json"},
        )

    @property
    def base_url(self) -> str:
        """获取当前连接的 base URL"""
        return self._base_url

    @property
    def timeout(self) -> float:
        """获取当前超时设置（秒）"""
        return self._timeout

    def is_connected(self) -> bool:
        """检查是否已成功建立连接

        Returns:
            bool: True 表示之前 connect() 调用成功
        """
        return self._connected

    # ==================== 核心请求方法 ====================

    async def _request(
        self,
        method: str,
        endpoint: str,
        json_data: Optional[dict] = None,
    ) -> dict:
        """发送 HTTP 请求到 Remote Control API

        Args:
            method: HTTP 方法 (GET, PUT)
            endpoint: API 端点路径，如 /remote/info
            json_data: 请求体 JSON 数据（仅 PUT 请求）

        Returns:
            dict: API 响应的 JSON 数据

        Raises:
            ConnectionError: 网络连接失败
            TimeoutError: 请求超时
            APIError: API 返回错误状态码
        """
        try:
            response = await self._client.request(method, endpoint, json=json_data)
        except httpx.TimeoutException as exc:
            raise TimeoutError(f"请求超时 ({self._timeout}s): {method} {endpoint}") from exc
        except httpx.ConnectError as exc:
            raise ConnectionError(
                f"无法连接到 UE Remote Control ({self._base_url})，"
                f"请确认 UE 编辑器已启动且 Remote Control 插件已启用"
            ) from exc
        except httpx.NetworkError as exc:
            raise ConnectionError(f"网络错误: {exc}") from exc

        # HTTP 错误状态码处理
        if response.status_code >= 400:
            raise APIError(
                f"Remote Control 返回错误: HTTP {response.status_code} {response.reason_phrase}",
                status_code=response.status_code,
                response_body=response.text,
            )

        return response.json()

    # ==================== 公开 API ====================

    async def connect(self) -> dict:
        """测试与 UE Remote Control 的连接

        调用 GET /remote/info 获取所有可用路由，验证服务是否可达。

        Returns:
            dict: 包含可用路由列表的响应数据

        Raises:
            ConnectionError: 无法建立连接
            TimeoutError: 连接超时
        """
        result = await self._request("GET", self.ENDPOINT_INFO)
        self._connected = True
        return result

    async def call_function(
        self,
        object_path: str,
        function_name: str,
        params: Optional[dict] = None,
    ) -> dict:
        """调用 UObject 上的函数

        对应 PUT /remote/object/call

        Args:
            object_path: 目标对象的路径，如 "/Game/BP_C.Default__BP_C"
            function_name: 要调用的函数名
            params: 函数参数字典，无参数时传 None

        Returns:
            dict: 函数返回值

        Example:
            result = await api.call_function(
                "/Game/BP_C.Default__BP_C",
                "DoSomething",
                {"TargetActor": "/Game/Map.Actor_1"}
            )
        """
        body = {
            "objectPath": object_path,
            "functionName": function_name,
        }
        if params:
            body["parameters"] = params

        return await self._request("PUT", self.ENDPOINT_CALL, body)

    async def get_property(self, object_path: str, property_name: str) -> Any:
        """读取 UObject 的属性值

        对应 PUT /remote/object/property (access=READ_ACCESS)

        Args:
            object_path: 目标对象的路径
            property_name: 要读取的属性名

        Returns:
            Any: 属性值，类型取决于 UE 侧属性类型

        Example:
            value = await api.get_property(
                "/Game/BP_C.Default__BP_C",
                "Health"
            )
        """
        body = {
            "objectPath": object_path,
            "propertyName": property_name,
            "access": "READ_ACCESS",
        }
        result = await self._request("PUT", self.ENDPOINT_PROPERTY, body)
        return result.get("propertyValue", result)

    async def set_property(
        self, object_path: str, property_name: str, value: Any
    ) -> dict:
        """写入 UObject 的属性值

        对应 PUT /remote/object/property (access=WRITE_ACCESS)

        Args:
            object_path: 目标对象的路径
            property_name: 要写入的属性名
            value: 新属性值

        Returns:
            dict: 写入操作响应

        Example:
            await api.set_property(
                "/Game/BP_C.Default__BP_C",
                "Speed",
                600.0
            )
        """
        body = {
            "objectPath": object_path,
            "propertyName": property_name,
            "access": "WRITE_ACCESS",
            "propertyValue": value,
        }
        return await self._request("PUT", self.ENDPOINT_PROPERTY, body)

    async def search_assets(
        self,
        class_names: Optional[list[str]] = None,
        package_paths: Optional[list[str]] = None,
        search_string: str = "",
        recursive: bool = True,
    ) -> list:
        """搜索 UE 编辑器中的资产

        对应 PUT /remote/search/assets

        Args:
            class_names: 限定资产类型列表，如 ["StaticMesh", "SkeletalMesh"]，None 表示不限
            package_paths: 限定搜索路径列表，如 ["/Game/Meshes"]，None 表示不限
            search_string: 资产名搜索关键词，空字符串表示不按名称筛选
            recursive: 是否递归搜索子目录，默认 True

        Returns:
            list: 匹配的资产信息列表

        Example:
            assets = await api.search_assets(
                class_names=["StaticMesh"],
                package_paths=["/Game/"],
                search_string="SM_"
            )
        """
        # 构建查询条件
        query: dict = {}
        if class_names:
            query["classNames"] = class_names
        if package_paths:
            query["packagePaths"] = package_paths
        if search_string:
            query["searchString"] = search_string

        # 构建过滤条件
        filter_data: dict = {}
        if class_names:
            filter_data["classNames"] = class_names
        if package_paths:
            filter_data["packagePaths"] = package_paths
        if recursive:
            filter_data["recursivePaths"] = True

        body: dict = {}
        if query:
            body["query"] = query
        if filter_data:
            body["filter"] = filter_data

        result = await self._request("PUT", self.ENDPOINT_SEARCH, body)
        # 响应中的 Assets 字段包含结果列表
        return result.get("Assets", result)

    async def describe_object(self, object_path: str) -> dict:
        """获取 UObject 的属性和函数列表

        对应 PUT /remote/object/describe

        Args:
            object_path: 目标对象的路径

        Returns:
            dict: 包含 Functions 和 Properties 列表的响应

        Example:
            desc = await api.describe_object("/Game/BP_C.Default__BP_C")
            print(desc["Functions"])   # 可调用的函数列表
            print(desc["Properties"])  # 可读写的属性列表
        """
        body = {"objectPath": object_path}
        return await self._request("PUT", self.ENDPOINT_DESCRIBE, body)

    async def batch(self, requests: list[dict]) -> list[dict]:
        """批量执行多个 Remote Control 请求

        对应 PUT /remote/batch

        Args:
            requests: 请求列表，每一项是一个完整的请求描述字典。
                      格式: [{"RequestId": 1, "URL": "/remote/object/call", "Verb": "PUT", "Body": {...}}, ...]

        Returns:
            list[dict]: 每个请求对应的响应列表

        Example:
            results = await api.batch([
                {
                    "RequestId": 1,
                    "URL": "/remote/object/property",
                    "Verb": "PUT",
                    "Body": {"objectPath": "/Game/BP.BP_C", "propertyName": "X", "access": "READ_ACCESS"}
                },
                {
                    "RequestId": 2,
                    "URL": "/remote/object/call",
                    "Verb": "PUT",
                    "Body": {"objectPath": "/Game/BP.BP_C", "functionName": "Tick"}
                }
            ])
        """
        body = {"Requests": requests}
        result = await self._request("PUT", self.ENDPOINT_BATCH, body)
        # 响应中的 Responses 字段包含结果列表
        return result.get("Responses", result)

    # ==================== 资源管理 ====================

    async def close(self) -> None:
        """关闭 HTTP 客户端连接"""
        await self._client.aclose()
        self._connected = False

    async def __aenter__(self) -> "RemoteControlAPI":
        """支持 async with 语法"""
        return self

    async def __aexit__(self, exc_type, exc_val, exc_tb) -> None:
        """退出 async with 块时自动关闭连接"""
        await self.close()
