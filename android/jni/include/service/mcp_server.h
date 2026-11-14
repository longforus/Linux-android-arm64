#pragma once
#include <nlohmann/json.hpp>
#include <regex>
#include <algorithm>
#include <string>
#include <string_view>
#include <functional>
#include <stdexcept>
namespace xst_mcp { using nlohmann::json; }
#include "mcp_catalog.h"

// Stateless MCP transport. The callback is the existing process-owned RPC dispatcher.
namespace xst_mcp {
struct Reply { int status; json body; };
inline json Error(const json &id, int code, const std::string &message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}
inline void Validate(const json &value, const json &schema, const std::string &path) {
    auto invalid = [&] { throw std::invalid_argument("Invalid argument: " + path); };
    if (schema.contains("anyOf")) {
        for (const auto &option : schema["anyOf"]) {
            try { Validate(value, option, path); return; } catch (const std::invalid_argument &) {}
        }
        invalid();
    }
    const std::string type = schema.value("type", "");
    if ((type == "object" && !value.is_object()) || (type == "array" && !value.is_array()) ||
        (type == "string" && !value.is_string()) || (type == "integer" && !value.is_number_integer()) ||
        (type == "boolean" && !value.is_boolean()) || (type == "null" && !value.is_null())) invalid();
    if (schema.contains("enum") && std::find(schema["enum"].begin(), schema["enum"].end(), value) == schema["enum"].end()) invalid();
    if (value.is_number()) {
        const double number = value.get<double>();
        if ((schema.contains("minimum") && number < schema["minimum"].get<double>()) ||
            (schema.contains("maximum") && number > schema["maximum"].get<double>()) ||
            (schema.contains("exclusiveMinimum") && number <= schema["exclusiveMinimum"].get<double>()) ||
            (schema.contains("exclusiveMaximum") && number >= schema["exclusiveMaximum"].get<double>())) invalid();
    }
    if (value.is_string()) {
        const auto &text = value.get_ref<const std::string &>();
        if (schema.contains("minLength") && text.size() < schema["minLength"].get<size_t>()) invalid();
        if (schema.contains("pattern")) {
            std::string pattern = schema["pattern"];
            auto flags = std::regex::ECMAScript;
            if (pattern.starts_with("(?i)")) { pattern.erase(0, 4); flags |= std::regex::icase; }
            if (!std::regex_search(text, std::regex(pattern, flags))) invalid();
        }
    }
    if (value.is_array()) {
        if ((schema.contains("minItems") && value.size() < schema["minItems"].get<size_t>()) ||
            (schema.contains("maxItems") && value.size() > schema["maxItems"].get<size_t>())) invalid();
        for (const auto &item : value) Validate(item, schema.at("items"), path + "[]");
    }
    if (value.is_object()) {
        for (const auto &key : schema.value("required", json::array())) if (!value.contains(key.get<std::string>())) invalid();
        const auto properties = schema.value("properties", json::object());
        for (const auto &[key, item] : value.items()) {
            if (properties.contains(key)) Validate(item, properties[key], path + "." + key);
            else if (schema.contains("additionalProperties") && schema["additionalProperties"] == false) invalid();
        }
    }
}
inline Reply Dispatch(std::string_view body, const std::function<json(const std::string &, const json &)> &rpc) {
    const auto request = json::parse(body, nullptr, false);
    if (request.is_discarded()) return {400, Error(nullptr, -32700, "Parse error")};
    if (!request.is_object() || request.value("jsonrpc", json{}) != "2.0" ||
        !request.contains("method") || !request["method"].is_string() ||
        (request.contains("id") && !request["id"].is_string() && !request["id"].is_number_integer()))
        return {400, Error(nullptr, -32600, "Invalid Request")};
    const auto method = request["method"].get<std::string>();
    const json id = request.value("id", json{});
    if (!request.contains("id")) {
        // Notifications never execute tools or return JSON-RPC responses.
        if (request.contains("params") && !request["params"].is_object()) return {400, json{}};
        return {202, json{}};
    }
    try {
        const auto params = request.value("params", json::object());
        if (!params.is_object()) throw std::invalid_argument("params must be an object");
        json result;
        if (method == "initialize") {
            if (!params.contains("protocolVersion") || !params["protocolVersion"].is_string() ||
                !params.contains("capabilities") || !params["capabilities"].is_object() ||
                !params.contains("clientInfo") || !params["clientInfo"].is_object())
                throw std::invalid_argument("initialize requires protocolVersion, capabilities and clientInfo");
            const std::string requested = params["protocolVersion"];
            const std::string version = (requested == "2025-03-26" || requested == "2025-06-18" || requested == "2025-11-25") ? requested : "2025-11-25";
            result = {{"protocolVersion", version}, {"capabilities", {{"tools", {{"listChanged", false}}}}},
                {"serverInfo", {{"name", "XSettings Native MCP"}, {"version", "1.0.0"}}},
                {"instructions", "Addresses and registers must be 0x-prefixed strings. Target, scans, viewer and breakpoints are shared with HTTP RPC and all clients."}};
        } else if (method == "ping") result = json::object();
        else if (method == "tools/list") {
            if (params.contains("cursor")) throw std::invalid_argument("This catalog is not paginated");
            json tools = Catalog();
            for (auto &tool : tools) tool.erase("operation");
            result = {{"tools", std::move(tools)}};
        } else if (method == "tools/call") {
            if (!params.contains("name") || !params["name"].is_string()) throw std::invalid_argument("name must be a string");
            const auto tool = std::find_if(Catalog().begin(), Catalog().end(), [&](const auto &entry) { return entry["name"] == params["name"]; });
            if (tool == Catalog().end()) return {200, Error(id, -32602, "Unknown tool")};
            json args = params.value("arguments", json::object());
            Validate(args, tool->at("inputSchema"), "arguments");
            for (const auto &[key, schema] : tool->at("inputSchema")["properties"].items())
                if (!args.contains(key) && schema.contains("default")) args[key] = schema["default"];
            const std::string op = tool->at("operation");
            if (op == "signature.create") { args["range"] = args["range_size"]; args.erase("range_size"); }
            if (op == "viewer.open" || op == "viewer.format")
                if (args["view_format"] == "hex") args["view_format"] = "hexadecimal";
            if (op == "scan.start" || op == "scan.refine") {
                if (args["mode"] == "pointer") args["value_type"] = "i64";
                if (args["mode"] == "string") args.erase("value_type");
            }
            if (op == "saved.add") {
                if (args["value_kind"] == "pointer") args["value_type"] = "i64";
                if (args["value_kind"] == "text") args["value_type"] = "i8";
            }
            for (auto it = args.begin(); it != args.end();) {
                if (it.value().is_null()) it = args.erase(it); else ++it;
            }
            json response = rpc(op, args);
            if (op == "memory.map" && response.contains("data") && response["data"].is_object()) {
                response["data"].erase("regions"); response["data"].erase("region_count");
            }
            if ((op == "syscall.start" || op == "cntvct.start") && response.value("ok", false)) {
                const auto log = rpc(op.substr(0, op.size() - 5) + "read", json::object());
                if (log.value("ok", false) && log.contains("data")) {
                    response["data"]["log"] = log["data"].value("log", "");
                    response["data"]["line_count"] = log["data"].value("line_count", 0);
                }
            }
            result = {{"content", json::array({{{"type", "text"}, {"text", response.dump()}}})},
                {"structuredContent", response}, {"isError", !response.value("ok", false)}};
        } else return {200, Error(id, -32601, "Method not found")};
        return {200, {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}};
    } catch (const std::invalid_argument &error) { return {200, Error(id, -32602, error.what())}; }
      catch (const std::exception &error) { return {200, Error(id, -32603, error.what())}; }
}
}
