// Host test: g++ -std=c++20 -I android/jni/json/single_include tools/mcp_protocol_test.cpp -o /tmp/xst-mcp-test
#include "../android/jni/include/service/mcp_server.h"
#include <cassert>
#include <iostream>
using xst_mcp::json;
int main() {
    int calls = 0;
    std::string operation;
    json arguments;
    auto rpc = [&](const std::string &op, const json &args) {
        ++calls; operation = op; arguments = args;
        return json{{"ok", op != "memory.write"}, {"data", {{"regions", json::array()}, {"region_count", 10}, {"modules", json::array()}}}};
    };
    auto send = [&](const std::string &method, json params = json::object()) {
        return xst_mcp::Dispatch(json{{"jsonrpc", "2.0"}, {"id", "exact-id"}, {"method", method}, {"params", params}}.dump(), rpc);
    };
    auto init = send("initialize", {{"protocolVersion", "2025-11-25"}, {"capabilities", json::object()}, {"clientInfo", {{"name", "test"}, {"version", "1"}}}});
    assert(init.body["result"]["protocolVersion"] == "2025-11-25" && calls == 0);
    assert(send("initialize").body["error"]["code"] == -32602);
    auto list = send("tools/list").body["result"]["tools"];
    assert(list.size() == 57 && calls == 0);
    for (const auto &tool : list) assert(!tool.contains("operation"));
    assert(send("ping").body["result"].is_object());
    assert(send("missing").body["error"]["code"] == -32601);
    assert(xst_mcp::Dispatch("{", rpc).body["error"]["code"] == -32700);
    assert(xst_mcp::Dispatch("[]", rpc).status == 400);
    auto notification = xst_mcp::Dispatch(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", rpc);
    assert(notification.status == 202 && notification.body.is_null() && calls == 0);
    auto call = [&](std::string name, json args = json::object()) { return send("tools/call", {{"name", name}, {"arguments", args}}).body; };
    assert(call("unknown")["error"]["code"] == -32602);
    assert(call("android_target_set_pid", {{"pid", 0}})["error"]["code"] == -32602 && calls == 0);
    assert(call("android_memory_read", {{"address", 9007199254740993ULL}, {"size", 4}})["error"]["code"] == -32602 && calls == 0);
    assert(call("android_memory_read", {{"address", "0x123"}, {"size", 0}})["error"]["code"] == -32602);
    auto read = call("android_memory_read", {{"address", "0xFFFFFFFFFFFFFFFF"}, {"size", 4}});
    assert(read["id"] == "exact-id" && !read["result"]["isError"].get<bool>());
    assert(operation == "memory.read" && arguments["address"] == "0xFFFFFFFFFFFFFFFF" && arguments["allow_partial"] == false);
    assert(call("android_memory_write", {{"address", "0x1"}, {"data_hex", "AA"}})["result"]["isError"] == true);
    call("android_saved_add", {{"address", "0x1"}, {"value_kind", "pointer"}});
    assert(arguments["value_type"] == "i64");
    call("android_signature_scan_address", {{"address", "0x1"}, {"range_size", 10}});
    assert(operation == "signature.create" && arguments["range"] == 10 && !arguments.contains("range_size"));
    call("android_memory_view_open", {{"address", "0x1"}, {"view_format", "hex"}});
    assert(arguments["view_format"] == "hexadecimal");
    assert(!call("android_memory_regions")["result"]["structuredContent"]["data"].contains("regions"));
    auto points = json::array({{{"address", "0x1"}, {"bp_type", "execute"}, {"bp_scope", "all"}, {"length", 4}}});
    assert(call("android_breakpoint_set", {{"mode", "hwbp"}, {"points", points}}).contains("result"));
    points[0]["length"] = 9;
    assert(call("android_breakpoint_set", {{"mode", "hwbp"}, {"points", points}})["error"]["code"] == -32602);
    assert(call("android_breakpoint_record_update", {{"index", 0}, {"field", "X29"}, {"value", "0x123"}}).contains("result"));
    assert(call("android_breakpoint_record_update", {{"index", 0}, {"field", "x31"}, {"value", "0x123"}}).contains("error"));
    call("android_syscall_start"); assert(operation == "syscall.read");
    call("android_memory_scan_start", {{"mode", "equal"}, {"value_type", "f32"}, {"value", "1.2"}, {"rounding", "exact"}, {"alignment", 1}});
    assert(operation == "scan.start" && arguments["rounding"] == "exact" && arguments["alignment"] == 1);
    assert(call("android_memory_scan_start", {{"mode", "equal"}, {"alignment", 3}}).contains("error"));
    auto http = [&](std::string type) { return xst_mcp::HandleHttp("POST", json{{"content-type", type}}, R"({"jsonrpc":"2.0","id":1,"method":"ping"})", rpc); };
    assert(http("Application/JSON; charset=utf-8").status == 200);
    assert(http("application/json-invalid").status == 415);
    assert(http("text/plain").status == 415);
    assert(xst_mcp::HandleHttp("GET", json::object(), "", rpc).status == 405);
    assert(xst_mcp::HandleHttp("OPTIONS", json::object(), "", rpc).status == 204);
    assert(xst_mcp::HandleHttp("POST", json{{"origin", "https://example.com"}}, "", rpc).status == 403);
    assert(xst_mcp::HandleHttp("POST", json{{"mcp-protocol-version", "invalid"}}, "", rpc).status == 400);
    auto invalidText = [&](const std::string &, const json &) { return json{{"ok", true}, {"data", std::string(1, char(0xff))}}; };
    auto textReply = xst_mcp::Dispatch(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"android_bridge_ping"}})", invalidText);
    assert(textReply.body["result"].contains("content"));
    assert(!textReply.body.dump(-1, ' ', false, json::error_handler_t::replace).empty());
    std::cout << "MCP protocol, catalog, validation and operation adaptation passed\n";
}
