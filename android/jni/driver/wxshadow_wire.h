#ifndef LS_WXSHADOW_WIRE_H
#define LS_WXSHADOW_WIRE_H

#include <cstdint>

// R^X Shadow Page 通信协议（append-only，复用 request_obj 内既有的 4096 字节 vmemrw buffer）。
// 与内核 lsdriver/io_version/wxshadow_wire.h 保持字段级一致；当前内核对 request_op_wxshadow
// 返回 -EOPNOTSUPP（请求编号保留，能力待内核侧启用后再开放）。

#define LS_WXSHADOW_MAGIC 0x57583031U /* "WX01" */
#define LS_WXSHADOW_ABI   1U

/* 能力位 (INFO 命令返回, flags 字段) */
#define LS_WXSHADOW_CAP_PATCH     1U
#define LS_WXSHADOW_CAP_READ_HIDE 2U
#define LS_WXSHADOW_CAP_GUP_HIDE  4U
#define LS_WXSHADOW_CAP_FORK_SAFE 8U

/* 命令 */
#define LS_WXSHADOW_INFO    1U
#define LS_WXSHADOW_PATCH   2U
#define LS_WXSHADOW_RELEASE 3U
#define LS_WXSHADOW_LIST    4U
#define LS_WXSHADOW_ENABLE  5U /* flags=1 开启 / 0 关闭整个 R^X shadow 机制 */

/* RELEASE 作用域 (scope 字段; addr==0 时按 scope 释放) */
#define LS_WXSHADOW_REL_ADDR 0U
#define LS_WXSHADOW_REL_TGID 1U
#define LS_WXSHADOW_REL_ALL  2U

/* 受管页状态 (LIST 命令返回) */
enum ls_wxshadow_page_state
{
    LS_WXSHADOW_STATE_SHADOW_X = 1,
    LS_WXSHADOW_STATE_ORIGINAL = 2,
    LS_WXSHADOW_STATE_DORMANT  = 3,
};

#define LS_WXSHADOW_LIST_MAX  64U
#define LS_WXSHADOW_PATCH_MAX 4032U /* 保证整条消息 <= 4096 字节 */

struct ls_wxshadow_page_info
{
    uint32_t tgid;
    uint32_t state;
    uint32_t patch_count;
    uint32_t reserved;
    uint64_t page_addr;
};

struct ls_wxshadow_message
{
    uint32_t magic;
    uint32_t abi;
    uint32_t command;
    uint32_t flags;
    uint32_t tgid;
    uint32_t scope;
    uint64_t addr;
    uint64_t len;
    int32_t  status;
    uint32_t page_size;
    uint32_t page_count;
    uint32_t list_count;
    union
    {
        uint8_t patch[LS_WXSHADOW_PATCH_MAX];
        ls_wxshadow_page_info list[LS_WXSHADOW_LIST_MAX];
    } u;
};

static_assert(sizeof(ls_wxshadow_message) <= 0x1000, "ls_wxshadow_message must fit in 4096-byte user_buffer");

#endif /* LS_WXSHADOW_WIRE_H */
