#pragma once
#include <nlohmann/json.hpp>
#include <variant>
#include "runtime.h"
#include "../base/system_log.h"
#include "../memory/signature_scanner.h"

// Shared business operations for native MCP and HTTP RPC.
class ApplicationService
{
    using json = nlohmann::json;
    ApplicationRuntime &application_;
    Driver &driver_;

    // 清理文本中的换行字符
    static std::string sanitizeLine(std::string text)
    {
        for (char &ch : text)
        {
            if (ch == '\n' || ch == '\r')
            {
                ch = ' ';
            }
        }
        return text;
    }

    // 按空白切分命令参数

    template <typename T, typename ParseFn> static std::optional<T> parseNumber(std::string_view text, ParseFn &&parse)
    {
        if (text.empty()) return std::nullopt;

        std::string temp(text);
        char *end = nullptr;
        errno = 0;
        const auto value = parse(temp.c_str(), &end);
        if (errno != 0 || end == temp.c_str() || *end != '\0') return std::nullopt;
        return static_cast<T>(value);
    }

    // 解析整数参数
    static std::optional<int> parseInt(std::string_view text)
    {
        const auto value = parseNumber<std::int64_t>(text, [](const char *s, char **end) { return std::strtoll(s, end, 0); });
        if (!value || *value < std::numeric_limits<int>::min() || *value > std::numeric_limits<int>::max()) return std::nullopt;
        return static_cast<int>(*value);
    }

    static std::optional<std::int64_t> parseInt64(std::string_view text)
    {
        return parseNumber<std::int64_t>(text, [](const char *s, char **end) { return std::strtoll(s, end, 0); });
    }

    // 将字符串转换为小写ASCII
    static std::string toLowerAscii(std::string_view input)
    {
        std::string out;
        out.reserve(input.size());
        for (const unsigned char ch : input)
        {
            out.push_back(static_cast<char>(std::tolower(ch)));
        }
        return out;
    }

    // 解析数据类型标记
    static std::optional<Types::DataType> parseDataTypeToken(std::string_view token)
    {
        const std::string t = toLowerAscii(token);
        if (t == "i8" || t == "int8") return Types::DataType::I8;
        if (t == "i16" || t == "int16") return Types::DataType::I16;
        if (t == "i32" || t == "int32") return Types::DataType::I32;
        if (t == "i64" || t == "int64") return Types::DataType::I64;
        if (t == "f32" || t == "float") return Types::DataType::Float;
        if (t == "f64" || t == "double") return Types::DataType::Double;
        return std::nullopt;
    }

    static std::string_view dataTypeToken(Types::DataType type)
    {
        switch (type)
        {
        case Types::DataType::I8:
            return "i8";
        case Types::DataType::I16:
            return "i16";
        case Types::DataType::I32:
            return "i32";
        case Types::DataType::I64:
            return "i64";
        case Types::DataType::Float:
            return "f32";
        case Types::DataType::Double:
            return "f64";
        default:
            return "";
        }
    }

    static std::optional<Types::SavedValueKind> parseSavedValueKindToken(std::string_view token)
    {
        const std::string t = toLowerAscii(token);
        if (t.empty() || t == "numeric" || t == "number") return Types::SavedValueKind::Numeric;
        if (t == "pointer" || t == "ptr") return Types::SavedValueKind::Pointer;
        if (t == "text" || t == "string") return Types::SavedValueKind::Text;
        return std::nullopt;
    }

    static std::string_view savedValueKindToken(Types::SavedValueKind kind)
    {
        switch (kind)
        {
        case Types::SavedValueKind::Numeric:
            return "numeric";
        case Types::SavedValueKind::Pointer:
            return "pointer";
        case Types::SavedValueKind::Text:
            return "text";
        default:
            return "";
        }
    }

    static std::optional<bool> parseBoolToken(std::string_view token)
    {
        const std::string t = toLowerAscii(token);
        if (t == "true" || t == "1" || t == "yes" || t == "on") return true;
        if (t == "false" || t == "0" || t == "no" || t == "off") return false;
        return std::nullopt;
    }

    // 解析扫描模式标记
    static std::optional<Types::FuzzyMode> parseFuzzyModeToken(std::string_view token)
    {
        const std::string t = toLowerAscii(token);
        if (t == "unknown") return Types::FuzzyMode::Unknown;
        if (t == "eq" || t == "equal") return Types::FuzzyMode::Equal;
        if (t == "gt" || t == "greater") return Types::FuzzyMode::Greater;
        if (t == "lt" || t == "less") return Types::FuzzyMode::Less;
        if (t == "inc" || t == "increased") return Types::FuzzyMode::Increased;
        if (t == "dec" || t == "decreased") return Types::FuzzyMode::Decreased;
        if (t == "chg" || t == "changed") return Types::FuzzyMode::Changed;
        if (t == "unchg" || t == "unchanged") return Types::FuzzyMode::Unchanged;
        if (t == "range") return Types::FuzzyMode::Range;
        if (t == "ptr" || t == "pointer") return Types::FuzzyMode::Pointer;
        if (t == "str" || t == "string") return Types::FuzzyMode::String;
        return std::nullopt;
    }

    static std::string_view fuzzyModeToken(Types::FuzzyMode mode)
    {
        switch (mode)
        {
        case Types::FuzzyMode::Unknown:
            return "unknown";
        case Types::FuzzyMode::Equal:
            return "equal";
        case Types::FuzzyMode::Greater:
            return "greater";
        case Types::FuzzyMode::Less:
            return "less";
        case Types::FuzzyMode::Increased:
            return "increased";
        case Types::FuzzyMode::Decreased:
            return "decreased";
        case Types::FuzzyMode::Changed:
            return "changed";
        case Types::FuzzyMode::Unchanged:
            return "unchanged";
        case Types::FuzzyMode::Range:
            return "range";
        case Types::FuzzyMode::Pointer:
            return "pointer";
        case Types::FuzzyMode::String:
            return "string";
        default:
            return "";
        }
    }

    // 解析内存浏览显示格式
    static std::optional<Types::ViewFormat> parseViewFormatToken(std::string_view token)
    {
        const std::string t = toLowerAscii(token);
        if (t == "hex") return Types::ViewFormat::Hex;
        if (t == "hexadecimal") return Types::ViewFormat::Hexadecimal;
        if (t == "i8" || t == "int8") return Types::ViewFormat::I8;
        if (t == "i16" || t == "int16") return Types::ViewFormat::I16;
        if (t == "i32" || t == "int32") return Types::ViewFormat::I32;
        if (t == "i64" || t == "int64") return Types::ViewFormat::I64;
        if (t == "f32" || t == "float") return Types::ViewFormat::Float;
        if (t == "f64" || t == "double") return Types::ViewFormat::Double;
        if (t == "disasm") return Types::ViewFormat::Disasm;
        return std::nullopt;
    }

    // 解析硬件断点类型
    static std::optional<Driver::bp_type> parseBpTypeToken(std::string_view token)
    {
        const std::string t = toLowerAscii(token);
        if (t == "1" || t == "read" || t == "r" || t == "bp_read") return Driver::BP_BREAKPOINT_R;
        if (t == "2" || t == "write" || t == "w" || t == "bp_write") return Driver::BP_BREAKPOINT_W;
        if (t == "3" || t == "read_write" || t == "rw" || t == "bp_read_write") return Driver::BP_BREAKPOINT_RW;
        if (t == "4" || t == "execute" || t == "x" || t == "exec" || t == "bp_execute") return Driver::BP_BREAKPOINT_X;
        return std::nullopt;
    }

    // 解析硬件断点作用线程范围
    static std::optional<Driver::bp_scope> parseBpScopeToken(std::string_view token)
    {
        const std::string t = toLowerAscii(token);
        if (t == "0" || t == "main" || t == "main_thread") return Driver::BP_SCOPE_MAIN_THREAD;
        if (t == "1" || t == "other" || t == "other_threads") return Driver::BP_SCOPE_OTHER_THREADS;
        if (t == "2" || t == "all" || t == "all_threads") return Driver::BP_SCOPE_ALL_THREADS;
        return std::nullopt;
    }

    static std::optional<Driver::bp_len> parseBpLengthValue(int length)
    {
        switch (length)
        {
        case 1:
            return Driver::BP_BREAKPOINT_LEN_1;
        case 2:
            return Driver::BP_BREAKPOINT_LEN_2;
        case 3:
            return Driver::BP_BREAKPOINT_LEN_3;
        case 4:
            return Driver::BP_BREAKPOINT_LEN_4;
        case 5:
            return Driver::BP_BREAKPOINT_LEN_5;
        case 6:
            return Driver::BP_BREAKPOINT_LEN_6;
        case 7:
            return Driver::BP_BREAKPOINT_LEN_7;
        case 8:
            return Driver::BP_BREAKPOINT_LEN_8;
        default:
            return std::nullopt;
        }
    }

    // 将显示格式枚举转换为标记
    static std::string_view viewFormatToToken(Types::ViewFormat format)
    {
        switch (format)
        {
        case Types::ViewFormat::Hex:
            return "hex";
        case Types::ViewFormat::Hexadecimal:
            return "hexadecimal";
        case Types::ViewFormat::I8:
            return "i8";
        case Types::ViewFormat::I16:
            return "i16";
        case Types::ViewFormat::I32:
            return "i32";
        case Types::ViewFormat::I64:
            return "i64";
        case Types::ViewFormat::Float:
            return "f32";
        case Types::ViewFormat::Double:
            return "f64";
        case Types::ViewFormat::Disasm:
            return "disasm";
        default:
            return "hexadecimal";
        }
    }

    // 将硬件断点类型转换为文本标记
    static std::string_view bpTypeToToken(Driver::bp_type type)
    {
        switch (type)
        {
        case Driver::BP_BREAKPOINT_R:
            return "read";
        case Driver::BP_BREAKPOINT_W:
            return "write";
        case Driver::BP_BREAKPOINT_RW:
            return "read_write";
        case Driver::BP_BREAKPOINT_X:
            return "execute";
        default:
            return "unknown";
        }
    }

    // 将硬件断点线程范围转换为文本标记
    static std::string_view bpScopeToToken(Driver::bp_scope scope)
    {
        switch (scope)
        {
        case Driver::BP_SCOPE_MAIN_THREAD:
            return "main";
        case Driver::BP_SCOPE_OTHER_THREADS:
            return "other";
        case Driver::BP_SCOPE_ALL_THREADS:
            return "all";
        default:
            return "unknown";
        }
    }

    // 将字节数组编码为十六进制字符串
    static std::string bytesToHex(const std::uint8_t *bytes, std::size_t count)
    {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string output;
        output.resize(count * 2);
        for (std::size_t i = 0; i < count; ++i)
        {
            output[i * 2] = digits[bytes[i] >> 4];
            output[i * 2 + 1] = digits[bytes[i] & 0x0F];
        }
        return output;
    }

    // 解析十六进制字节流
    static std::optional<std::vector<std::uint8_t>> parseHexBytes(std::string_view text)
    {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(text.size() / 2);
        int high = -1;
        for (const unsigned char ch : text)
        {
            if (std::isspace(ch) != 0) continue;
            if (std::isxdigit(ch) == 0) return std::nullopt;
            const int digit = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch - 'A' + 10;
            if (high < 0) high = digit;
            else
            {
                bytes.push_back(static_cast<std::uint8_t>((high << 4) | digit));
                high = -1;
            }
        }
        if (high >= 0 || bytes.empty()) return std::nullopt;
        return bytes;
    }

    // 合并指定起点后的参数为字符串

    // 生成成功响应文本

    // 生成失败响应文本

    // 构建内存信息JSON响应
    static json buildMemoryInfoJson(int status, const auto &info)
    {
        json root;
        const int moduleCount = std::clamp(info.module_count, 0, MAX_MODULES);
        const int regionCount = std::clamp(info.region_count, 0, MAX_SCAN_REGIONS);

        root["status"] = status;
        root["module_count"] = moduleCount;
        root["region_count"] = regionCount;
        root["modules"] = json::array();
        root["regions"] = json::array();

        for (int i = 0; i < moduleCount; ++i)
        {
            const auto &mod = info.modules[i];
            const int segCount = std::clamp(mod.seg_count, 0, MAX_SEGS_PER_MODULE);

            json moduleItem;
            moduleItem["name"] = std::string(mod.name);
            moduleItem["seg_count"] = segCount;
            moduleItem["segs"] = json::array();

            for (int j = 0; j < segCount; ++j)
            {
                const auto &seg = mod.segs[j];
                moduleItem["segs"].push_back({
                    {"index", seg.index},
                    {"prot", static_cast<int>(seg.prot)},
                    {"start", std::format("0x{:X}", seg.start)},
                    {"end", std::format("0x{:X}", seg.end)},
                });
            }

            root["modules"].push_back(std::move(moduleItem));
        }

        for (int i = 0; i < regionCount; ++i)
        {
            const auto &region = info.regions[i];
            root["regions"].push_back({
                {"start", std::format("0x{:X}", region.start)},
                {"end", std::format("0x{:X}", region.end)},
            });
        }

        return root;
    }

    // 构建内存浏览快照JSON
    static json buildViewerSnapshotJson(const MemViewer &viewer)
    {
        json root;
        root["read_success"] = viewer.readSuccess();
        root["read_status"] = viewer.readStatus();
        root["base"] = std::format("0x{:X}", static_cast<std::uint64_t>(viewer.base()));
        const auto format = viewer.format();
        root["format"] = viewFormatToToken(format);

        const auto &buffer = viewer.buffer();
        root["byte_count"] = buffer.size();
        root["data_hex"] = bytesToHex(buffer.data(), buffer.size());

        if (format == Types::ViewFormat::Disasm)
        {
            root["disasm"] = json::array();
            std::size_t emittedLines = 0;
            for (const auto &line : viewer.getDisasm())
            {
                if (emittedLines >= buffer.size() / 4) break;
                json item;
                item["valid"] = line.valid;
                item["address"] = std::format("0x{:X}", line.address);
                item["size"] = line.size;
                item["bytes_hex"] = bytesToHex(line.bytes, std::min(line.size, sizeof(line.bytes)));
                item["mnemonic"] = sanitizeLine(line.mnemonic);
                item["op_str"] = sanitizeLine(line.op_str);
                root["disasm"].push_back(std::move(item));
                ++emittedLines;
            }
        }

        return root;
    }

    static json buildBreakpointDataJson(auto &info)
    {
        json data = {{"num_brps", info.num_brps}, {"num_wrps", info.num_wrps}, {"points", json::array()}};

        for (auto &point : info.points)
        {
            const int pointRecordCount = std::clamp(point.record_count, 0, BP_RECORD_MAX);
            json pointItem = {{"bt", static_cast<int>(point.bt)}, {"bl", static_cast<int>(point.bl)}, {"bs", static_cast<int>(point.bs)}, {"hit_addr", std::format("0x{:X}", point.hit_addr)}, {"record_count", pointRecordCount}, {"records", json::array()}};

            for (int i = 0; i < pointRecordCount; ++i)
            {
                auto &rec = point.records[i];
                MemUtils::HwbpRequestAll(rec);
                json item = {{"mask", json::array()}, {"hit_count", static_cast<std::uint64_t>(MemUtils::HwbpReadRegisterValue(rec, Driver::IDX_HIT_COUNT))}, {"pc", std::format("0x{:X}", static_cast<std::uint64_t>(MemUtils::HwbpReadRegisterValue(rec, Driver::IDX_PC)))}, {"lr", std::format("0x{:X}", static_cast<std::uint64_t>(MemUtils::HwbpReadRegisterValue(rec, Driver::IDX_LR)))}, {"sp", std::format("0x{:X}", static_cast<std::uint64_t>(MemUtils::HwbpReadRegisterValue(rec, Driver::IDX_SP)))}, {"orig_x0", std::format("0x{:X}", static_cast<std::uint64_t>(MemUtils::HwbpReadRegisterValue(rec, Driver::IDX_ORIG_X0)))}, {"syscallno", std::format("0x{:X}", static_cast<std::uint32_t>(MemUtils::HwbpReadRegisterValue(rec, Driver::IDX_SYSCALLNO)))}, {"pstate", std::format("0x{:X}", static_cast<std::uint64_t>(MemUtils::HwbpReadRegisterValue(rec, Driver::IDX_PSTATE)))}, {"fpsr", std::format("0x{:X}", static_cast<std::uint32_t>(MemUtils::HwbpReadRegisterValue(rec, Driver::IDX_FPSR)))}, {"fpcr", std::format("0x{:X}", static_cast<std::uint32_t>(MemUtils::HwbpReadRegisterValue(rec, Driver::IDX_FPCR)))}};
                for (int m = 0; m < 18; ++m) item["mask"].push_back(std::format("0x{:X}", rec.mask[m]));
                for (int reg = 0; reg < 30; ++reg) item[std::format("x{}", reg)] = std::format("0x{:X}", static_cast<std::uint64_t>(MemUtils::HwbpReadRegisterValue(rec, Driver::IDX_X0 + reg)));
                for (int reg = 0; reg < 32; ++reg)
                {
                    const auto qreg = MemUtils::HwbpReadRegisterValue(rec, Driver::IDX_Q0 + reg);
                    item[std::format("q{}", reg)] = {{"lo", std::format("0x{:X}", static_cast<std::uint64_t>(qreg))}, {"hi", std::format("0x{:X}", static_cast<std::uint64_t>(qreg >> 64))}};
                }
                pointItem["records"].push_back(std::move(item));
            }
            data["points"].push_back(std::move(pointItem));
        }
        return data;
    }

    // 构建特征码扫描结果JSON
    static json buildSignatureMatchesJson(const std::vector<uintptr_t> &matches, std::int64_t range, std::string_view pattern)
    {
        constexpr std::size_t kMaxReturnedMatches = 4096;
        const std::size_t returnedCount = std::min(matches.size(), kMaxReturnedMatches);

        json root;
        root["count"] = matches.size();
        root["returned_count"] = returnedCount;
        root["truncated"] = (matches.size() > returnedCount);
        root["range"] = range;
        root["pattern"] = std::string(pattern);
        root["matches"] = json::array();

        for (std::size_t i = 0; i < returnedCount; ++i)
        {
            const auto addr = static_cast<std::uint64_t>(matches[i]);
            root["matches"].push_back({
                {"addr", std::format("0x{:X}", addr)},
            });
        }

        return root;
    }

    // 发送完整响应数据
    static json makeProtocolError(std::string_view message)
    {
        return json{
            {"ok", false},
            {"error", std::string(message)},
        };
    }

    static std::optional<std::string> getRequiredStringParam(const json &params, std::string_view key)
    {
        const auto it = params.find(std::string(key));
        if (it == params.end())
        {
            return std::nullopt;
        }

        if (it->is_string())
        {
            return it->get<std::string>();
        }

        if (it->is_boolean() || it->is_number_integer() || it->is_number_unsigned() || it->is_number_float())
        {
            return it->dump();
        }

        return std::nullopt;
    }

public:
    ApplicationService(ApplicationRuntime &application, Driver &driver) : application_(application), driver_(driver) {}

    json dispatch(std::string_view operation, const json &params) try
    {
        auto &runtime = application_.resources();
        const std::string op(operation);

        auto finalize = [&](json out) -> json
        {
            out["operation"] = op;
            return out;
        };

        auto fail = [&](std::string_view message) -> json { return finalize(makeProtocolError(message)); };

        auto ok = [&]() -> json { return finalize(json{{"ok", true}}); };

        auto okData = [&](json data) -> json
        {
            json out = ok();
            out["data"] = std::move(data);
            return out;
        };

        using BreakpointPointsResult = std::variant<std::vector<Driver::bp_point>, json>;
        auto parseBreakpointPoints = [&]() -> BreakpointPointsResult
        {
            const auto pointsIt = params.find("points");
            if (pointsIt == params.end() || !pointsIt->is_array() || pointsIt->empty()) return BreakpointPointsResult{std::in_place_index<1>, fail(std::format("operation={} 缺少参数 points", op))};

            auto parseUInt64Value = [](const json &value) -> std::optional<std::uint64_t>
            {
                if (value.is_number_unsigned()) return value.get<std::uint64_t>();
                if (value.is_number_integer())
                {
                    const auto signedValue = value.get<std::int64_t>();
                    return signedValue >= 0 ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(signedValue)) : std::nullopt;
                }
                if (value.is_string()) return MemUtils::ParseUInt64(value.get<std::string>());
                return std::nullopt;
            };
            auto parseIntValue = [](const json &value) -> std::optional<int>
            {
                if (value.is_number_unsigned())
                {
                    const auto raw = value.get<std::uint64_t>();
                    return raw <= static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ? std::optional<int>(static_cast<int>(raw)) : std::nullopt;
                }
                if (value.is_number_integer())
                {
                    const auto raw = value.get<std::int64_t>();
                    return raw >= std::numeric_limits<int>::min() && raw <= std::numeric_limits<int>::max() ? std::optional<int>(static_cast<int>(raw)) : std::nullopt;
                }
                if (value.is_string()) return parseInt(value.get<std::string>());
                return std::nullopt;
            };
            auto parseStringValue = [](const json &value) -> std::optional<std::string>
            {
                if (value.is_string()) return value.get<std::string>();
                if (value.is_number_integer()) return std::format("{}", value.get<std::int64_t>());
                if (value.is_number_unsigned()) return std::format("{}", value.get<std::uint64_t>());
                return std::nullopt;
            };

            std::vector<Driver::bp_point> points;
            if (pointsIt->size() > BP_CONFIG_MAX) return BreakpointPointsResult{std::in_place_index<1>, fail(std::format("points 最多 {} 个", BP_CONFIG_MAX))};
            points.reserve(pointsIt->size());
            for (const auto &pointJson : *pointsIt)
            {
                if (!pointJson.is_object()) return BreakpointPointsResult{std::in_place_index<1>, fail("points 中存在无效断点配置")};
                const auto addressIt = pointJson.find("address");
                const auto typeIt = pointJson.find("bp_type");
                const auto scopeIt = pointJson.find("bp_scope");
                const auto lengthIt = pointJson.find("length");
                if (addressIt == pointJson.end() || typeIt == pointJson.end() || scopeIt == pointJson.end() || lengthIt == pointJson.end()) return BreakpointPointsResult{std::in_place_index<1>, fail("points 中每个断点都需要 address/bp_type/bp_scope/length")};

                const auto address = parseUInt64Value(*addressIt);
                const auto typeToken = parseStringValue(*typeIt);
                const auto scopeToken = parseStringValue(*scopeIt);
                const auto lengthValue = parseIntValue(*lengthIt);
                if (!address || *address == 0 || !typeToken || !scopeToken || !lengthValue) return BreakpointPointsResult{std::in_place_index<1>, fail("points 中存在无效断点参数")};

                const auto bpType = parseBpTypeToken(*typeToken);
                const auto bpScope = parseBpScopeToken(*scopeToken);
                if (*lengthValue < 1 || *lengthValue > 8) return BreakpointPointsResult{std::in_place_index<1>, fail("points 中存在无效断点参数，长度范围为 1-8")};
                const auto bpLength = parseBpLengthValue(*lengthValue);
                if (!bpType || !bpScope || !bpLength) return BreakpointPointsResult{std::in_place_index<1>, fail("points 中存在无效断点参数，长度范围为 1-8")};

                Driver::bp_point point{};
                point.hit_addr = *address;
                point.bt = *bpType;
                point.bl = *bpLength;
                point.bs = *bpScope;
                points.push_back(point);
            }
            return BreakpointPointsResult{std::in_place_index<0>, std::move(points)};
        };

        auto breakpointSetResult = [&](int status, std::string_view mode, const std::vector<Driver::bp_point> &points) -> json
        {
            runtime.hwbpMode = mode;

            json outPoints = json::array();
            for (std::size_t index = 0; index < points.size(); ++index)
            {
                outPoints.push_back({{"index", index}, {"address", std::format("0x{:X}", points[index].hit_addr)}, {"type", std::string(bpTypeToToken(points[index].bt))}, {"scope", std::string(bpScopeToToken(points[index].bs))}, {"length", static_cast<int>(points[index].bl)}});
            }
            return okData({{"status", status}, {"mode", mode}, {"point_count", points.size()}, {"points", std::move(outPoints)}});
        };

        auto requiredString = [&](std::string_view key, std::string_view desc) -> std::variant<std::string, json>
        {
            const auto value = getRequiredStringParam(params, key);
            if (!value.has_value() || value->empty()) return std::variant<std::string, json>{std::in_place_index<1>, fail(std::format("operation={} 缺少参数 {}", op, desc))};
            return std::variant<std::string, json>{std::in_place_index<0>, *value};
        };

        auto optionalString = [&](std::string_view key) -> std::string
        {
            const auto value = getRequiredStringParam(params, key);
            return value.has_value() ? *value : "";
        };

        auto requiredParsed = [&]<typename T>(std::string_view key, std::string_view desc, auto &&parse) -> std::variant<T, json>
        {
            const auto text = requiredString(key, desc);
            if (std::holds_alternative<json>(text)) return std::variant<T, json>{std::in_place_index<1>, std::get<json>(text)};
            const auto parsed = parse(std::get<std::string>(text));
            if (!parsed.has_value()) return std::variant<T, json>{std::in_place_index<1>, fail(std::format("operation={} 参数 {} 无效", op, desc))};
            return std::variant<T, json>{std::in_place_index<0>, *parsed};
        };

        auto requiredUInt64 = [&](std::string_view key, std::string_view desc) -> std::variant<std::uint64_t, json> { return requiredParsed.template operator()<std::uint64_t>(key, desc, [](std::string_view text) { return MemUtils::ParseUInt64(text); }); };

        auto requiredInt = [&](std::string_view key, std::string_view desc) -> std::variant<int, json> { return requiredParsed.template operator()<int>(key, desc, parseInt); };

        auto requiredInt64 = [&](std::string_view key, std::string_view desc) -> std::variant<std::int64_t, json> { return requiredParsed.template operator()<std::int64_t>(key, desc, parseInt64); };

        auto scannerStateJson = [&]() -> json
        {
            const auto state = runtime.scanner.state();
            return {
                {"scanning", state.scanning}, {"progress", state.progress}, {"count", state.count}, {"string_scan", state.stringScan}, {"value_type", state.stringScan ? "string" : state.dataType.has_value() ? Types::Labels::TYPE[static_cast<size_t>(*state.dataType)] : ""}, {"mode", fuzzyModeToken(state.mode)}, {"alignment", state.alignment}, {"value_format", state.mode == Types::FuzzyMode::Pointer ? "hex_address" : state.mode == Types::FuzzyMode::String ? "text" : "number"},
            };
        };

        auto pointerStateJson = [&]() -> json
        {
            const auto state = runtime.pointer.state();
            return {
                {"busy", state.operation != PointerManager::Operation::Idle}, {"scanning", state.operation == PointerManager::Operation::Scanning}, {"operation", PointerManager::OperationName(state.operation)}, {"last_operation", PointerManager::OperationName(state.lastOperation)}, {"progress", state.progress}, {"count", state.count}, {"completed", state.completed}, {"success", state.success}, {"error", state.error},
            };
        };

        auto savedStatesJson = [&]() -> json
        {
            json items = json::array();
            for (const auto &state : runtime.saved.snapshotStates())
            {
                items.push_back({
                    {"address", std::format("0x{:X}", state.item.address)},
                    {"value_type", std::string(dataTypeToken(state.item.type))},
                    {"value_type_label", Types::Labels::TYPE[static_cast<size_t>(state.item.type)]},
                    {"value_kind", std::string(savedValueKindToken(state.item.kind))},
                    {"text_length", state.item.textLength},
                    {"note", state.item.note},
                    {"value", state.value},
                    {"locked", state.locked},
                    {"lock_value", state.lockValue},
                });
            }
            return json{{"count", items.size()}, {"items", std::move(items)}};
        };

        std::lock_guard<std::mutex> requestLock(runtime.requestMutex);

        if (op == "bridge.ping")
        {
            auto data = runtime.debug.capabilities();
            data["target_operations"] = true;
            return okData(std::move(data));
        }
        if (op == "bridge.capabilities") return okData(runtime.debug.capabilities());
        if (op.starts_with("trace."))
        {
            if (op == "trace.configure") return okData(runtime.debug.configure(params.at("directory").get<std::string>()));
            if (op == "trace.status") return okData(runtime.debug.status());
            if (op == "trace.list") return okData(runtime.debug.list());
            if (op == "trace.stop") { runtime.debug.stop(); return okData(runtime.debug.status()); }
            if (op == "trace.read")
            {
                const auto after = requiredUInt64("after", "after");
                const auto count = requiredInt("count", "count");
                if (std::holds_alternative<json>(after)) return std::get<json>(after);
                if (std::holds_alternative<json>(count)) return std::get<json>(count);
                const int limit = std::get<int>(count);
                if (limit < 1 || limit > 500) return fail("count 范围 1-500");
                return okData(runtime.debug.read(params.at("session_id").get<std::string>(), std::get<uint64_t>(after), limit));
            }
            if (op == "trace.start")
            {
                std::lock_guard targetLock(Config::TargetMutex);
                if (!runtime.hwbpMode.empty() || runtime.scanner.isScanning() || runtime.pointer.isBusy())
                    return fail("目标有活动断点或扫描，请先停止");
                const auto pc = requiredUInt64("start_pc", "start_pc");
                const auto steps = requiredUInt64("max_steps", "max_steps");
                const auto duration = requiredUInt64("duration_ms", "duration_ms");
                if (std::holds_alternative<json>(pc)) return std::get<json>(pc);
                if (std::holds_alternative<json>(steps)) return std::get<json>(steps);
                if (std::holds_alternative<json>(duration)) return std::get<json>(duration);
                if (driver_.GetGlobalPid() <= 0) return fail("先选择目标进程");
                return okData(runtime.debug.start(driver_.GetGlobalPid(), std::get<uint64_t>(pc), std::get<uint64_t>(steps), std::get<uint64_t>(duration)));
            }
            return fail("未知 trace operation");
        }
        if (op == "breakpoint.backtrace")
        {
            if (runtime.debug.busy()) return fail("Trace 正在独占事件流");
            const auto stream = requiredUInt64("stream", "stream");
            const auto sequence = requiredUInt64("event_seq", "event_seq");
            if (std::holds_alternative<json>(stream)) return std::get<json>(stream);
            if (std::holds_alternative<json>(sequence)) return std::get<json>(sequence);
            const uint64_t seq = std::get<uint64_t>(sequence);
            if (!seq || !std::get<uint64_t>(stream)) return fail("stream 和 event_seq 必须大于零");
            ls_debug_message message{}; message.command = LS_DEBUG_READ;
            message.stream = std::get<uint64_t>(stream); message.after = seq - 1; message.count = 1;
            const int status = driver_.DebugCall(message);
            if (status) return fail("回溯请求失败 status=" + std::to_string(status));
            if (!message.count || message.events[0].seq != seq) return fail("命中事件不存在或已被覆盖");
            return okData({{"stream", std::to_string(message.stream)}, {"event", lsdebug::EventJson(message.events[0])}});
        }
        if (op.starts_with("breakpoint.events."))
        {
            if (runtime.debug.busy()) return fail("Trace 正在独占事件流");
            ls_debug_message message{};
            uint64_t cursor = 0;
            if (op == "breakpoint.events.start") { message.command = LS_DEBUG_EVENTS_START; message.tgid = driver_.GetGlobalPid(); }
            else if (op == "breakpoint.events.stop") message.command = LS_DEBUG_EVENTS_STOP;
            else if (op == "breakpoint.events.read" || op == "breakpoint.events.status")
            {
                const auto stream = requiredUInt64("stream", "stream");
                if (std::holds_alternative<json>(stream)) return std::get<json>(stream);
                message.stream = std::get<uint64_t>(stream);
                if (!message.stream) return fail("stream 必须为有效的非零会话 ID");
                message.command = LS_DEBUG_STATUS;
                if (op == "breakpoint.events.read")
                {
                    const auto after = requiredUInt64("after", "after");
                    const auto count = requiredInt("count", "count");
                    if (std::holds_alternative<json>(after)) return std::get<json>(after);
                    if (std::holds_alternative<json>(count)) return std::get<json>(count);
                    const int limit = std::get<int>(count);
                    if (limit < 1 || limit > LS_DEBUG_BATCH) return fail("count 范围 1-8");
                    cursor = message.after = std::get<uint64_t>(after);
                    message.count = limit; message.command = LS_DEBUG_READ;
                }
            }
            else return fail("未知 breakpoint.events operation");
            const int status = driver_.DebugCall(message);
            if (status) return fail("事件请求失败 status=" + std::to_string(status));
            if (message.command == LS_DEBUG_EVENTS_START || message.command == LS_DEBUG_EVENTS_STOP)
            { message.command = LS_DEBUG_STATUS; message.count = 0; const int s = driver_.DebugCall(message); if (s) return fail("事件状态失败 status=" + std::to_string(s)); }
            return okData(lsdebug::MessageJson(message, cursor));
        }

        if (op == "target.find")
        {
            const auto package = requiredString("package_name", "package_name");
            if (std::holds_alternative<json>(package)) return std::get<json>(package);
            const int pid = driver_.GetPid(std::get<std::string>(package));
            if (pid <= 0) return fail("未找到进程");
            return okData({{"pid", pid}});
        }

        if (op == "target.select")
        {
            const auto pid = requiredInt("pid", "pid");
            if (std::holds_alternative<json>(pid)) return std::get<json>(pid);
            if (std::get<int>(pid) <= 0) return fail("pid 参数无效");
            if (!runtime.selectTarget(std::get<int>(pid))) return fail("扫描任务运行中或旧监听停止失败，无法切换 PID");
            return okData({{"pid", driver_.GetGlobalPid()}});
        }

        if (op == "target.get") return okData({{"pid", driver_.GetGlobalPid()}});

        if (op == "syscall.start" || op == "syscall.stop")
        {
            const int pid = driver_.GetGlobalPid();
            if (pid <= 0) return fail("全局PID未设置，请先执行 target.select 或 target.attach");
            const int status = op == "syscall.start" ? runtime.startSyscallMonitor() : runtime.stopSyscallMonitor();
            if (status != 0) return fail(std::format("系统调用监听请求失败，状态={}", status));
            return okData({{"pid", pid}, {"active", op == "syscall.start"}, {"status", status}});
        }

        if (op == "syscall.read")
        {
            int status = 0;
            const std::string log = SyscallLog::ReadDmesg("sysmon", &status);
            if (status < 0) return fail(std::format("读取驱动监控日志失败 status={}，请使用配套新版内核", status));
            return okData({{"log", log}, {"line_count", std::ranges::count(log, '\n')}});
        }

        if (op == "cntvct.start" || op == "cntvct.stop")
        {
            const int pid = driver_.GetGlobalPid();
            if (pid <= 0) return fail("全局PID未设置，请先执行 target.select 或 target.attach");
            const int status = op == "cntvct.start" ? runtime.startCntvctMonitor() : runtime.stopCntvctMonitor();
            if (status != 0) return fail(std::format("CNTVCT 监听请求失败，状态={}", status));
            return okData({{"pid", pid}, {"active", op == "cntvct.start"}, {"status", status}});
        }

        if (op == "cntvct.read")
        {
            int status = 0;
            const std::string log = SyscallLog::ReadDmesg("cntvct", &status);
            if (status < 0) return fail(std::format("读取驱动监控日志失败 status={}，请使用配套新版内核", status));
            return okData({{"log", log}, {"line_count", std::ranges::count(log, '\n')}});
        }

        if (op == "target.attach")
        {
            const auto package = requiredString("package_name", "package_name");
            if (std::holds_alternative<json>(package)) return std::get<json>(package);
            const int pid = driver_.GetPid(std::get<std::string>(package));
            if (pid <= 0) return fail("未找到进程");
            if (!runtime.selectTarget(pid)) return fail("扫描任务运行中或旧监听停止失败，无法切换 PID");
            return okData({{"pid", pid}});
        }

        if (op == "env.read")
        {
            const std::string threadName = optionalString("thread_name");

            const int pid = driver_.GetGlobalPid();
            if (pid <= 0) return fail("全局PID未设置，请先执行 target.select 或 target.attach");

            const auto info = driver_.GetEnvParams(threadName);
            if (info.tls_status != 0 && info.pacga_status != 0) return fail("获取环境参数失败");

            return okData({
                {"pid", pid},
                {"thread_name", threadName},
                {"tpidr_el0", std::format("0x{:X}", info.tpidr_el0)},
                {"pacga_lo", std::format("0x{:X}", info.pacga_lo)},
                {"pacga_hi", std::format("0x{:X}", info.pacga_hi)},
                {"tls_status", info.tls_status},
                {"pacga_status", info.pacga_status},
            });
        }

        if (op == "memory.map")
        {
            const std::unique_ptr<Driver::virtual_memory> info(new Driver::virtual_memory(driver_.GetMemoryInfo()));
            return okData(buildMemoryInfoJson(0, *info));
        }

        if (op == "module.resolve")
        {
            const auto moduleName = requiredString("module_name", "module_name");
            const auto segmentIndex = requiredInt("segment_index", "segment_index");
            const auto which = requiredString("which", "which");
            if (std::holds_alternative<json>(moduleName)) return std::get<json>(moduleName);
            if (std::holds_alternative<json>(segmentIndex)) return std::get<json>(segmentIndex);
            if (std::holds_alternative<json>(which)) return std::get<json>(which);

            const std::string whichValue = toLowerAscii(std::get<std::string>(which));
            if (whichValue != "start" && whichValue != "end") return fail("which 必须是 start 或 end");

            std::uint64_t address = 0;
            if (!driver_.GetModuleAddress(std::get<std::string>(moduleName), static_cast<short>(std::get<int>(segmentIndex)), &address, whichValue == "start")) return fail("未找到目标模块或段");
            return okData({{"address", std::format("0x{:X}", address)}});
        }

        if (op == "memory.dump")
        {
            const auto target = requiredString("target", "target");
            if (std::holds_alternative<json>(target)) return std::get<json>(target);
            if (driver_.GetGlobalPid() <= 0) return fail("全局PID未设置，请先执行 target.select 或 target.attach");
            std::string path;
            if (!driver_.DumpMemory(std::get<std::string>(target), &path)) return fail("内存 Dump 失败，请检查模块名或地址范围");
            return okData({{"target", std::get<std::string>(target)}, {"path", path}});
        }

        if (op == "scan.start" || op == "scan.refine")
        {
            const auto mode = requiredString("mode", "mode");
            if (std::holds_alternative<json>(mode)) return std::get<json>(mode);

            const auto fuzzyMode = parseFuzzyModeToken(std::get<std::string>(mode));
            if (!fuzzyMode.has_value()) return fail("mode 无效，支持: unknown/eq/gt/lt/inc/dec/changed/unchanged/range/pointer/string");

            const int pid = driver_.GetGlobalPid();
            if (pid <= 0) return fail("全局PID未设置，请先执行 target.select 或 target.attach");

            const bool isFirst = (op == "scan.start");
            size_t alignment = 0;
            if (params.contains("alignment"))
            {
                const auto parsedAlignment = parseInt(optionalString("alignment"));
                if (!parsedAlignment || (*parsedAlignment != 0 && *parsedAlignment != 1 && *parsedAlignment != 2 && *parsedAlignment != 4 && *parsedAlignment != 8)) return fail("alignment 必须是 0/1/2/4/8");
                alignment = static_cast<size_t>(*parsedAlignment);
            }
            MemUtils::FloatRounding rounding = MemUtils::FloatRounding::Rounded;
            const auto roundingToken = toLowerAscii(optionalString("rounding"));
            if (roundingToken == "exact") rounding = MemUtils::FloatRounding::Exact;
            else if (roundingToken == "truncated") rounding = MemUtils::FloatRounding::Truncated;
            else if (roundingToken == "extreme") rounding = MemUtils::FloatRounding::Extreme;
            else if (!roundingToken.empty() && roundingToken != "rounded") return fail("rounding 无效，支持 rounded/truncated/extreme/exact");
            const std::string valueToken = optionalString("value");
            if (*fuzzyMode == Types::FuzzyMode::String)
            {
                if (valueToken.empty()) return fail("string 模式需要 value 参数");
                if (!runtime.scanner.startStringAsync(pid, valueToken, isFirst)) return fail("扫描请求被拒绝，请检查任务状态和结果类型");
                return okData(scannerStateJson());
            }

            const auto type = requiredString("value_type", "value_type");
            if (std::holds_alternative<json>(type)) return std::get<json>(type);
            const auto dataType = parseDataTypeToken(std::get<std::string>(type));
            if (!dataType.has_value()) return fail("value_type 无效，支持: i8/i16/i32/i64/f32/f64");
            if (*fuzzyMode == Types::FuzzyMode::Pointer && *dataType != Types::DataType::I64) return fail("pointer 模式只支持 i64");

            if (*fuzzyMode == Types::FuzzyMode::Pointer)
            {
                if (valueToken.empty()) return fail("pointer 模式需要 value 参数");
                const auto parsed = MemUtils::ParseUInt64(valueToken, 16);
                if (!parsed) return fail("value 参数不是有效的十六进制地址");
                const auto target = static_cast<int64_t>(MemUtils::Normalize(static_cast<uintptr_t>(*parsed)));
                if (!runtime.scanner.startAsync<int64_t>(pid, target, Types::DataType::I64, *fuzzyMode, isFirst, int64_t{}, {}, rounding, alignment)) return fail("扫描请求被拒绝，请检查任务状态、数据类型和扫描模式");
                return okData(scannerStateJson());
            }

            const bool needValue = *fuzzyMode == Types::FuzzyMode::Equal || *fuzzyMode == Types::FuzzyMode::Greater || *fuzzyMode == Types::FuzzyMode::Less || *fuzzyMode == Types::FuzzyMode::Range;
            if (needValue && valueToken.empty()) return fail("当前模式需要 value 参数");
            const std::string rangeToken = optionalString("range_max");
            if (*fuzzyMode == Types::FuzzyMode::Range && rangeToken.empty()) return fail("range 模式需要 range_max 参数");

            return MemUtils::DispatchType(*dataType,
                                          [&]<typename T>() -> json
                                          {
                                              if (*fuzzyMode == Types::FuzzyMode::Unknown || (*fuzzyMode >= Types::FuzzyMode::Increased && *fuzzyMode <= Types::FuzzyMode::Unchanged))
                                              {
                                                  if (!runtime.scanner.startAsync<T>(pid, T{}, *dataType, *fuzzyMode, isFirst, T{}, {}, rounding, alignment)) return fail("扫描请求被拒绝，请检查任务状态、数据类型和扫描模式");
                                                  return okData(scannerStateJson());
                                              }

                                              const auto target = MemUtils::ParseScanValue<T>(valueToken);
                                              if (!target) return fail("value 参数超出目标有符号类型范围");

                                              T rangeMax{};
                                              if (*fuzzyMode == Types::FuzzyMode::Range)
                                              {
                                                  const auto parsedRange = MemUtils::ParseScanValue<T>(rangeToken);
                                                  if (!parsedRange) return fail("range_max 参数超出目标有符号类型范围");
                                                  rangeMax = *parsedRange;
                                              }

                                              if (!runtime.scanner.startAsync<T>(pid, *target, *dataType, *fuzzyMode, isFirst, rangeMax, valueToken, rounding, alignment)) return fail("扫描请求被拒绝，请检查任务状态、数据类型和扫描模式");
                                              return okData(scannerStateJson());
                                          });
        }

        if (op == "scan.get") return okData(scannerStateJson());

        if (op == "scan.clear")
        {
            if (runtime.scanner.isScanning()) return fail("内存扫描运行中，无法清空结果");
            runtime.scanner.clear();
            return okData(scannerStateJson());
        }

        if (op == "scan.results")
        {
            const auto start = requiredUInt64("start", "start");
            const auto count = requiredUInt64("count", "count");
            const auto type = requiredString("value_type", "value_type");
            if (std::holds_alternative<json>(start)) return std::get<json>(start);
            if (std::holds_alternative<json>(count)) return std::get<json>(count);
            if (std::holds_alternative<json>(type)) return std::get<json>(type);
            if (std::get<std::uint64_t>(count) == 0 || std::get<std::uint64_t>(count) > 200) return fail("count 范围 1-200");

            const std::string typeToken = toLowerAscii(std::get<std::string>(type));
            const bool stringType = (typeToken == "str" || typeToken == "string" || typeToken == "text");
            const auto dataType = parseDataTypeToken(std::get<std::string>(type));
            const auto pageState = runtime.scanner.pageState(static_cast<size_t>(std::get<std::uint64_t>(start)), static_cast<size_t>(std::get<std::uint64_t>(count)));
            const auto &scannerState = pageState.state;
            if (scannerState.scanning) return fail("内存扫描运行中，请完成后获取结果");
            if (!stringType && !dataType.has_value()) return fail("value_type 参数无效");
            if (scannerState.stringScan != stringType) return fail("value_type 与当前扫描结果类型不一致");
            if (!stringType && (!scannerState.dataType.has_value() || *scannerState.dataType != *dataType)) return fail("value_type 与当前扫描结果类型不一致");
            const bool pointerType = scannerState.mode == Types::FuzzyMode::Pointer;

            json payload;
            payload["start"] = std::get<std::uint64_t>(start);
            payload["request_count"] = std::get<std::uint64_t>(count);
            payload["result_count"] = pageState.results.size();
            payload["total_count"] = scannerState.count;
            payload["type"] = std::get<std::string>(type);
            payload["mode"] = fuzzyModeToken(scannerState.mode);
            payload["value_format"] = pointerType ? "hex_address" : stringType ? "text" : "number";
            payload["items"] = json::array();
            for (const auto addr : pageState.results)
            {
                payload["items"].push_back({
                    {"addr", std::format("0x{:X}", static_cast<std::uint64_t>(addr))},
                    {"value", stringType    ? MemUtils::ReadAsText(addr)
                              : pointerType ? MemUtils::ReadAsPointerString(addr)
                                            : MemUtils::ReadAsString(addr, *dataType)},
                });
            }
            return okData(std::move(payload));
        }

        if (op == "viewer.open")
        {
            const auto address = requiredUInt64("address", "address");
            if (std::holds_alternative<json>(address)) return std::get<json>(address);
            const std::string viewFormat = optionalString("view_format");
            std::optional<Types::ViewFormat> format;
            if (!viewFormat.empty())
            {
                format = parseViewFormatToken(viewFormat);
                if (!format.has_value()) return fail("view_format 无效，支持: hexadecimal/hex/i8/i16/i32/i64/f32/f64/disasm");
            }
            runtime.viewer.open(static_cast<uintptr_t>(std::get<std::uint64_t>(address)), format);
            if (runtime.viewer.format() == Types::ViewFormat::Disasm) runtime.viewer.waitDisasm();
            return okData(buildViewerSnapshotJson(runtime.viewer));
        }

        if (op == "viewer.seek")
        {
            const auto offset = requiredString("offset", "offset");
            if (std::holds_alternative<json>(offset)) return std::get<json>(offset);
            if (!runtime.viewer.applyOffset(std::get<std::string>(offset))) return fail("offset 参数无效");
            if (runtime.viewer.format() == Types::ViewFormat::Disasm) runtime.viewer.waitDisasm();
            return okData(buildViewerSnapshotJson(runtime.viewer));
        }

        if (op == "viewer.format")
        {
            const auto viewFormat = requiredString("view_format", "view_format");
            if (std::holds_alternative<json>(viewFormat)) return std::get<json>(viewFormat);
            const auto format = parseViewFormatToken(std::get<std::string>(viewFormat));
            if (!format.has_value()) return fail("view_format 无效，支持: hexadecimal/hex/i8/i16/i32/i64/f32/f64/disasm");
            runtime.viewer.setFormat(*format);
            if (runtime.viewer.format() == Types::ViewFormat::Disasm) runtime.viewer.waitDisasm();
            return okData(buildViewerSnapshotJson(runtime.viewer));
        }

        if (op == "viewer.refresh")
        {
            runtime.viewer.refresh();
            if (runtime.viewer.format() == Types::ViewFormat::Disasm) runtime.viewer.waitDisasm();
            return okData(buildViewerSnapshotJson(runtime.viewer));
        }

        if (op == "pointer.get") return okData(pointerStateJson());

        if (op == "pointer.scan")
        {
            const std::string modeToken = optionalString("mode");
            const std::string mode = toLowerAscii(modeToken.empty() ? "module" : modeToken);
            const auto target = requiredUInt64("target", "target");
            const auto depth = requiredInt("depth", "depth");
            const auto maxOffset = requiredInt("max_offset", "max_offset");
            if (std::holds_alternative<json>(target)) return std::get<json>(target);
            if (std::holds_alternative<json>(depth)) return std::get<json>(depth);
            if (std::holds_alternative<json>(maxOffset)) return std::get<json>(maxOffset);
            if (std::get<int>(depth) <= 0 || std::get<int>(depth) > 16) return fail("depth 范围为 1-16");
            if (std::get<int>(maxOffset) <= 0) return fail("max_offset 必须大于 0");

            const bool useManual = (mode == "manual");
            const bool useArray = (mode == "array");
            std::uint64_t manualBase = 0;
            std::uint64_t arrayBase = 0;
            std::size_t arrayCount = 0;
            if (useManual)
            {
                const auto manual = requiredUInt64("manual_base", "manual_base");
                if (std::holds_alternative<json>(manual)) return std::get<json>(manual);
                manualBase = std::get<std::uint64_t>(manual);
            }
            else if (useArray)
            {
                const auto base = requiredUInt64("array_base", "array_base");
                const auto count = requiredUInt64("array_count", "array_count");
                if (std::holds_alternative<json>(base)) return std::get<json>(base);
                if (std::holds_alternative<json>(count)) return std::get<json>(count);
                if (std::get<std::uint64_t>(count) == 0 || std::get<std::uint64_t>(count) > 1000000) return fail("array_count 范围为 1-1000000");
                arrayBase = std::get<std::uint64_t>(base);
                arrayCount = static_cast<std::size_t>(std::get<std::uint64_t>(count));
            }
            else if (mode != "module")
            {
                return fail("mode 仅支持 module/manual/array");
            }

            const int pid = driver_.GetGlobalPid();
            if (pid <= 0) return fail("全局PID未设置，请先执行 target.select 或 target.attach");
            if (runtime.pointer.isBusy()) return fail("当前已有指针操作在运行");

            const std::string moduleFilter = optionalString("module_filter");
            if (!runtime.pointer.startAsync(pid, static_cast<uintptr_t>(std::get<std::uint64_t>(target)), std::get<int>(depth), std::get<int>(maxOffset), useManual, static_cast<uintptr_t>(manualBase), useArray, static_cast<uintptr_t>(arrayBase), arrayCount, moduleFilter)) return fail("指针扫描请求被拒绝，请检查目标、参数和当前操作状态");
            return okData(pointerStateJson());
        }

        if (op == "pointer.merge")
        {
            if (!runtime.pointer.MergeBins()) return fail("当前已有指针操作在运行，无法合并结果");
            return okData(pointerStateJson());
        }

        if (op == "pointer.export")
        {
            if (!runtime.pointer.ExportToTxt()) return fail("当前已有指针操作在运行，无法导出结果");
            return okData(pointerStateJson());
        }

        if (op == "breakpoint.get")
        {
            auto &info = driver_.GetBreakpointInfo();
            return okData({{"mode", runtime.hwbpMode}, {"bp_info", buildBreakpointDataJson(info)}});
        }

        if (op == "breakpoint.set")
        {
            const auto mode = requiredString("mode", "mode");
            if (std::holds_alternative<json>(mode)) return std::get<json>(mode);
            const std::string modeName = toLowerAscii(std::get<std::string>(mode));
            if (modeName != "hwbp" && modeName != "ptebp" && modeName != "stepbp") return fail("mode 无效，支持: hwbp/ptebp/stepbp");
            if (runtime.debug.busy()) return fail("先停止 Trace");
            if (!runtime.hwbpMode.empty()) return fail(std::format("{} 设置前请先移除当前断点", modeName));

            const int pid = driver_.GetGlobalPid();
            if (pid <= 0) return fail("全局PID未设置，请先执行 target.select 或 target.attach");

            auto parsedPoints = parseBreakpointPoints();
            if (std::holds_alternative<json>(parsedPoints)) return std::get<json>(parsedPoints);
            auto points = std::move(std::get<std::vector<Driver::bp_point>>(parsedPoints));

            const auto pointSpan = std::span<const Driver::bp_point>(points.data(), points.size());
            const int status = modeName == "hwbp" ? driver_.SetProcessHwbpRef(pointSpan) : (modeName == "stepbp" ? driver_.SetProcessStepbpRef(pointSpan) : driver_.SetProcessPtebpRef(pointSpan));
            if (status != 0) return fail(std::format("设置 {} 失败 status={}", modeName, status));
            return breakpointSetResult(status, modeName, points);
        }

        if (op == "breakpoint.clear")
        {
            auto &activeMode = runtime.hwbpMode;
            if (activeMode.empty()) return okData({{"mode", ""}, {"cleared", false}});

            if (activeMode == "hwbp") driver_.RemoveProcessHwbpRef();
            else if (activeMode == "stepbp") driver_.RemoveProcessStepbpRef();
            else driver_.RemoveProcessPtebpRef();
            activeMode.clear();
            return okData({{"mode", ""}, {"cleared", true}});
        }

        if (op == "breakpoint_record.update")
        {
            const auto index = requiredInt("index", "index");
            const auto field = requiredString("field", "field");
            const auto valueText = requiredString("value", "value");
            if (std::holds_alternative<json>(index)) return std::get<json>(index);
            if (std::holds_alternative<json>(field)) return std::get<json>(field);
            if (std::holds_alternative<json>(valueText)) return std::get<json>(valueText);
            const auto value = MemUtils::ParseUInt128(std::get<std::string>(valueText));
            if (!value.has_value()) return fail(std::format("operation={} 参数 value 无效", op));
            const int recordIndex = std::get<int>(index);
            if (recordIndex < 0) return fail("index 越界");
            auto &info = driver_.GetBreakpointInfo();
            int pointRecordIndex = recordIndex;
            for (int pointIndex = 0; pointIndex < BP_CONFIG_MAX; ++pointIndex)
            {
                auto &point = info.points[pointIndex];
                const int recordCount = std::clamp(point.record_count, 0, BP_RECORD_MAX);
                if (pointRecordIndex < recordCount)
                {
                    if (!MemUtils::AssignHwbpRecordField(point.records[pointRecordIndex], std::get<std::string>(field), *value)) return fail("field 无效");
                    return okData({{"index", recordIndex}, {"point_index", pointIndex}, {"point_record_index", pointRecordIndex}, {"field", std::get<std::string>(field)}, {"value_hex", MemUtils::FormatUInt128Hex(*value)}});
                }
                pointRecordIndex -= recordCount;
            }
            return fail("index 越界");
        }

        if (op == "signature.create")
        {
            const auto address = requiredUInt64("address", "address");
            const auto range = requiredInt("range", "range");
            if (std::holds_alternative<json>(address)) return std::get<json>(address);
            if (std::holds_alternative<json>(range)) return std::get<json>(range);
            const std::string requestedFile = optionalString("file_name");
            const std::string fileName = requestedFile.empty() ? std::string(SignatureScanner::SIG_DEFAULT_FILE) : requestedFile;
            if (!SignatureScanner::ScanAddressSignature(static_cast<uintptr_t>(std::get<std::uint64_t>(address)), std::get<int>(range), fileName.c_str())) return fail("特征码保存失败");
            return okData({{"saved", true}, {"file", fileName}});
        }

        if (op == "signature.scan")
        {
            const std::string requestedFile = optionalString("file_name");
            const std::string fileName = requestedFile.empty() ? std::string(SignatureScanner::SIG_DEFAULT_FILE) : requestedFile;
            json payload = buildSignatureMatchesJson(SignatureScanner::ScanSignatureFromFile(fileName.c_str()), 0, "");
            payload["file"] = fileName;
            return okData(std::move(payload));
        }

        if (op == "signature.match")
        {
            const auto rangeOffset = requiredInt64("range_offset", "range_offset");
            const auto pattern = requiredString("pattern", "pattern");
            if (std::holds_alternative<json>(rangeOffset)) return std::get<json>(rangeOffset);
            if (std::holds_alternative<json>(pattern)) return std::get<json>(pattern);
            if (std::get<std::int64_t>(rangeOffset) < static_cast<std::int64_t>(std::numeric_limits<int>::min()) || std::get<std::int64_t>(rangeOffset) > static_cast<std::int64_t>(std::numeric_limits<int>::max())) return fail("range_offset 无效");
            const auto matches = SignatureScanner::ScanSignature(std::get<std::string>(pattern).c_str(), static_cast<int>(std::get<std::int64_t>(rangeOffset)));
            return okData(buildSignatureMatchesJson(matches, std::get<std::int64_t>(rangeOffset), std::get<std::string>(pattern)));
        }

        if (op == "signature.filter")
        {
            const auto address = requiredUInt64("address", "address");
            if (std::holds_alternative<json>(address)) return std::get<json>(address);
            const std::string requestedFile = optionalString("file_name");
            const std::string fileName = requestedFile.empty() ? std::string(SignatureScanner::SIG_DEFAULT_FILE) : requestedFile;
            const auto result = SignatureScanner::FilterSignature(static_cast<uintptr_t>(std::get<std::uint64_t>(address)), fileName.c_str());
            return okData({{"success", result.success}, {"changed_count", result.changedCount}, {"total_count", result.totalCount}, {"old_signature", result.oldSignature}, {"new_signature", result.newSignature}, {"file", fileName}});
        }

        if (op == "saved.list") return okData(savedStatesJson());

        if (op == "saved.add")
        {
            const auto address = requiredUInt64("address", "address");
            const auto valueType = requiredString("value_type", "value_type");
            if (std::holds_alternative<json>(address)) return std::get<json>(address);
            if (std::holds_alternative<json>(valueType)) return std::get<json>(valueType);
            const auto dataType = parseDataTypeToken(std::get<std::string>(valueType));
            if (!dataType.has_value()) return fail("value_type 无效");

            const std::string kindText = optionalString("value_kind");
            const auto kind = parseSavedValueKindToken(kindText);
            if (!kind.has_value()) return fail("value_kind 无效");
            if (*kind == Types::SavedValueKind::Pointer && *dataType != Types::DataType::I64) return fail("pointer 保存项必须使用 i64");
            if (*kind == Types::SavedValueKind::Text && *dataType != Types::DataType::I8) return fail("text 保存项必须使用 i8");

            size_t textLength = 64;
            if (const std::string textLengthValue = optionalString("text_length"); !textLengthValue.empty())
            {
                const auto parsed = parseInt(textLengthValue);
                if (!parsed || *parsed < 1 || *parsed > 256) return fail("text_length 范围 1-256");
                textLength = static_cast<size_t>(*parsed);
            }

            auto &saved = runtime.saved;
            const uintptr_t savedAddress = static_cast<uintptr_t>(std::get<std::uint64_t>(address));
            const bool created = saved.add(savedAddress, *dataType, *kind, textLength);
            if (!created && !saved.contains(savedAddress)) return fail("保存地址失败");
            if (const std::string note = optionalString("note"); !note.empty() && !saved.setNote(savedAddress, note)) return fail("设置备注失败");
            json data = savedStatesJson();
            data["created"] = created;
            return okData(std::move(data));
        }

        if (op == "saved.remove")
        {
            const auto address = requiredUInt64("address", "address");
            if (std::holds_alternative<json>(address)) return std::get<json>(address);
            const bool removed = runtime.saved.remove(static_cast<uintptr_t>(std::get<std::uint64_t>(address)));
            json data = savedStatesJson();
            data["removed"] = removed;
            return okData(std::move(data));
        }

        if (op == "saved.write")
        {
            const auto address = requiredUInt64("address", "address");
            const auto value = requiredString("value", "value");
            if (std::holds_alternative<json>(address)) return std::get<json>(address);
            if (std::holds_alternative<json>(value)) return std::get<json>(value);
            if (!runtime.saved.write(static_cast<uintptr_t>(std::get<std::uint64_t>(address)), std::get<std::string>(value))) return fail("写入保存地址失败");
            return okData(savedStatesJson());
        }

        if (op == "saved.note.set")
        {
            const auto address = requiredUInt64("address", "address");
            if (std::holds_alternative<json>(address)) return std::get<json>(address);
            if (!runtime.saved.setNote(static_cast<uintptr_t>(std::get<std::uint64_t>(address)), optionalString("note"))) return fail("保存地址不存在");
            return okData(savedStatesJson());
        }

        if (op == "saved.lock.set")
        {
            const auto address = requiredUInt64("address", "address");
            const auto lockedText = requiredString("locked", "locked");
            if (std::holds_alternative<json>(address)) return std::get<json>(address);
            if (std::holds_alternative<json>(lockedText)) return std::get<json>(lockedText);
            const auto locked = parseBoolToken(std::get<std::string>(lockedText));
            if (!locked.has_value()) return fail("locked 无效");
            if (!runtime.saved.setLocked(static_cast<uintptr_t>(std::get<std::uint64_t>(address)), *locked, optionalString("value"))) return fail(*locked ? "锁定保存地址失败" : "保存地址不存在");
            return okData(savedStatesJson());
        }

        if (op == "saved.offset")
        {
            const auto offsetText = requiredString("offset", "offset");
            if (std::holds_alternative<json>(offsetText)) return std::get<json>(offsetText);
            const auto offset = MemUtils::ParseHexOffset(std::get<std::string>(offsetText));
            if (!offset.has_value()) return fail("offset 无效");
            runtime.saved.applyOffset(offset->offset, offset->negative);
            return okData(savedStatesJson());
        }

        if (op == "saved.clear")
        {
            runtime.saved.clear();
            return okData(savedStatesJson());
        }

        if (op == "memory.read")
        {
            const auto address = requiredUInt64("address", "address");
            const auto size = requiredInt("size", "size");
            if (std::holds_alternative<json>(address)) return std::get<json>(address);
            if (std::holds_alternative<json>(size)) return std::get<json>(size);
            constexpr int MAX_TRANSFER_SIZE = 1024 * 1024;
            const int byteCount = std::get<int>(size);
            if (byteCount <= 0 || byteCount > MAX_TRANSFER_SIZE) return fail("size 范围 1-1048576");
            auto result = driver_.ReadDetailed(std::get<std::uint64_t>(address), static_cast<size_t>(byteCount));
            if (!result.complete() && !params.value("allow_partial", false))
            {
                auto response = fail("读取不完整；设置 allow_partial=true 获取有效区段");
                response["read_bytes"] = result.validBytes;
                response["requested_bytes"] = byteCount;
                response["complete"] = false;
                return response;
            }
            json segments = json::array();
            for (const auto &segment : result.segments)
            {
                json item{{"address", lsdebug::Hex(segment.address)}, {"offset", segment.offset}, {"size", segment.size},
                    {"status", segment.status}, {"valid", segment.valid}};
                if (segment.valid) item["data_hex"] = bytesToHex(result.bytes.data() + segment.offset, segment.size);
                segments.push_back(std::move(item));
            }
            return okData({
                {"address", lsdebug::Hex(std::get<std::uint64_t>(address))}, {"size", byteCount},
                {"requested_bytes", byteCount}, {"read_bytes", result.validBytes}, {"complete", result.complete()},
                // Legacy clients receive flat data only when every byte is valid.
                {"data_hex", result.complete() ? bytesToHex(result.bytes.data(), result.bytes.size()) : ""},
                {"segments", std::move(segments)},
            });
        }

        if (op == "memory.write")
        {
            const auto address = requiredUInt64("address", "address");
            const auto dataHex = requiredString("data_hex", "data_hex");
            if (std::holds_alternative<json>(address)) return std::get<json>(address);
            if (std::holds_alternative<json>(dataHex)) return std::get<json>(dataHex);
            auto bytes = parseHexBytes(std::get<std::string>(dataHex));
            if (!bytes.has_value() || bytes->empty()) return fail("data_hex 无效");
            constexpr size_t MAX_TRANSFER_SIZE = 1024 * 1024;
            if (bytes->size() > MAX_TRANSFER_SIZE) return fail("data_hex 最大 1048576 字节");
            const int writeBytes = driver_.Write(std::get<std::uint64_t>(address), bytes->data(), bytes->size());
            if (writeBytes != static_cast<int>(bytes->size())) return fail(std::format("写入失败 status={}", writeBytes));
            std::vector<std::uint8_t> readback(bytes->size());
            const int readBytes = driver_.Read(std::get<std::uint64_t>(address), readback.data(), readback.size());
            const bool readbackComplete = readBytes == static_cast<int>(readback.size());
            return okData({
                {"address", std::format("0x{:X}", std::get<std::uint64_t>(address))},
                {"size", bytes->size()},
                {"verified", readbackComplete && readback == *bytes},
                {"readback_status", readBytes},
                {"readback_hex", readbackComplete ? bytesToHex(readback.data(), readback.size()) : ""},
            });
        }

        if (op.starts_with("wxshadow."))
        {
            if (op == "wxshadow.status")
            {
                ls_wxshadow_message message{};
                message.command = LS_WXSHADOW_INFO;
                const int status = driver_.WxShadowCall(message);
                if (status) return fail("wxshadow 状态查询失败 status=" + std::to_string(status));
                return okData({
                    {"status", message.status},
                    {"page_size", message.page_size},
                    {"cap_patch", (message.flags & LS_WXSHADOW_CAP_PATCH) != 0},
                    {"cap_read_hide", (message.flags & LS_WXSHADOW_CAP_READ_HIDE) != 0},
                    {"cap_gup_hide", (message.flags & LS_WXSHADOW_CAP_GUP_HIDE) != 0},
                    {"cap_fork_safe", (message.flags & LS_WXSHADOW_CAP_FORK_SAFE) != 0},
                });
            }
            if (op == "wxshadow.enable")
            {
                const auto enabled = requiredInt("enabled", "enabled");
                if (std::holds_alternative<json>(enabled)) return std::get<json>(enabled);
                const int val = std::get<int>(enabled);
                if (val != 0 && val != 1) return fail("enabled 必须为 0 或 1");
                ls_wxshadow_message message{};
                message.command = LS_WXSHADOW_ENABLE;
                message.flags = val ? 1U : 0U;
                const int status = driver_.WxShadowCall(message);
                if (status) return fail("wxshadow enable 失败 status=" + std::to_string(status));
                return okData({{"enabled", val != 0}, {"status", message.status}});
            }
            if (op == "wxshadow.patch")
            {
                const auto address = requiredUInt64("address", "address");
                const auto dataHex = requiredString("data_hex", "data_hex");
                if (std::holds_alternative<json>(address)) return std::get<json>(address);
                if (std::holds_alternative<json>(dataHex)) return std::get<json>(dataHex);
                auto bytes = parseHexBytes(std::get<std::string>(dataHex));
                if (!bytes.has_value() || bytes->empty()) return fail("data_hex 无效");
                if (bytes->size() > LS_WXSHADOW_PATCH_MAX) return fail(std::format("data_hex 最大 {} 字节", LS_WXSHADOW_PATCH_MAX));

                // 按 4KB 页边界拆分跨页写入；多块提交时，任一块失败即返回部分成功进度。
                const uint64_t start = std::get<std::uint64_t>(address);
                const size_t total = bytes->size();
                size_t written = 0;
                uint32_t chunks = 0;
                while (written < total)
                {
                    const uint64_t chunkAddr = start + written;
                    const size_t pageRemain = 0x1000 - (chunkAddr & 0xFFF);
                    const size_t chunkLen = std::min({total - written, pageRemain, static_cast<size_t>(LS_WXSHADOW_PATCH_MAX)});
                    ls_wxshadow_message message{};
                    message.command = LS_WXSHADOW_PATCH;
                    message.tgid = driver_.GetGlobalPid();
                    message.addr = chunkAddr;
                    message.len = chunkLen;
                    __builtin_memcpy(message.u.patch, bytes->data() + written, chunkLen);
                    const int status = driver_.WxShadowCall(message);
                    if (status) return fail(std::format("wxshadow patch 失败 status={}（已写入 {}/{} 字节）", status, written, total));
                    written += chunkLen;
                    ++chunks;
                }
                return okData({
                    {"address", std::format("0x{:X}", start)},
                    {"size", total},
                    {"chunks", chunks},
                    {"status", 0},
                });
            }
            if (op == "wxshadow.release")
            {
                ls_wxshadow_message message{};
                message.command = LS_WXSHADOW_RELEASE;
                message.tgid = driver_.GetGlobalPid();
                const std::string addressText = optionalString("address");
                if (params.contains("all")) message.scope = LS_WXSHADOW_REL_ALL;
                else if (!addressText.empty())
                {
                    const auto address = MemUtils::ParseUInt64(addressText);
                    if (!address.has_value()) return fail("address 无效");
                    message.addr = *address;
                    message.scope = LS_WXSHADOW_REL_ADDR;
                }
                else message.scope = LS_WXSHADOW_REL_TGID;
                const int status = driver_.WxShadowCall(message);
                if (status) return fail("wxshadow release 失败 status=" + std::to_string(status));
                return okData({{"status", message.status}});
            }
            if (op == "wxshadow.list")
            {
                ls_wxshadow_message message{};
                message.command = LS_WXSHADOW_LIST;
                const int status = driver_.WxShadowCall(message);
                if (status) return fail("wxshadow list 失败 status=" + std::to_string(status));
                json pages = json::array();
                const uint32_t count = message.list_count < LS_WXSHADOW_LIST_MAX ? message.list_count : LS_WXSHADOW_LIST_MAX;
                for (uint32_t i = 0; i < count; ++i)
                {
                    pages.push_back({
                        {"tgid", message.u.list[i].tgid},
                        {"state", message.u.list[i].state},
                        {"patch_count", message.u.list[i].patch_count},
                        {"page_addr", std::format("0x{:X}", message.u.list[i].page_addr)},
                    });
                }
                return okData({
                    {"page_count", message.page_count},
                    {"list_count", message.list_count},
                    {"pages", std::move(pages)},
                });
            }
            return fail("未知 wxshadow operation");
        }

        return fail(std::format("未知 operation: {}", op));
    } catch (const std::exception &error)
    {
        return {{"ok", false}, {"operation", std::string(operation)}, {"error", error.what()}};
    }


};
