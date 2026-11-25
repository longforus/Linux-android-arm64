#ifndef LS_WXSHADOW_WIRE_H
#define LS_WXSHADOW_WIRE_H

/*
 * R^X Shadow Page 通信协议 (append-only, 复用 request_obj 内既有的 4096 字节 vmemrw buffer)
 *
 * 与 debug_wire.h 同源：不新增 request_op、不改变 sizeof(struct request_obj)，
 * 由内核与用户态共享本结构体定义。内核把 struct ls_wxshadow_message 直接映射到
 * req->vmemrw_info.user_buffer (4096 字节)，用户态在 driver/wxshadow_wire.h 保持同一布局。
 *
 * 注意：请求体整体必须 <= 4096 字节，下面用 _Static_assert 在编译期保证。
 */

#include <linux/types.h>

#define LS_WXSHADOW_MAGIC 0x57583031U /* "WX01" */
#define LS_WXSHADOW_ABI   1U

/* 能力位 (INFO 命令返回, flags 字段) */
#define LS_WXSHADOW_CAP_PATCH     1U /* R^X shadow patch */
#define LS_WXSHADOW_CAP_READ_HIDE 2U /* do_page_fault 读隐藏 */
#define LS_WXSHADOW_CAP_GUP_HIDE  4U /* follow_page_pte GUP 读取隐藏 */
#define LS_WXSHADOW_CAP_FORK_SAFE 8U /* dup_mmap fork 保护 */

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
enum ls_wxshadow_page_state {
	LS_WXSHADOW_STATE_SHADOW_X = 1, /* --x 激活, 执行走 shadow */
	LS_WXSHADOW_STATE_ORIGINAL = 2, /* r-- 读隐藏态, 执行触发权限异常 */
	LS_WXSHADOW_STATE_DORMANT  = 3, /* hook 退休, 保留 shadow 但映射已恢复 */
};

#define LS_WXSHADOW_LIST_MAX  64U
#define LS_WXSHADOW_PATCH_MAX 4032U /* 保证整条消息 <= 4096 字节 */

struct ls_wxshadow_page_info {
	uint32_t tgid;
	uint32_t state;       /* enum ls_wxshadow_page_state */
	uint32_t patch_count; /* 该页活动 patch 数 */
	uint32_t reserved;
	uint64_t page_addr;
};

struct ls_wxshadow_message {
	uint32_t magic;
	uint32_t abi;
	uint32_t command;  /* LS_WXSHADOW_* */
	uint32_t flags;    /* 输入: 能力/作用域标志; INFO 输出: 能力位 */
	uint32_t tgid;     /* 目标进程 TGID */
	uint32_t scope;    /* RELEASE: LS_WXSHADOW_REL_* */
	uint64_t addr;     /* PATCH: 目标虚拟地址; RELEASE(REL_ADDR): 目标地址 */
	uint64_t len;      /* PATCH: 补丁长度 */
	int32_t  status;   /* 输出: 0 或 -errno */
	uint32_t page_size;   /* 输出: PAGE_SIZE */
	uint32_t page_count;  /* 输出: 受管页总数 */
	uint32_t list_count;  /* 输出: u.list[] 已填条目数 */
	union {
		uint8_t patch[LS_WXSHADOW_PATCH_MAX];                    /* PATCH: 补丁字节 */
		struct ls_wxshadow_page_info list[LS_WXSHADOW_LIST_MAX]; /* LIST: 受管页 */
	} u;
};

/* 编译期保证整条消息塞进 4096 字节 user_buffer */
_Static_assert(sizeof(struct ls_wxshadow_message) <= 0x1000,
	       "ls_wxshadow_message must fit in 4096-byte user_buffer");

#endif /* LS_WXSHADOW_WIRE_H */
