#ifndef ARM64_WXSHADOW_H
#define ARM64_WXSHADOW_H

/*
 * W^X (R^X) Shadow Page — lsdriver 原生子模块
 *
 * 抽取 KernelPatch wxshadow 的 R^X 核心逻辑，完全改用 Pixel 6 (5.10.157) 原生内核
 * API 与 lsdriver 基础设施，去除 KPM 的偏移扫描与 prctl syscall hook：
 *
 *   1. 页表与权限：基于原始 PTE 快照重建映射
 *        - SHADOW (--x)：清 PTE_USER/PTE_RDONLY/PTE_UXN，置 PTE_PXN -> 仅可执行
 *        - ORIGINAL (r--)：置 PTE_USER|PTE_RDONLY|PTE_UXN -> 仅可读
 *      用 get_user_pte()/set_pte()/flush_tlb_addr_all_asid_all_cpus() 完成无锁切换；
 *      缓存同步复用 arm64_sync_code_range_all_cpus()。
 *   2. 关键 hook 懒安装/自动卸载：
 *        - do_page_fault（必选）：读异常 SHADOW(--x)->ORIGINAL(r--)，取指异常
 *          ORIGINAL(r--)->SHADOW(--x)，写异常撤销该页；均 regs[0]=0; return 1 跳过原 handler。
 *        - follow_page_pte（可选，GUP 读隐藏）：入口切原始页(不刷 TLB)，返回跳板恢复 shadow。
 *        - dup_mmap（可选，fork 保护）：fork 前父进程 shadow 恢复原映射，返回跳板再激活。
 *        - exit_mmap（必选）：进程退出前恢复原 PTE，避免 zap 触发 Bad page map。
 *   3. 锁分层：全局 mutex 串行化管理路径；全局 spinlock(irqsave) 保护页链表与状态并串行化 PTE 切换。
 *
 * 本模块为 header-only（单 TU），由 lsdriver.c 统一包含。
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <asm/esr.h>
#include <asm/pgtable.h>
#include <asm/pgtable-hwdef.h>
#include <asm/pgtable-prot.h>
#include <asm/ptrace.h>

#include "arm64_reg.h"
#include "export_fun.h"
#include "inline_hook_frame.h"
#include "io_version/io_struct.h"
#include "io_version/wxshadow_wire.h"
#include "klog/lsdriver_log.h"

#ifndef FOLL_WRITE
#define FOLL_WRITE 0x01
#endif

/* 内核内部页面状态 (数值与 wire 层对齐) */
enum wxshadow_state {
	WX_STATE_SHADOW_X = LS_WXSHADOW_STATE_SHADOW_X,
	WX_STATE_ORIGINAL = LS_WXSHADOW_STATE_ORIGINAL,
	WX_STATE_DORMANT  = LS_WXSHADOW_STATE_DORMANT,
};

#define WXSHADOW_MAX_PATCHES 32

struct wxshadow_patch {
	u16 offset;
	u16 len;
	void *data; /* kmalloc 分配, len 字节 */
};

struct wxshadow_page {
	struct list_head list;
	struct mm_struct *mm; /* 页面持有引用, 释放时 mmput */
	pid_t tgid;
	unsigned long page_addr;
	struct page *shadow_page; /* 私有 shadow 物理页 */
	pte_t original_pte;       /* 原始 PTE 快照(含原始 PFN) */
	int state;
	int refcount; /* 链表 1 引用 + 每个 find 调用者 1 引用 */
	bool dead;
	bool fork_paused;
	struct wxshadow_patch patches[WXSHADOW_MAX_PATCHES];
	int nr_patches;
};

struct wxshadow_ctx {
	struct list_head pages;
	spinlock_t lock; /* 保护页链表与状态, 并串行化 PTE 切换 (irqsave) */
	struct mutex mutex; /* 串行化管理路径 (PATCH/RELEASE/LIST) */

	struct hook_entry hook_mem_abort;
	struct hook_entry hook_follow_page_pte;
	struct hook_entry hook_dup_mmap;
	struct hook_entry hook_exit_mmap;

	bool hooks_installed;
	bool follow_page_pte_enabled;
	bool dup_mmap_enabled;

	/* mmu_notifier: 检测目标 mm 的 mprotect/munmap 等映射失效, 及时拆页恢复原文 */
	struct mmu_notifier mmu_notifier;
	struct mm_struct *notifier_mm;   /* notifier 当前注册的 mm */
	bool mmu_notifier_registered;
};

static struct wxshadow_ctx g_wxshadow = {
	.pages = LIST_HEAD_INIT(g_wxshadow.pages),
	.lock = __SPIN_LOCK_UNLOCKED(g_wxshadow.lock),
	.mutex = __MUTEX_INITIALIZER(g_wxshadow.mutex),
};

/* 运行时总开关: 关掉后 wxshadow.* 全部 fail-closed 回 -EOPNOTSUPP 且不装 hook,
 * 等价于回到旧方案 (memory.write / HWBP / DPTDBG / STEPBP)。默认开。 */
static bool g_wxshadow_enabled = true;

/* ============================ PTE 构造 ============================ */

/* 基于原始 PTE 快照重建：换 PFN、重写 USER/RDONLY/UXN，显式置 PXN。 */
static inline pte_t wx_build_pte(struct wxshadow_page *p, unsigned long pfn, pteval_t prot)
{
	pteval_t v = pte_val(p->original_pte);

	v &= ~(PTE_ADDR_MASK | PTE_USER | PTE_RDONLY | PTE_UXN);
	v |= (pfn << PAGE_SHIFT) & PTE_ADDR_MASK;
	v |= prot;
	v |= PTE_PXN | PTE_AF;
	return __pte(v);
}

static inline pte_t wx_pte_shadow(struct wxshadow_page *p)
{
	return wx_build_pte(p, page_to_pfn(p->shadow_page), 0); /* --x */
}

static inline pte_t wx_pte_original_read(struct wxshadow_page *p)
{
	return wx_build_pte(p, pte_pfn(p->original_pte), PTE_USER | PTE_RDONLY | PTE_UXN); /* r-- */
}

/*
 * 低层写 PTE + 刷 TLB。调用者负责加锁(全局 spinlock)。
 * 写入前校验当前 PTE 的 PFN 归属 (须为 shadow 或 original, 忽略 AF/DBM 动态位)，
 * 防止 COW/映射变更后误写。返回 0 成功；-EFAULT 页表项不存在/失效；-ESTALE 映射已变更。
 */
static int wx_write_pte_locked(struct wxshadow_page *p, pte_t new_pte, bool flush)
{
	pte_t *ptep = get_user_pte(p->mm, p->page_addr);
	pteval_t cur;
	unsigned long cur_pfn;

	if (!ptep)
		return -EFAULT;
	cur = pte_val(READ_ONCE(*ptep));
	if (!(cur & PTE_VALID))
		return -EFAULT;

	cur_pfn = (cur & PTE_ADDR_MASK) >> PAGE_SHIFT;
	if (cur_pfn != page_to_pfn(p->shadow_page) && cur_pfn != pte_pfn(p->original_pte))
		return -ESTALE;

	set_pte(ptep, new_pte);
	if (flush)
		flush_tlb_addr_all_asid_all_cpus(p->page_addr);
	return 0;
}

/* ============================ 页面生命周期 ============================ */

static void wx_page_free(struct wxshadow_page *p)
{
	struct mm_struct *mm = p->mm;
	int i;

	for (i = 0; i < p->nr_patches; i++)
		kfree(p->patches[i].data);
	if (p->shadow_page)
		__free_page(p->shadow_page);
	kfree(p);
	if (mm)
		mmdrop(mm); /* 释放 mmgrab 持有的 mm_count */
}

static void wx_page_put(struct wxshadow_page *p)
{
	struct wxshadow_page *to_free = NULL;
	unsigned long flags;

	spin_lock_irqsave(&g_wxshadow.lock, flags);
	if (--p->refcount == 0)
		to_free = p;
	spin_unlock_irqrestore(&g_wxshadow.lock, flags);

	if (to_free)
		wx_page_free(to_free);
}

/* 返回持有调用者引用的页面 (refcount 已 +1)，调用者须 wx_page_put。 */
static struct wxshadow_page *wx_find_page(struct mm_struct *mm, unsigned long addr)
{
	unsigned long page_addr = addr & PAGE_MASK;
	struct wxshadow_page *p;
	unsigned long flags;

	spin_lock_irqsave(&g_wxshadow.lock, flags);
	list_for_each_entry(p, &g_wxshadow.pages, list) {
		if (p->mm == mm && p->page_addr == page_addr && !p->dead) {
			p->refcount++;
			spin_unlock_irqrestore(&g_wxshadow.lock, flags);
			return p;
		}
	}
	spin_unlock_irqrestore(&g_wxshadow.lock, flags);
	return NULL;
}

/*
 * 创建受管页: 复制原始页到 shadow。
 * 用 mmgrab(mm) 仅保活 mm_struct(mm_count)，不卡住 mm_users——这样目标进程退出时
 * exit_mmap 仍能正常触发本模块的清理 hook。调用者仍须对传入的 mm 持 mm_users 引用
 * (get_mm_by_pid 返回值) 并在本函数返回后自行 mmput。
 * 成功返回 refcount=2 (链表引用 + 调用者引用)；失败返回 ERR_PTR，不 mmgrab。
 */
static struct wxshadow_page *wx_create_page(struct mm_struct *mm, pid_t tgid,
					    unsigned long page_addr)
{
	struct wxshadow_page *p;
	void *src, *dst;
	pteval_t raw;
	unsigned long flags;

	if (read_user_pte_value(mm, page_addr, &raw)) {
		ls_log_always_tag("wxshadow", "create_page: read_user_pte_value failed addr=%lx\n", page_addr);
		return ERR_PTR(-EFAULT);
	}
	if (!(raw & PTE_VALID) || !(raw & PTE_TYPE_PAGE)) {
		ls_log_always_tag("wxshadow", "create_page: PTE not present addr=%lx pte=%llx\n", page_addr, (unsigned long long)raw);
		return ERR_PTR(-EFAULT);
	}
	if (raw & PTE_UXN) {
		ls_log_always_tag("wxshadow", "create_page: PTE UXN (非可执行) addr=%lx\n", page_addr);
		return ERR_PTR(-EACCES); /* 仅接受可执行代码页 */
	}
	if (raw & PTE_CONT) {
		ls_log_always_tag("wxshadow", "create_page: PTE_CONT (64KB 连续块) 拒绝 addr=%lx\n", page_addr);
		return ERR_PTR(-EOPNOTSUPP); /* 拒绝 64KB 连续 PTE 块 */
	}
	if (!pfn_valid((raw & PTE_ADDR_MASK) >> PAGE_SHIFT)) {
		ls_log_always_tag("wxshadow", "create_page: pfn invalid addr=%lx\n", page_addr);
		return ERR_PTR(-EFAULT);
	}

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);

	p->original_pte = __pte(raw);

	p->shadow_page = alloc_page(GFP_KERNEL);
	if (!p->shadow_page) {
		kfree(p);
		return ERR_PTR(-ENOMEM);
	}

	src = page_address(pfn_to_page(pte_pfn(p->original_pte)));
	dst = page_address(p->shadow_page);
	if (!src || !dst) {
		__free_page(p->shadow_page);
		kfree(p);
		return ERR_PTR(-EFAULT);
	}
	__builtin_memcpy(dst, src, PAGE_SIZE);

	mmgrab(mm); /* 页面长期持有 mm_struct, 仅加 mm_count */
	p->mm = mm;
	p->tgid = tgid;
	p->page_addr = page_addr;
	p->state = WX_STATE_DORMANT;
	p->refcount = 2; /* 链表 + 调用者 */
	INIT_LIST_HEAD(&p->list);

	spin_lock_irqsave(&g_wxshadow.lock, flags);
	list_add(&p->list, &g_wxshadow.pages);
	spin_unlock_irqrestore(&g_wxshadow.lock, flags);

	return p;
}

/* ============================ shadow 重建 ============================ */

/* 从原始页重建整页 shadow 内容, 叠加所有活动 patch, 同步缓存。 */
static void wx_rebuild_shadow(struct wxshadow_page *p)
{
	void *dst = page_address(p->shadow_page);
	void *src = page_address(pfn_to_page(pte_pfn(p->original_pte)));
	int i;

	if (!dst || !src)
		return;
	__builtin_memcpy(dst, src, PAGE_SIZE);
	for (i = 0; i < p->nr_patches; i++) {
		struct wxshadow_patch *pt = &p->patches[i];
		if (pt->data && pt->len)
			__builtin_memcpy((char *)dst + pt->offset, pt->data, pt->len);
	}
	arm64_sync_code_range_all_cpus(dst, PAGE_SIZE);
}

/* ============================ 页面拆解 ============================ */

/*
 * 拆解单页: 恢复原始 PTE, 标记 dead, 从链表摘除, 释放链表引用。
 * 可重复调用 (dead 幂等)。调用者须持有一个调用者引用。
 */
static int wx_teardown_page(struct wxshadow_page *p)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&g_wxshadow.lock, flags);
	if (p->dead) {
		spin_unlock_irqrestore(&g_wxshadow.lock, flags);
		return 0;
	}
	p->dead = true;
	p->fork_paused = false;
	spin_unlock_irqrestore(&g_wxshadow.lock, flags);

	/* 恢复原始映射 (原 PTE 快照) */
	spin_lock_irqsave(&g_wxshadow.lock, flags);
	if (p->mm && p->page_addr)
		ret = wx_write_pte_locked(p, p->original_pte, true);
	spin_unlock_irqrestore(&g_wxshadow.lock, flags);

	spin_lock_irqsave(&g_wxshadow.lock, flags);
	list_del_init(&p->list);
	spin_unlock_irqrestore(&g_wxshadow.lock, flags);

	wx_page_put(p); /* 释放链表引用 */
	return ret;
}

/* pop-under-lock 逐页拆解指定 mm 的所有受管页 (并发安全)。 */
static int wx_teardown_pages_for_mm(struct mm_struct *mm)
{
	struct wxshadow_page *p, *tmp;
	unsigned long flags;
	int count = 0;

	for (;;) {
		p = NULL;
		spin_lock_irqsave(&g_wxshadow.lock, flags);
		list_for_each_entry(tmp, &g_wxshadow.pages, list) {
			if (tmp->mm == mm && !tmp->dead) {
				tmp->refcount++;
				p = tmp;
				break;
			}
		}
		spin_unlock_irqrestore(&g_wxshadow.lock, flags);
		if (!p)
			break;
		wx_teardown_page(p);
		wx_page_put(p);
		count++;
	}
	return count;
}

static int wx_teardown_all(void)
{
	struct wxshadow_page *p, *tmp;
	unsigned long flags;
	int count = 0;

	for (;;) {
		p = NULL;
		spin_lock_irqsave(&g_wxshadow.lock, flags);
		list_for_each_entry(tmp, &g_wxshadow.pages, list) {
			if (!tmp->dead) {
				tmp->refcount++;
				p = tmp;
				break;
			}
		}
		spin_unlock_irqrestore(&g_wxshadow.lock, flags);
		if (!p)
			break;
		wx_teardown_page(p);
		wx_page_put(p);
		count++;
	}
	return count;
}

/* ============================ mmu_notifier (映射失效检测) ============================ */

/* 拆解 [start, end) 范围内指定 mm 的受管页 (mprotect/munmap 等触发)。 */
static int wx_teardown_pages_in_range(struct mm_struct *mm, unsigned long start, unsigned long end)
{
	struct wxshadow_page *p, *tmp;
	unsigned long flags;
	int count = 0;

	for (;;) {
		p = NULL;
		spin_lock_irqsave(&g_wxshadow.lock, flags);
		list_for_each_entry(tmp, &g_wxshadow.pages, list) {
			if (tmp->mm == mm && !tmp->dead &&
			    tmp->page_addr >= start && tmp->page_addr < end) {
				tmp->refcount++;
				p = tmp;
				break;
			}
		}
		spin_unlock_irqrestore(&g_wxshadow.lock, flags);
		if (!p)
			break;
		wx_teardown_page(p);
		wx_page_put(p);
		count++;
	}
	if (count)
		ls_log_always_tag("wxshadow", "mmu_notifier: 拆解 %d 页 [%lx, %lx)\n", count, start, end);
	return count;
}

static void wx_mmu_notifier_release(struct mmu_notifier *mn, struct mm_struct *mm)
{
	(void)mn;
	wx_teardown_pages_for_mm(mm);
	g_wxshadow.mmu_notifier.ops = NULL;
	g_wxshadow.mmu_notifier_registered = false;
	g_wxshadow.notifier_mm = NULL;
}

static int wx_mmu_notifier_invalidate_range_start(struct mmu_notifier *mn,
						   const struct mmu_notifier_range *range)
{
	(void)mn;

	/*
	 * 只对真正会改变我们 PTE 的事件拆页:
	 *   UNMAP          -> munmap/mremap/exit 移除映射
	 *   CLEAR          -> zap/madvise 清 PTE, 或内存迁移 try_to_unmap (CMA/ksm/migrate)
	 *   PROTECTION_VMA -> mprotect 改权限 (本模块引入 notifier 的初衷)
	 *   MIGRATE        -> migrate_vma 迁移
	 * 跳过:
	 *   PROTECTION_PAGE -> 仅清除 PTE 写位 (fork 的 copy_page_range / page_mkclean /
	 *                      wp_clean/clear_refs)。我们的 shadow 页是 --x (永不可写),
	 *                      写保护对它无影响; fork 由 dup_mmap hook 单独处理。
	 *   SOFT_DIRTY / RELEASE -> 不改变我们的 PTE。
	 */
	switch (range->event) {
	case MMU_NOTIFY_UNMAP:
	case MMU_NOTIFY_CLEAR:
	case MMU_NOTIFY_PROTECTION_VMA:
	case MMU_NOTIFY_MIGRATE:
		break;
	default:
		return 0;
	}

	if (!mmu_notifier_range_blockable(range))
		return -EAGAIN;
	wx_teardown_pages_in_range(range->mm, range->start, range->end);
	return 0;
}

static const struct mmu_notifier_ops wx_mmu_notifier_ops = {
	.release = wx_mmu_notifier_release,
	.invalidate_range_start = wx_mmu_notifier_invalidate_range_start,
};

static int wx_register_mmu_notifier(struct mm_struct *mm)
{
	int (*register_fn)(struct mmu_notifier *, struct mm_struct *) =
		(void *)generic_kallsyms_lookup_name("mmu_notifier_register");

	if (!register_fn)
		return -EOPNOTSUPP;
	g_wxshadow.mmu_notifier.ops = &wx_mmu_notifier_ops;
	if (register_fn(&g_wxshadow.mmu_notifier, mm)) {
		g_wxshadow.mmu_notifier.ops = NULL;
		return -EINVAL;
	}
	g_wxshadow.notifier_mm = mm;
	g_wxshadow.mmu_notifier_registered = true;
	ls_log_always_tag("wxshadow", "mmu_notifier registered\n");
	return 0;
}

static void wx_unregister_mmu_notifier(void)
{
	void (*unregister_fn)(struct mmu_notifier *, struct mm_struct *) =
		(void *)generic_kallsyms_lookup_name("mmu_notifier_unregister");

	if (g_wxshadow.mmu_notifier_registered && unregister_fn) {
		unregister_fn(&g_wxshadow.mmu_notifier, g_wxshadow.notifier_mm);
		ls_log_always_tag("wxshadow", "mmu_notifier unregistered\n");
	}
	g_wxshadow.mmu_notifier.ops = NULL;
	g_wxshadow.mmu_notifier_registered = false;
	g_wxshadow.notifier_mm = NULL;
}

/* ============================ Fork 保护 ============================ */

/* fork 前把父进程所有活动 shadow 页切回 r--, 使子进程复制原始 PFN。 */
static int wx_pause_pages_for_mm(struct mm_struct *mm)
{
	struct wxshadow_page *p;
	unsigned long flags;
	int count = 0;

	spin_lock_irqsave(&g_wxshadow.lock, flags);
	list_for_each_entry(p, &g_wxshadow.pages, list) {
		if (p->mm != mm || p->dead || p->state != WX_STATE_SHADOW_X || p->fork_paused)
			continue;
		p->fork_paused = true;
		if (wx_write_pte_locked(p, wx_pte_original_read(p), true) == 0) {
			p->state = WX_STATE_ORIGINAL;
			count++;
		}
	}
	spin_unlock_irqrestore(&g_wxshadow.lock, flags);
	return count;
}

static int wx_resume_pages_for_mm(struct mm_struct *mm)
{
	struct wxshadow_page *p;
	unsigned long flags;
	int count = 0;

	spin_lock_irqsave(&g_wxshadow.lock, flags);
	list_for_each_entry(p, &g_wxshadow.pages, list) {
		if (p->mm != mm || p->dead || !p->fork_paused)
			continue;
		p->fork_paused = false;
		if (p->state == WX_STATE_ORIGINAL && wx_write_pte_locked(p, wx_pte_shadow(p), true) == 0) {
			p->state = WX_STATE_SHADOW_X;
			count++;
		}
	}
	spin_unlock_irqrestore(&g_wxshadow.lock, flags);
	return count;
}

/* ============================ Hook 工作函数 ============================ */

/* --- exit_mmap(mm) --- */
static int wx_exit_mmap_work(struct pt_regs *hook_regs)
{
	struct mm_struct *mm = (struct mm_struct *)(uintptr_t)hook_regs->regs[0];

	if (mm)
		wx_teardown_pages_for_mm(mm);
	return 0;
}

/* --- do_mem_abort(far, esr, regs) --- */
static int wx_mem_abort_work(struct pt_regs *hook_regs)
{
	unsigned long far = hook_regs->regs[0];
	unsigned int esr = (unsigned int)hook_regs->regs[1];
	struct pt_regs *regs = (struct pt_regs *)(uintptr_t)hook_regs->regs[2];
	struct mm_struct *mm;
	struct wxshadow_page *p;
	unsigned long flags;
	unsigned int ec;
	bool need_teardown = false;
	int handled = 0;

	if (!regs || !user_mode(regs) || !g_wxshadow.hooks_installed)
		return 0;
	mm = current->mm;
	if (!mm)
		return 0;

	ec = ESR_ELx_EC(esr);
	if (ec != ESR_ELx_EC_IABT_LOW && ec != ESR_ELx_EC_DABT_LOW)
		return 0;
	/* 仅处理权限异常 (FSC=0x0c..0x0f); 其它(缺页/访问标志)交给原 handler */
	if (((esr & 0x3f) & 0x3c) != 0x0c)
		return 0;
	/* stage-1 页表遍历异常 (S1PTW) 不是对受管页的数据访问, 交给原 handler */
	if (esr & ESR_ELx_S1PTW)
		return 0;

	p = wx_find_page(mm, untagged_addr(far));
	if (!p)
		return 0;

	spin_lock_irqsave(&g_wxshadow.lock, flags);
	if (!p->dead) {
		if (ec == ESR_ELx_EC_IABT_LOW) {
			/* 取指异常: ORIGINAL -> SHADOW (--x) */
			if (p->state == WX_STATE_ORIGINAL && !p->fork_paused) {
				if (wx_write_pte_locked(p, wx_pte_shadow(p), true) == 0) {
					p->state = WX_STATE_SHADOW_X;
					handled = 1;
				} else {
					need_teardown = true;
				}
			}
		} else if (esr & ESR_ELx_WNR) {
			/* 写异常: 撤销该页 shadow */
			need_teardown = true;
		} else {
			/* 读异常 (含 cache maintenance CM): SHADOW -> ORIGINAL (r--) */
			if (p->state == WX_STATE_SHADOW_X) {
				if (wx_write_pte_locked(p, wx_pte_original_read(p), true) == 0) {
					p->state = WX_STATE_ORIGINAL;
					handled = 1;
				} else {
					need_teardown = true;
				}
			}
		}
	}
	spin_unlock_irqrestore(&g_wxshadow.lock, flags);

	if (need_teardown)
		wx_teardown_page(p);
	wx_page_put(p);

	if (handled)
		return 1; /* 跳过原 do_mem_abort, 异常返回 EL0 重试 */
	return 0;
}

/* --- follow_page_pte(vma, address, pmd, flags, pgmap) GUP 读隐藏 --- */

static void __attribute__((used, __noinline__)) wx_follow_page_pte_return_work(void *meta);

__attribute__((naked, used)) void wx_ret_trampoline_follow_page_pte(void)
{
	asm volatile("str x0, [sp, #8]\n"
		     "mov x0, sp\n"
		     "bl wx_follow_page_pte_return_work\n"
		     "ldp x16, x0, [sp], #304\n"
		     "ret x16\n");
}

static void __attribute__((used, __noinline__)) wx_follow_page_pte_return_work(void *meta)
{
	struct wxshadow_page *p = *(struct wxshadow_page **)((char *)meta + 16);
	unsigned long flags;

	if (!p)
		return;
	spin_lock_irqsave(&g_wxshadow.lock, flags);
	if (!p->dead) {
		int r = wx_write_pte_locked(p, wx_pte_shadow(p), true);
		if (r != 0)
			ls_log_always_tag("wxshadow", "follow_page_pte restore failed ret=%d addr=%lx\n",
					  r, p->page_addr);
	}
	spin_unlock_irqrestore(&g_wxshadow.lock, flags);
	wx_page_put(p);
}

static int wx_follow_page_pte_work(struct pt_regs *hook_regs)
{
	struct vm_area_struct *vma = (struct vm_area_struct *)(uintptr_t)hook_regs->regs[0];
	unsigned long address = hook_regs->regs[1];
	unsigned int flags = (unsigned int)hook_regs->regs[3];
	struct wxshadow_page *p;
	struct mm_struct *mm;
	unsigned long irq;
	void *meta;
	bool swapped = false;

	if (!vma || (flags & FOLL_WRITE))
		return 0;
	mm = vma->vm_mm;
	if (!mm)
		return 0;

	p = wx_find_page(mm, untagged_addr(address));
	if (!p)
		return 0;

	spin_lock_irqsave(&g_wxshadow.lock, irq);
	if (!p->dead && p->state == WX_STATE_SHADOW_X) {
		/* 临时切回原始页, 不刷 TLB (目标进程仍经 TLB 执行 shadow) */
		swapped = (wx_write_pte_locked(p, wx_pte_original_read(p), false) == 0);
	}
	spin_unlock_irqrestore(&g_wxshadow.lock, irq);

	if (!swapped) {
		wx_page_put(p);
		return 0;
	}

	/* 经 metadata 传递状态并安装返回跳板 */
	meta = hook_frame_metadata(hook_regs);
	*(unsigned long *)((char *)meta + 16) = (unsigned long)p;
	*(unsigned long *)meta = hook_regs->regs[30];
	hook_regs->regs[30] = (unsigned long)wx_ret_trampoline_follow_page_pte;
	hook_regs->sp = (unsigned long)meta;
	return 0;
}

/* --- dup_mmap(mm, oldmm) fork 保护 --- */

static void __attribute__((used, __noinline__)) wx_dup_mmap_return_work(void *meta);

__attribute__((naked, used)) void wx_ret_trampoline_dup_mmap(void)
{
	asm volatile("str x0, [sp, #8]\n"
		     "mov x0, sp\n"
		     "bl wx_dup_mmap_return_work\n"
		     "ldp x16, x0, [sp], #304\n"
		     "ret x16\n");
}

static void __attribute__((used, __noinline__)) wx_dup_mmap_return_work(void *meta)
{
	struct mm_struct *oldmm = *(struct mm_struct **)((char *)meta + 16);

	if (oldmm)
		wx_resume_pages_for_mm(oldmm);
}

static int wx_dup_mmap_work(struct pt_regs *hook_regs)
{
	struct mm_struct *oldmm = (struct mm_struct *)(uintptr_t)hook_regs->regs[1];
	void *meta;

	if (!oldmm)
		return 0;
	if (wx_pause_pages_for_mm(oldmm) <= 0)
		return 0;

	meta = hook_frame_metadata(hook_regs);
	*(unsigned long *)((char *)meta + 16) = (unsigned long)oldmm;
	*(unsigned long *)meta = hook_regs->regs[30];
	hook_regs->regs[30] = (unsigned long)wx_ret_trampoline_dup_mmap;
	hook_regs->sp = (unsigned long)meta;
	return 0;
}

/* ============================ Hook 懒安装/卸载 ============================ */

static int wx_install_hooks(void)
{
	int ret;

	if (g_wxshadow.hooks_installed)
		return 0;

	g_wxshadow.hook_mem_abort = (struct hook_entry)HOOK_ENTRY("do_mem_abort", wx_mem_abort_work);
	g_wxshadow.hook_exit_mmap = (struct hook_entry)HOOK_ENTRY("exit_mmap", wx_exit_mmap_work);

	ret = hook_entry_install(&g_wxshadow.hook_mem_abort);
	if (ret) {
		ls_log_always_tag("wxshadow", "install do_mem_abort failed: %d\n", ret);
		return ret;
	}
	ret = hook_entry_install(&g_wxshadow.hook_exit_mmap);
	if (ret) {
		hook_entry_remove(&g_wxshadow.hook_mem_abort);
		ls_log_always_tag("wxshadow", "install exit_mmap failed: %d\n", ret);
		return ret;
	}

	g_wxshadow.hook_follow_page_pte = (struct hook_entry)HOOK_ENTRY("follow_page_pte", wx_follow_page_pte_work);
	if (hook_entry_install(&g_wxshadow.hook_follow_page_pte) == 0) {
		g_wxshadow.follow_page_pte_enabled = true;
		ls_log_always_tag("wxshadow", "follow_page_pte GUP hide enabled\n");
	} else {
		g_wxshadow.follow_page_pte_enabled = false;
		ls_log_always_tag("wxshadow", "follow_page_pte unavailable, GUP hide disabled\n");
	}

	g_wxshadow.hook_dup_mmap = (struct hook_entry)HOOK_ENTRY("dup_mmap", wx_dup_mmap_work);
	if (hook_entry_install(&g_wxshadow.hook_dup_mmap) == 0) {
		g_wxshadow.dup_mmap_enabled = true;
		ls_log_always_tag("wxshadow", "dup_mmap fork protect enabled\n");
	} else {
		g_wxshadow.dup_mmap_enabled = false;
		ls_log_always_tag("wxshadow", "dup_mmap unavailable, fork protect disabled\n");
	}

	g_wxshadow.hooks_installed = true;
	return 0;
}

static void wx_remove_hooks(void)
{
	if (!g_wxshadow.hooks_installed)
		return;

	if (g_wxshadow.dup_mmap_enabled) {
		hook_entry_remove(&g_wxshadow.hook_dup_mmap);
		g_wxshadow.dup_mmap_enabled = false;
	}
	if (g_wxshadow.follow_page_pte_enabled) {
		hook_entry_remove(&g_wxshadow.hook_follow_page_pte);
		g_wxshadow.follow_page_pte_enabled = false;
	}
	hook_entry_remove(&g_wxshadow.hook_exit_mmap);
	hook_entry_remove(&g_wxshadow.hook_mem_abort);
	g_wxshadow.hooks_installed = false;

	wx_unregister_mmu_notifier();
}

/* ============================ 管理命令 ============================ */

static int wx_patch(struct ls_wxshadow_message *msg)
{
	struct mm_struct *mm;
	struct wxshadow_page *p;
	unsigned long addr = untagged_addr(msg->addr);
	unsigned long page_addr = addr & PAGE_MASK;
	unsigned long offset = addr & ~PAGE_MASK;
	unsigned long len = msg->len;
	void *data = NULL;
	bool created = false;
	int ret = 0;
	int i, free_idx;
	unsigned long flags_lock;
	struct wxshadow_patch *slot;

	if (!len || len > LS_WXSHADOW_PATCH_MAX || offset + len > PAGE_SIZE) {
		ls_log_always_tag("wxshadow", "patch invalid len=%lu offset=%lu\n", len, offset);
		return -EINVAL;
	}

	mm = get_mm_by_pid((pid_t)msg->tgid);
	if (!mm)
		return -ESRCH;

	/* 复制用户态补丁数据 */
	data = kmalloc(len, GFP_KERNEL);
	if (!data) {
		mmput(mm);
		return -ENOMEM;
	}
	__builtin_memcpy(data, msg->u.patch, len);

	p = wx_find_page(mm, page_addr);
	if (!p) {
		/* 新建页: wx_create_page 内部 mmgrab 保活 mm_struct; 本函数的 mm_users 引用继续持有到 out */
		p = wx_create_page(mm, msg->tgid, page_addr);
		if (IS_ERR(p)) {
			ret = PTR_ERR(p);
			goto out;
		}
		created = true;

		/* 注册 mmu_notifier (仅首个 mm, best-effort; 失败仅降级 mprotect 检测, 不阻断 patch) */
		if (!g_wxshadow.mmu_notifier_registered) {
			ret = wx_register_mmu_notifier(mm);
			if (ret < 0)
				ls_log_always_tag("wxshadow", "mmu_notifier register failed: %d\n", ret);
			ret = 0;
		}
	}

	/* upsert patch 记录 (同一 offset 覆盖) */
	slot = NULL;
	free_idx = -1;
	for (i = 0; i < p->nr_patches; i++) {
		if (p->patches[i].data && p->patches[i].offset == offset) {
			slot = &p->patches[i];
			break;
		}
		if (free_idx < 0 && !p->patches[i].data)
			free_idx = i;
	}
	if (!slot) {
		if (free_idx < 0) {
			if (p->nr_patches >= WXSHADOW_MAX_PATCHES) {
				ret = -ENOSPC;
				if (created) {
					wx_teardown_page(p);
					wx_page_put(p);
				} else {
					wx_page_put(p);
				}
				goto out;
			}
			free_idx = p->nr_patches++;
		}
		slot = &p->patches[free_idx];
	}
	if (slot->data)
		kfree(slot->data);
	slot->offset = (u16)offset;
	slot->len = (u16)len;
	slot->data = data;
	data = NULL;

	/* 重建 shadow 并激活 (--x) */
	wx_rebuild_shadow(p);
	spin_lock_irqsave(&g_wxshadow.lock, flags_lock);
	if (!p->dead && p->state != WX_STATE_SHADOW_X) {
		ret = wx_write_pte_locked(p, wx_pte_shadow(p), true);
		if (ret == 0)
			p->state = WX_STATE_SHADOW_X;
	}
	spin_unlock_irqrestore(&g_wxshadow.lock, flags_lock);

	wx_page_put(p);

out:
	kfree(data);
	mmput(mm); /* 释放 mm_users (页面已 mmgrab 持有 mm_count) */
	return ret;
}

static int wx_release(struct ls_wxshadow_message *msg)
{
	struct mm_struct *mm;
	struct wxshadow_page *p;
	unsigned long addr = untagged_addr(msg->addr);

	switch (msg->scope) {
	case LS_WXSHADOW_REL_ALL:
		wx_teardown_all();
		break;
	case LS_WXSHADOW_REL_TGID:
		mm = get_mm_by_pid((pid_t)msg->tgid);
		if (mm) {
			wx_teardown_pages_for_mm(mm);
			mmput(mm);
		}
		break;
	case LS_WXSHADOW_REL_ADDR:
	default:
		mm = get_mm_by_pid((pid_t)msg->tgid);
		if (!mm)
			return -ESRCH;
		p = wx_find_page(mm, addr);
		if (p) {
			unsigned long offset = addr & ~PAGE_MASK;
			int i, remaining = 0;

			for (i = 0; i < p->nr_patches; i++) {
				if (p->patches[i].data && p->patches[i].offset == offset) {
					kfree(p->patches[i].data);
					p->patches[i].data = NULL;
					p->patches[i].len = 0;
				}
				if (p->patches[i].data)
					remaining++;
			}
			if (remaining == 0) {
				wx_teardown_page(p);
			} else {
				unsigned long flags_lock;
				wx_rebuild_shadow(p);
				spin_lock_irqsave(&g_wxshadow.lock, flags_lock);
				if (!p->dead && wx_write_pte_locked(p, wx_pte_shadow(p), true) == 0)
					p->state = WX_STATE_SHADOW_X;
				spin_unlock_irqrestore(&g_wxshadow.lock, flags_lock);
			}
			wx_page_put(p);
		}
		mmput(mm);
		break;
	}

	if (list_empty(&g_wxshadow.pages))
		wx_remove_hooks();
	return 0;
}

static int wx_list(struct ls_wxshadow_message *msg)
{
	struct wxshadow_page *p;
	unsigned long flags;
	uint32_t n = 0;

	msg->page_count = 0;
	msg->list_count = 0;

	spin_lock_irqsave(&g_wxshadow.lock, flags);
	list_for_each_entry(p, &g_wxshadow.pages, list) {
		msg->page_count++;
		if (n < LS_WXSHADOW_LIST_MAX) {
			msg->u.list[n].tgid = (uint32_t)p->tgid;
			msg->u.list[n].state = (uint32_t)p->state;
			msg->u.list[n].patch_count = (uint32_t)p->nr_patches;
			msg->u.list[n].page_addr = p->page_addr;
			n++;
		}
	}
	spin_unlock_irqrestore(&g_wxshadow.lock, flags);

	msg->list_count = n;
	return 0;
}

/* ============================ 对外入口 ============================ */

static void wxshadow_set_enabled(bool enabled);

/*
 * 处理 request_op_wxshadow。msg 指向 req->vmemrw_info.user_buffer (>= 4096 字节)。
 * 返回 0 或 -errno。
 */
static int wxshadow_dispatch(struct ls_wxshadow_message *msg)
{
	uint32_t caps = LS_WXSHADOW_CAP_PATCH | LS_WXSHADOW_CAP_READ_HIDE;
	int ret;

	if (msg->magic != LS_WXSHADOW_MAGIC || msg->abi != LS_WXSHADOW_ABI)
		return -EPROTO;

	/* 开关命令必须在 enabled 检查之前处理, 否则关闭后无法再开启 */
	if (msg->command == LS_WXSHADOW_ENABLE) {
		wxshadow_set_enabled(msg->flags != 0);
		msg->status = 0;
		return 0;
	}

	if (!g_wxshadow_enabled)
		return -EOPNOTSUPP;

	if (g_wxshadow.follow_page_pte_enabled)
		caps |= LS_WXSHADOW_CAP_GUP_HIDE;
	if (g_wxshadow.dup_mmap_enabled)
		caps |= LS_WXSHADOW_CAP_FORK_SAFE;

	mutex_lock(&g_wxshadow.mutex);

	switch (msg->command) {
	case LS_WXSHADOW_INFO:
		msg->flags = caps;
		msg->page_size = PAGE_SIZE;
		ret = 0;
		break;
	case LS_WXSHADOW_PATCH:
		ret = wx_install_hooks();
		if (ret == 0)
			ret = wx_patch(msg);
		break;
	case LS_WXSHADOW_RELEASE:
		ret = wx_release(msg);
		break;
	case LS_WXSHADOW_LIST:
		ret = wx_list(msg);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	msg->status = ret;
	mutex_unlock(&g_wxshadow.mutex);
	return ret;
}

/* 目标退出 / LS 断开时全量清理 (在 taskstats_exit_hook_work 调用)。 */
static void wxshadow_release_all(void)
{
	mutex_lock(&g_wxshadow.mutex);
	wx_teardown_all();
	wx_remove_hooks();
	mutex_unlock(&g_wxshadow.mutex);
}

/* 切换开关: 关掉时立即撤销所有受管页并卸 hook, 回到旧方案。 */
static void wxshadow_set_enabled(bool enabled)
{
	g_wxshadow_enabled = enabled;
	if (!enabled)
		wxshadow_release_all();
	ls_log_always_tag("wxshadow", "switch: %s\n", enabled ? "on" : "off");
}

#endif /* ARM64_WXSHADOW_H */
