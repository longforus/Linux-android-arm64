#ifndef LS_DEBUG_CAPTURE_H
#define LS_DEBUG_CAPTURE_H
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include "io_version/debug_wire.h"

static DEFINE_SPINLOCK(ls_debug_lock);
static struct ls_debug_event ls_debug_ring[LS_DEBUG_RING];
static uint64_t ls_debug_stream, ls_debug_total, ls_debug_start_ns;
static uint64_t ls_debug_max_steps, ls_debug_start_pc;
static uint32_t ls_debug_pid, ls_debug_tid, ls_debug_reason;
static bool ls_debug_active, ls_debug_trace, ls_debug_triggered;
static struct break_point *ls_debug_trace_config;
static long (*ls_debug_copy_nofault)(void *, const void __user *, size_t);

// A recycled PID must not inherit, or be disarmed by, an old trace configuration.
static bool ls_debug_info_matches(const struct break_point *info, struct task_struct *task)
{
    return info != READ_ONCE(ls_debug_trace_config) ||
        (task && READ_ONCE(task->group_leader->start_time) == READ_ONCE(ls_debug_start_ns));
}

static bool ls_debug_trace_finished(struct task_struct *task)
{
    return READ_ONCE(ls_debug_trace) && task &&
        READ_ONCE(ls_debug_tid) == task->pid && !READ_ONCE(ls_debug_active);
}

// Exception context: bounded reads only, no allocation, faults or sleeping.
static void ls_debug_capture(struct pt_regs *regs, bool step)
{
    struct ls_debug_event event = {0};
    unsigned long irq;
    uint64_t stream, fp;
    unsigned int i;
    if (!regs || !user_mode(regs) || !READ_ONCE(ls_debug_active)) return;
    if (READ_ONCE(ls_debug_trace) != step || current->tgid != READ_ONCE(ls_debug_pid)) return;
    if (READ_ONCE(current->group_leader->start_time) != READ_ONCE(ls_debug_start_ns)) return;
    if (step && current->pid != READ_ONCE(ls_debug_tid)) return;
    stream = READ_ONCE(ls_debug_stream);
    event.timestamp_ns = ktime_get_ns();
    event.process_start_ns = READ_ONCE(current->group_leader->start_time);
    event.tgid = current->tgid;
    event.tid = current->pid;
    event.kind = step ? 2 : 1;
    event.pc = regs->pc;
    event.lr = regs->regs[30];
    event.sp = regs->sp;
    event.fp = regs->regs[29];
    fp = event.fp;
    if (!step && fp && !READ_ONCE(ls_debug_copy_nofault)) event.frame_reason = LS_DEBUG_FRAME_UNREADABLE;
    if (!step && READ_ONCE(ls_debug_copy_nofault))
    {
        for (i = 0; i < LS_DEBUG_FRAMES && fp; i++)
        {
            struct ls_debug_frame frame;
            if ((fp & 15) || fp < event.sp || fp >= TASK_SIZE - sizeof(frame))
            { event.frame_reason = LS_DEBUG_FRAME_INVALID; break; }
            if (ls_debug_copy_nofault(&frame, (const void __user *)(uintptr_t)fp, sizeof(frame)))
            { event.frame_reason = LS_DEBUG_FRAME_UNREADABLE; break; }
            event.frames[event.frame_count++] = frame;
            if (!frame.fp) break;
            if (frame.fp <= fp || frame.fp - fp > 1024 * 1024)
            { event.frame_reason = LS_DEBUG_FRAME_INVALID; break; }
            fp = frame.fp;
            if (i + 1 == LS_DEBUG_FRAMES) event.frame_reason = LS_DEBUG_FRAME_LIMIT;
        }
    }
    spin_lock_irqsave(&ls_debug_lock, irq);
    if (ls_debug_active && stream == ls_debug_stream)
    {
        if (step && !ls_debug_triggered && event.pc == ls_debug_start_pc) ls_debug_triggered = true;
        if (!step || ls_debug_triggered)
        {
            event.seq = ++ls_debug_total;
            ls_debug_ring[(event.seq - 1) % LS_DEBUG_RING] = event;
            if (step && ls_debug_total >= ls_debug_max_steps)
            { ls_debug_active = false; ls_debug_reason = LS_DEBUG_REASON_LIMIT; }
        }
    }
    spin_unlock_irqrestore(&ls_debug_lock, irq);
}

static void ls_debug_target_exit(struct task_struct *task)
{
    unsigned long irq;
    spin_lock_irqsave(&ls_debug_lock, irq);
    if (ls_debug_active && task && ls_debug_pid == task->tgid &&
        ls_debug_start_ns == READ_ONCE(task->group_leader->start_time))
    { ls_debug_active = false; ls_debug_reason = LS_DEBUG_REASON_EXIT; }
    spin_unlock_irqrestore(&ls_debug_lock, irq);
}

static int ls_debug_begin(struct ls_debug_message *message, bool trace)
{
    struct task_struct *task;
    unsigned long irq;
    uint64_t start;
    if (!message->tgid || (trace && (message->tid != message->tgid ||
        !message->start_pc || (message->start_pc & 3) || message->start_pc >= TASK_SIZE ||
        !message->max_steps || message->max_steps > 1000000))) return -EINVAL;
    rcu_read_lock();
    task = find_task_by_vpid(message->tgid);
    if (!task || task->tgid != message->tgid || (READ_ONCE(task->flags) & PF_EXITING))
    { rcu_read_unlock(); return -ESRCH; }
    start = READ_ONCE(task->group_leader->start_time);
    rcu_read_unlock();
    ls_debug_copy_nofault = (void *)generic_kallsyms_lookup_name("copy_from_user_nofault");
    spin_lock_irqsave(&ls_debug_lock, irq);
    if (ls_debug_active || ls_debug_trace)
    { spin_unlock_irqrestore(&ls_debug_lock, irq); return -EBUSY; }
    ls_debug_stream++;
    ls_debug_pid = message->tgid;
    ls_debug_tid = trace ? message->tid : 0;
    ls_debug_start_ns = start;
    ls_debug_start_pc = message->start_pc;
    ls_debug_max_steps = message->max_steps;
    ls_debug_total = 0;
    ls_debug_reason = LS_DEBUG_REASON_NONE;
    ls_debug_triggered = false;
    ls_debug_trace = trace;
    ls_debug_active = true;
    message->stream = ls_debug_stream;
    message->process_start_ns = start;
    spin_unlock_irqrestore(&ls_debug_lock, irq);
    return 0;
}

static void ls_debug_stop(bool release_trace)
{
    unsigned long irq;
    spin_lock_irqsave(&ls_debug_lock, irq);
    ls_debug_active = false;
    if (!ls_debug_reason) ls_debug_reason = LS_DEBUG_REASON_STOP;
    if (release_trace) ls_debug_trace = false;
    spin_unlock_irqrestore(&ls_debug_lock, irq);
}

static int ls_debug_snapshot(struct ls_debug_message *message, bool read_events)
{
    unsigned long irq;
    uint64_t seq, first;
    uint32_t limit = message->count;
    if (read_events && (!limit || limit > LS_DEBUG_BATCH)) return -EINVAL;
    spin_lock_irqsave(&ls_debug_lock, irq);
    if (message->stream && message->stream != ls_debug_stream)
    { spin_unlock_irqrestore(&ls_debug_lock, irq); return -ESTALE; }
    if (read_events && message->after > ls_debug_total)
    { spin_unlock_irqrestore(&ls_debug_lock, irq); return -ERANGE; }
    message->stream = ls_debug_stream;
    message->active = ls_debug_active;
    message->captured = ls_debug_total;
    message->oldest = ls_debug_total > LS_DEBUG_RING ? ls_debug_total - LS_DEBUG_RING + 1 : 1;
    message->newest = ls_debug_total;
    message->dropped = ls_debug_total > LS_DEBUG_RING ? ls_debug_total - LS_DEBUG_RING : 0;
    message->process_start_ns = ls_debug_start_ns;
    message->reason = ls_debug_reason;
    message->count = 0;
    if (read_events && message->after < ls_debug_total)
    {
        first = message->after + 1;
        if (first < message->oldest) first = message->oldest;
        for (seq = first; seq <= ls_debug_total && message->count < limit; seq++)
            message->events[message->count++] = ls_debug_ring[(seq - 1) % LS_DEBUG_RING];
    }
    spin_unlock_irqrestore(&ls_debug_lock, irq);
    return 0;
}
#endif
