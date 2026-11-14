# 原生 MCP

来源：`android/jni/include/service/{http_server,mcp_server,mcp_catalog}.h`。

先启动 Android 独立工具的 mode 4（或 XSettings App 的 HTTP 前台服务），再连接下述端点。

### 电脑直接连接 MCP

先启动 mode 4，再在支持 **Streamable HTTP** 的 MCP 客户端中配置：

```json
{
  "mcpServers": {
    "xsettings": {
      "type": "http",
      "url": "http://<手机IP>:9494/mcp"
    }
  }
}
```

配置字段随客户端而异；端点是 `/mcp`，不是旧式 SSE 的 `/sse`。USB 可执行 `adb forward tcp:9494 tcp:9494`，把地址改为 `http://127.0.0.1:9494/mcp`。局域网直连无需端口转发。

- 与 `/api/rpc` 共用监听端口、前台保活、`ApplicationRuntime`、Driver 单例和操作互斥锁；不会再次 `runMode()` 或握手。目标 PID、扫描、查看器、断点和保存列表由所有客户端共享。
- 提供 57 个 `android_*` 工具，保留 Python 桥接中的工具名、参数默认值、十六进制字符串约束，以及内存映射结果裁剪等适配。电脑连接配置/发现工具及 `android://connection` 资源不在手机端提供。
- 支持协议版本 `2025-03-26`、`2025-06-18`、`2025-11-25`；未知版本在 initialize 返回 `2025-11-25` 供客户端决定是否继续。支持 `initialize`、`ping`、`tools/list`、`tools/call`、通知。
- 无会话 Streamable HTTP：POST 返回 JSON，通知返回空 `202`；不分配 `Mcp-Session-Id`，GET/DELETE 返回 `405`（不提供 SSE、恢复、服务端通知或会话删除）。初始化和列工具不会发送驱动请求。
- 工具执行失败通过 `result.isError=true` 返回；协议/参数错误通过 JSON-RPC error 返回。地址、寄存器保持字符串，避免 64 位精度丢失。
- 沿用现有 HTTP 服务的无认证访问方式；拒绝带 `Origin` 的浏览器请求。供受信任的原生 MCP 客户端使用。

可直接检查初始化和工具目录：

```bash
curl http://<手机IP>:9494/mcp -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"manual","version":"1"}}}'
curl http://<手机IP>:9494/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
```

维护：`mcp_server.h` 为协议与适配层，`mcp_catalog.h` 是工具目录。修改上游 Python 工具后可用 `python3 tools/generate_mcp_catalog.py windows/LuckyStarMcp.py` 再生目录（开发机需 Pydantic 2，App 无 Python 依赖），并检查操作映射及适配逻辑。主机协议验证：

```bash
g++ -std=c++20 -I android/jni/json/single_include tools/mcp_protocol_test.cpp -o /tmp/xst-mcp-test
/tmp/xst-mcp-test
```


检查工具目录与 Python 定义是否一致（不写文件）：

```bash
python3 tools/generate_mcp_catalog.py windows/LuckyStarMcp.py --check
```

`mcp_server.h` 和 `mcp_catalog.h` 应与 XSettings fork 保持一致，HTTP 集成仅同步 MCP 路由与请求头处理；不要覆盖 fork 的 JNI、PID 兜底和日志适配。
