#pragma once
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <nlohmann/json.hpp>
#include <variant>
#include <sched.h>
#include <sys/mount.h>
#include "../../thread-pool/include/BS_thread_pool.hpp"
#include "../base/system_log.h"
#include "../application/service.h"
#include "../memory/signature_scanner.h"
#include "mcp_server.h"

// ============================================================================
// HTTP 服务器模块
// ============================================================================

extern "C"
{
    extern const unsigned char cloudflared_blob_start[];
    extern const unsigned char cloudflared_blob_end[];
}

namespace
{
    using nlohmann::json;

    constexpr std::uint16_t kServerPort = 9494;
    constexpr int kListenBacklog = 32;
    constexpr std::size_t kMaxHttpHeaderBytes = 64 * 1024;
    constexpr std::size_t kMaxHttpBodyBytes = 16 * 1024 * 1024;
    constexpr std::string_view kCloudflaredPath = "/data/akernel/.cloudflared";
    constexpr std::string_view kCloudflaredTempPath = "/data/akernel/.cloudflared.tmp";

    std::string toLowerAscii(std::string_view input)
    {
        std::string out(input);
        for (auto &ch : out) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        return out;
    }

    // 打印系统错误信息
    void printErrno(std::string_view action)
    {
        const int error = errno;
        LS_LOGE_TAG_FMT("HTTP", "{}: errno={} ({})", action, error, std::strerror(error));
    }

    bool IsLanInterface(std::string_view interfaceName)
    {
        constexpr std::array<std::string_view, 6> prefixes = {"wlan", "eth", "ap", "rndis", "usb", "bt-pan"};
        return std::ranges::any_of(prefixes, [interfaceName](std::string_view prefix) { return interfaceName.starts_with(prefix); });
    }

    void PrintLanHttpAddresses()
    {
        ifaddrs *interfaces = nullptr;
        if (getifaddrs(&interfaces) != 0)
        {
            printErrno("枚举局域网地址失败");
            return;
        }

        bool found = false;
        std::vector<std::string> printedAddresses;
        for (const ifaddrs *entry = interfaces; entry != nullptr; entry = entry->ifa_next)
        {
            if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) continue;
            if ((entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0) continue;

            const std::string_view interfaceName = entry->ifa_name != nullptr ? entry->ifa_name : "";
            if (!IsLanInterface(interfaceName)) continue;

            char addressBuffer[INET_ADDRSTRLEN]{};
            const auto *ipv4Address = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
            if (inet_ntop(AF_INET, &ipv4Address->sin_addr, addressBuffer, sizeof(addressBuffer)) == nullptr) continue;

            const std::string address = addressBuffer;
            if (std::ranges::find(printedAddresses, address) != printedAddresses.end()) continue;
            printedAddresses.push_back(address);
            found = true;
            LS_LOGI_TAG_FMT("HTTP", "局域网地址 [{}]: http://{}:{}", interfaceName, address, kServerPort);
            LS_LOGI_TAG_FMT("HTTP", "局域网 RPC [{}]: http://{}:{}/api/rpc", interfaceName, address, kServerPort);
        }
        freeifaddrs(interfaces);

        if (!found) LS_LOGW_TAG_FMT("HTTP", "未检测到局域网 IPv4 地址，HTTP 服务仍监听 http://0.0.0.0:{}", kServerPort);
    }

    bool WritePrivateEtcFile(const char *path, std::string_view content)
    {
        const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) return false;

        std::size_t written = 0;
        while (written < content.size())
        {
            const ssize_t result = write(fd, content.data() + written, content.size() - written);
            if (result < 0)
            {
                if (errno == EINTR) continue;
                close(fd);
                return false;
            }
            if (result == 0)
            {
                close(fd);
                return false;
            }
            written += static_cast<std::size_t>(result);
        }

        return close(fd) == 0;
    }

    bool ConfigureCloudflaredDns()
    {
        if (unshare(CLONE_NEWNS) != 0) return false;
        if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) return false;
        if (mount("tmpfs", "/system/etc", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755,size=64k") != 0) return false;

        constexpr std::string_view resolvConf = "nameserver 1.1.1.1\n"
                                                "nameserver 8.8.8.8\n"
                                                "options timeout:2 attempts:2\n";
        constexpr std::string_view hosts = "127.0.0.1 localhost\n"
                                           "::1 ip6-localhost\n";

        if (!WritePrivateEtcFile("/system/etc/resolv.conf", resolvConf)) return false;
        if (!WritePrivateEtcFile("/system/etc/hosts", hosts)) return false;
        return setenv("SSL_CERT_DIR", "/apex/com.android.conscrypt/cacerts", 1) == 0;
    }

    bool ExtractCloudflared()
    {
        const std::size_t blobSize = static_cast<std::size_t>(cloudflared_blob_end - cloudflared_blob_start);
        if (blobSize == 0) return false;

        const int fd = open(kCloudflaredTempPath.data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0700);
        if (fd < 0)
        {
            printErrno("创建 cloudflared 临时文件失败");
            return false;
        }

        std::size_t written = 0;
        while (written < blobSize)
        {
            const ssize_t result = write(fd, cloudflared_blob_start + written, blobSize - written);
            if (result < 0)
            {
                if (errno == EINTR) continue;
                printErrno("释放 cloudflared 失败");
                close(fd);
                unlink(kCloudflaredTempPath.data());
                return false;
            }
            if (result == 0)
            {
                close(fd);
                unlink(kCloudflaredTempPath.data());
                return false;
            }
            written += static_cast<std::size_t>(result);
        }

        if (fchmod(fd, 0700) != 0)
        {
            printErrno("设置 cloudflared 权限失败");
            close(fd);
            unlink(kCloudflaredTempPath.data());
            return false;
        }
        close(fd);

        if (rename(kCloudflaredTempPath.data(), kCloudflaredPath.data()) != 0)
        {
            printErrno("安装 cloudflared 失败");
            unlink(kCloudflaredTempPath.data());
            return false;
        }
        return true;
    }

    void StartCloudflared()
    {
        if (!ExtractCloudflared()) return;

        const pid_t pid = fork();
        if (pid < 0)
        {
            printErrno("启动 cloudflared 失败");
            unlink(kCloudflaredPath.data());
            return;
        }
        if (pid == 0)
        {
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            if (!ConfigureCloudflaredDns())
            {
                printErrno("配置 cloudflared 私有 DNS 失败");
                _exit(126);
            }
            execl(kCloudflaredPath.data(), kCloudflaredPath.data(), "tunnel", "--no-autoupdate", "--edge-ip-version", "4", "--url", "http://127.0.0.1:9494", static_cast<char *>(nullptr));
            _exit(127);
        }

        LS_LOGI_TAG_FMT("HTTP", "cloudflared Tunnel 已启动，PID={}，公网地址将写入 /sdcard/log.txt", pid);
    }

    bool sendAll(int fd, std::string_view data)
    {
        std::size_t sentTotal = 0;
        while (sentTotal < data.size())
        {
            const ssize_t sent = send(fd, data.data() + sentTotal, data.size() - sentTotal, MSG_NOSIGNAL);
            if (sent < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                return false;
            }

            if (sent == 0)
            {
                return false;
            }
            sentTotal += static_cast<std::size_t>(sent);
        }
        return true;
    }

    bool sendHttpResponse(int fd, int statusCode, std::string_view statusText, std::string_view body, std::string_view extraHeaders = "")
    {
        const std::string response = std::format("HTTP/1.1 {} {}\r\n"
                                                 "Content-Type: application/json; charset=utf-8\r\n"
                                                 "Content-Length: {}\r\n"
                                                 "Connection: close\r\n"
                                                 "Access-Control-Allow-Origin: *\r\n"
                                                 "Access-Control-Allow-Headers: Content-Type, Authorization, MCP-Protocol-Version, Mcp-Session-Id\r\n"
                                                 "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                                                 "{}\r\n{}",
                                                 statusCode, statusText, body.size(), extraHeaders, body);
        return sendAll(fd, response);
    }

    bool receiveHttpRequest(int fd, std::string *method, std::string *path, std::string *body, int *errorStatus, json *headers)
    {
        std::string request;
        request.reserve(4096);
        std::array<char, 4096> chunk{};
        std::size_t headerEnd = std::string::npos;

        while (headerEnd == std::string::npos)
        {
            const ssize_t received = recv(fd, chunk.data(), chunk.size(), 0);
            if (received == 0) return false;
            if (received < 0)
            {
                if (errno == EINTR) continue;
                return false;
            }

            request.append(chunk.data(), static_cast<std::size_t>(received));
            if (request.size() > kMaxHttpHeaderBytes)
            {
                *errorStatus = 431;
                return false;
            }
            headerEnd = request.find("\r\n\r\n");
        }

        const std::size_t requestLineEnd = request.find("\r\n");
        if (requestLineEnd == std::string::npos)
        {
            *errorStatus = 400;
            return false;
        }

        const std::string_view requestLine(request.data(), requestLineEnd);
        const std::size_t firstSpace = requestLine.find(' ');
        const std::size_t secondSpace = firstSpace == std::string_view::npos ? std::string_view::npos : requestLine.find(' ', firstSpace + 1);
        if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos)
        {
            *errorStatus = 400;
            return false;
        }
        *method = std::string(requestLine.substr(0, firstSpace));
        *path = std::string(requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1));

        std::size_t contentLength = 0;
        std::size_t lineStart = requestLineEnd + 2;
        while (lineStart < headerEnd)
        {
            const std::size_t lineEnd = request.find("\r\n", lineStart);
            if (lineEnd == std::string::npos || lineEnd > headerEnd) break;

            const std::string_view line(request.data() + lineStart, lineEnd - lineStart);
            const std::size_t colon = line.find(':');
            if (colon != std::string_view::npos) {
                const auto key = toLowerAscii(line.substr(0, colon));
                std::string value(line.substr(colon + 1));
                const auto first = value.find_first_not_of(" \t");
                const auto last = value.find_last_not_of(" \t");
                value = first == std::string::npos ? "" : value.substr(first, last - first + 1);
                if (headers->contains(key)) { *errorStatus = 400; return false; }
                (*headers)[key] = value;
            }
            if (colon != std::string_view::npos && toLowerAscii(line.substr(0, colon)) == "content-length")
            {
                std::string_view value = line.substr(colon + 1);
                const std::size_t valueStart = value.find_first_not_of(" \t");
                const std::size_t valueEnd = value.find_last_not_of(" \t");
                if (valueStart == std::string_view::npos)
                {
                    *errorStatus = 400;
                    return false;
                }
                value = value.substr(valueStart, valueEnd - valueStart + 1);
                const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), contentLength);
                if (ec != std::errc{} || ptr != value.data() + value.size())
                {
                    *errorStatus = 400;
                    return false;
                }
            }
            lineStart = lineEnd + 2;
        }

        if (contentLength > kMaxHttpBodyBytes)
        {
            *errorStatus = 413;
            return false;
        }

        const std::size_t bodyStart = headerEnd + 4;
        while (request.size() - bodyStart < contentLength)
        {
            const ssize_t received = recv(fd, chunk.data(), chunk.size(), 0);
            if (received == 0)
            {
                *errorStatus = 400;
                return false;
            }
            if (received < 0)
            {
                if (errno == EINTR) continue;
                return false;
            }
            request.append(chunk.data(), static_cast<std::size_t>(received));
        }

        body->assign(request.data() + bodyStart, contentLength);
        return true;
    }

    json rpcProtocolError(std::string_view message) { return {{"ok", false}, {"error", message}}; }

    // 统一命令派发入口：网络层仅接受 JSON 请求并返回 JSON 响应。
    std::string DispatchCommandUnified(ApplicationService &service, const std::string &request)
    {
        const auto parsedReq = json::parse(request, nullptr, false);
        if (parsedReq.is_discarded())
        {
            return json({{"ok", false}, {"error", "请求必须是 JSON 字符串对象"}}).dump(-1, ' ', false, json::error_handler_t::replace);
        }

        if (!parsedReq.is_object())
        {
            return json({{"ok", false}, {"error", "请求必须是 JSON 对象"}}).dump(-1, ' ', false, json::error_handler_t::replace);
        }

        if (const auto operation = parsedReq.find("operation"); operation != parsedReq.end())
        {
            if (!operation->is_string())
            {
                return rpcProtocolError("operation 字段必须是字符串").dump(-1, ' ', false, json::error_handler_t::replace);
            }

            const auto &operationName = operation->get_ref<const std::string &>();
            if (const auto params = parsedReq.find("params"); params != parsedReq.end())
            {
                if (!params->is_object())
                {
                    json error = rpcProtocolError("params 字段必须是对象");
                    error["operation"] = operationName;
                    return error.dump(-1, ' ', false, json::error_handler_t::replace);
                }
                return service.dispatch(operationName, *params).dump(-1, ' ', false, json::error_handler_t::replace);
            }

            return service.dispatch(operationName, json::object()).dump(-1, ' ', false, json::error_handler_t::replace);
        }

        return rpcProtocolError("请求缺少 operation 字段").dump(-1, ' ', false, json::error_handler_t::replace);
    }

    void HandleClientConnection(ApplicationService &service, int clientFd, sockaddr_in clientAddr, const std::string &healthResponse)
    {
        char clientIp[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp)) == nullptr)
        {
            std::strncpy(clientIp, "未知地址", sizeof(clientIp) - 1);
            clientIp[sizeof(clientIp) - 1] = '\0';
        }

        const auto clientPort = ntohs(clientAddr.sin_port);
        std::string method;
        std::string path;
        std::string body;
        int errorStatus = 0;
        json headers = json::object();
        if (!receiveHttpRequest(clientFd, &method, &path, &body, &errorStatus, &headers))
        {
            if (errorStatus == 413) sendHttpResponse(clientFd, 413, "Payload Too Large", R"({"ok":false,"error":"请求体过大"})");
            else if (errorStatus == 431) sendHttpResponse(clientFd, 431, "Request Header Fields Too Large", R"({"ok":false,"error":"请求头过大"})");
            else if (errorStatus != 0) sendHttpResponse(clientFd, 400, "Bad Request", R"({"ok":false,"error":"HTTP 请求格式无效"})");
            close(clientFd);
            return;
        }

        LS_LOGD_TAG_FMT("HTTP", "收到请求: {}:{} {} {}", clientIp, clientPort, method, path);
        if (path == "/mcp" || path == "/mcp/")
        {
            const auto reply = xst_mcp::HandleHttp(method, headers, body, [&](const std::string &op, const json &params) {
                return service.dispatch(op, params);
            });
            sendHttpResponse(clientFd, reply.status,
                reply.status == 202 ? "Accepted" : reply.status == 204 ? "No Content" :
                reply.status == 400 ? "Bad Request" : reply.status == 403 ? "Forbidden" :
                reply.status == 405 ? "Method Not Allowed" : reply.status == 415 ? "Unsupported Media Type" : "OK",
                reply.body.is_null() ? "" : reply.body.dump(-1, ' ', false, json::error_handler_t::replace),
                reply.status == 405 ? "Allow: POST, OPTIONS\r\n" : "");
        }
        else if (method == "OPTIONS")
        {
            sendHttpResponse(clientFd, 204, "No Content", "");
        }
        else if (method == "GET" && path == "/health")
        {
            sendHttpResponse(clientFd, 200, "OK", healthResponse);
        }
        else if (method == "POST" && path == "/api/rpc")
        {
            const std::string response = DispatchCommandUnified(service, body);
            if (!sendHttpResponse(clientFd, 200, "OK", response)) printErrno("发送 HTTP 回复失败");
        }
        else if (method != "GET" && method != "POST")
        {
            sendHttpResponse(clientFd, 405, "Method Not Allowed", R"({"ok":false,"error":"不支持的 HTTP 方法"})");
        }
        else
        {
            sendHttpResponse(clientFd, 404, "Not Found", R"({"ok":false,"error":"接口不存在"})");
        }
        close(clientFd);
    }
} // namespace

// 程序入口：初始化 HTTP 服务并处理客户端请求。
int http_server(ApplicationRuntime &application)
{
    const int serverFd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (serverFd < 0)
    {
        printErrno("创建套接字失败");
        return 1;
    }
    constexpr int enableReuse = 1;
    if (setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &enableReuse, sizeof(enableReuse)) < 0)
    {
        printErrno("设置套接字选项失败");
        close(serverFd);
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(kServerPort);

    if (bind(serverFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
    {
        printErrno("绑定端口失败");
        close(serverFd);
        return 1;
    }

    if (listen(serverFd, kListenBacklog) < 0)
    {
        printErrno("开始监听失败");
        close(serverFd);
        return 1;
    }

    // Query the running kernel once; passive health polling must not send driver requests.
    const auto capabilities = application.resources().debug.capabilities();
    const std::string healthResponse = json{{"ok", true}, {"service", "LS_KTool"}, {"transport", "http"},
        {"kernel_version", capabilities.value("kernel_version", std::string{})},
        {"debug_abi", capabilities.at("debug_abi")}, {"debug_status", capabilities.at("debug_status")}}.dump(-1, ' ', false, json::error_handler_t::replace);

    LS_LOGI_TAG_FMT("HTTP", "服务端已监听 http://0.0.0.0:{}", kServerPort);
    LS_LOGI_TAG_FMT("HTTP", "MCP 端点: http://<手机IP>:{}/mcp", kServerPort);
    PrintLanHttpAddresses();
    StartCloudflared();

    ApplicationService service(application, *dr);
    for (;;)
    {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        const int clientFd = accept(serverFd, reinterpret_cast<sockaddr *>(&clientAddr), &clientLen);
        if (clientFd < 0)
        {
            if (errno == EINTR) continue;
            printErrno("接受连接失败");
            continue;
        }

        Config::ActiveThreadPools().client().detach_task([&service, clientFd, clientAddr, healthResponse] { HandleClientConnection(service, clientFd, clientAddr, healthResponse); });
    }
}
