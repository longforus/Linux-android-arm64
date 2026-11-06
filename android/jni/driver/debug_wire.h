#ifndef LS_DEBUG_WIRE_H
#define LS_DEBUG_WIRE_H

// Append-only extension over the EXISTING 4096-byte vmemrw buffer. Do not grow
// request_obj: an old kernel pins exactly its original number of pages.
#define LS_DEBUG_MAGIC 0x4c534431U
#define LS_DEBUG_ABI 1U
#define LS_DEBUG_RING 256U
#define LS_DEBUG_FRAMES 8U
#define LS_DEBUG_BATCH 8U
#define LS_DEBUG_CAP_EVENTS 1U
#define LS_DEBUG_CAP_FP 2U
#define LS_DEBUG_CAP_STEP_TRACE 4U
#define LS_DEBUG_INFO 1U
#define LS_DEBUG_EVENTS_START 2U
#define LS_DEBUG_EVENTS_STOP 3U
#define LS_DEBUG_READ 4U
#define LS_DEBUG_TRACE_START 5U
#define LS_DEBUG_TRACE_STOP 6U
#define LS_DEBUG_STATUS 7U
#define LS_DEBUG_REASON_NONE 0U
#define LS_DEBUG_REASON_LIMIT 1U
#define LS_DEBUG_REASON_EXIT 2U
#define LS_DEBUG_REASON_STOP 3U
#define LS_DEBUG_FRAME_END 0U
#define LS_DEBUG_FRAME_LIMIT 1U
#define LS_DEBUG_FRAME_UNREADABLE 2U
#define LS_DEBUG_FRAME_INVALID 3U

struct ls_debug_frame { uint64_t fp, lr; };
struct ls_debug_event
{
    uint64_t seq, timestamp_ns, process_start_ns;
    uint64_t pc, lr, sp, fp;
    uint32_t tgid, tid, kind, frame_count, frame_reason, reserved;
    struct ls_debug_frame frames[LS_DEBUG_FRAMES];
};
struct ls_debug_message
{
    uint32_t magic, abi, command, flags;
    uint32_t tgid, tid, count, active;
    uint64_t stream, after, start_pc, max_steps;
    uint64_t captured, dropped, oldest, newest, process_start_ns;
    uint32_t reason, page_size;
    char version[32];
    struct ls_debug_event events[LS_DEBUG_BATCH];
};
#endif
