/*
 *  x86 misc helpers
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "hw/core/cpu.h"   /* cpu_memory_rw_debug */
#include "exec/helper-proto.h"
#include "exec/cputlb.h"
#include "exec/target_page.h" /* TARGET_PAGE_MASK/TARGET_PAGE_SIZE (fi_lin_to_phys) */
#include "helper-tcg.h"

/*
 * NOTE: the translator must set DisasContext.cc_op to CC_OP_EFLAGS
 * after generating a call to a helper that uses this.
 */
void cpu_load_eflags(CPUX86State *env, int eflags, int update_mask)
{
    CC_SRC = eflags & (CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C);
    CC_OP = CC_OP_EFLAGS;
    env->df = 1 - (2 * ((eflags >> 10) & 1));
    env->eflags = (env->eflags & ~update_mask) |
        (eflags & update_mask) | 0x2;
}

void helper_into(CPUX86State *env, int next_eip_addend)
{
    int eflags;

    eflags = cpu_cc_compute_all(env);
    if (eflags & CC_O) {
        raise_interrupt(env, EXCP04_INTO, next_eip_addend);
    }
}

void helper_cpuid(CPUX86State *env)
{
    uint32_t eax, ebx, ecx, edx;

    cpu_svm_check_intercept_param(env, SVM_EXIT_CPUID, 0, GETPC());

    cpu_x86_cpuid(env, (uint32_t)env->regs[R_EAX], (uint32_t)env->regs[R_ECX],
                  &eax, &ebx, &ecx, &edx);
    env->regs[R_EAX] = eax;
    env->regs[R_EBX] = ebx;
    env->regs[R_ECX] = ecx;
    env->regs[R_EDX] = edx;
}

void helper_rdtsc(CPUX86State *env)
{
    uint64_t val;

    if ((env->cr[4] & CR4_TSD_MASK) && ((env->hflags & HF_CPL_MASK) != 0)) {
        raise_exception_ra(env, EXCP0D_GPF, GETPC());
    }
    cpu_svm_check_intercept_param(env, SVM_EXIT_RDTSC, 0, GETPC());

    val = cpu_get_tsc(env) + env->tsc_offset;
    env->regs[R_EAX] = (uint32_t)(val);
    env->regs[R_EDX] = (uint32_t)(val >> 32);
}

G_NORETURN void helper_rdpmc(CPUX86State *env)
{
    if (((env->cr[4] & CR4_PCE_MASK) == 0 ) &&
        ((env->hflags & HF_CPL_MASK) != 0)) {
        raise_exception_ra(env, EXCP0D_GPF, GETPC());
    }
    cpu_svm_check_intercept_param(env, SVM_EXIT_RDPMC, 0, GETPC());

    /* currently unimplemented */
    qemu_log_mask(LOG_UNIMP, "x86: unimplemented rdpmc\n");
    raise_exception_err(env, EXCP06_ILLOP, 0);
}

G_NORETURN void helper_pause(CPUX86State *env)
{
    CPUState *cs = env_cpu(env);

    /* Do gen_eob() tasks before going back to the main loop.  */
    do_end_instruction(env);
    helper_rechecking_single_step(env);

    /* Just let another CPU run.  */
    cs->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit(cs);
}

uint64_t helper_rdpkru(CPUX86State *env, uint32_t ecx)
{
    if ((env->cr[4] & CR4_PKE_MASK) == 0) {
        raise_exception_err_ra(env, EXCP06_ILLOP, 0, GETPC());
    }
    if (ecx != 0) {
        raise_exception_err_ra(env, EXCP0D_GPF, 0, GETPC());
    }

    return env->pkru;
}

void helper_wrpkru(CPUX86State *env, uint32_t ecx, uint64_t val)
{
    CPUState *cs = env_cpu(env);

    if ((env->cr[4] & CR4_PKE_MASK) == 0) {
        raise_exception_err_ra(env, EXCP06_ILLOP, 0, GETPC());
    }
    if (ecx != 0 || (val & 0xFFFFFFFF00000000ull)) {
        raise_exception_err_ra(env, EXCP0D_GPF, 0, GETPC());
    }

    env->pkru = val;
    tlb_flush(cs);
}

target_ulong HELPER(rdpid)(CPUX86State *env)
{
#if !defined CONFIG_USER_ONLY
    return env->tsc_aux;
#elif defined CONFIG_LINUX && defined CONFIG_GETCPU
    unsigned cpu, node;
    getcpu(&cpu, &node);
    return (node << 12) | (cpu & 0xfff);
#elif defined CONFIG_SCHED_GETCPU
    return sched_getcpu();
#else
    return 0;
#endif
}

/* xemu: guest call tracing (see xemu-calltrace.c) */
void xemu_calltrace_record(uint32_t call_site, uint32_t callee);

void HELPER(xemu_calltrace_call)(target_ulong call_site, target_ulong callee)
{
    xemu_calltrace_record((uint32_t)call_site, (uint32_t)callee);
}

/* Non-faulting guest read: returns 0 (never raises) on an unmapped page,
 * so tracing can't perturb guest execution. */
static uint32_t ct_safe_ldl(CPUX86State *env, uint32_t va)
{
    uint32_t v = 0;
    if (cpu_memory_rw_debug(env_cpu(env), va, &v, 4, false) != 0) {
        return 0;
    }
    return v; /* guest and host are little-endian */
}

void xemu_calltrace_record_data(uint32_t call_site, uint32_t callee,
                                const uint32_t args[6]);

void HELPER(xemu_calltrace_data)(CPUX86State *env, target_ulong call_site,
                                 target_ulong callee)
{
    uint32_t esp = (uint32_t)env->regs[R_ESP];
    uint32_t a[6];
    a[0] = (uint32_t)env->regs[R_ECX];
    a[1] = (uint32_t)env->regs[R_EDX];
    for (int k = 0; k < 4; k++) {
        a[2 + k] = ct_safe_ldl(env, esp + 4u * (uint32_t)k);
    }
    xemu_calltrace_record_data((uint32_t)call_site, (uint32_t)callee, a);
}

/* xemu frame inspector (see xemu-frameinspect.c) */
#include "../../../xemu-frameinspect.h"

/*
 * Guest thread key: current KTHREAD pointer at KPCR.PrcbData.CurrentThread
 * = fs:[0x28] (fixed Xbox kernel ABI). Read non-faulting and cached;
 * refreshed at every CALL/RET, which brackets thread switches closely
 * enough for best-effort attribution (stores between a switch and the
 * next CALL/RET may attribute to the previous thread's path).
 */
static uint32_t fi_cached_thread_key;

static uint32_t fi_thread_key(CPUX86State *env)
{
    uint32_t key = ct_safe_ldl(env, (uint32_t)env->segs[R_FS].base + 0x28);
    if (key != 0) {
        fi_cached_thread_key = key;
    }
    return fi_cached_thread_key;
}

/* Used by the store helpers (Tasks 7/8), which live in this same file —
 * keep it static so no prototype is needed. */
static uint32_t fi_thread_key_cached(void)
{
    return fi_cached_thread_key;
}

void HELPER(xemu_fi_call)(CPUX86State *env, target_ulong call_site,
                          target_ulong callee)
{
    uint32_t key = fi_thread_key(env);
    uint32_t esp = (uint32_t)env->regs[R_ESP];
    uint32_t args[6];
    args[0] = (uint32_t)env->regs[R_ECX];
    args[1] = (uint32_t)env->regs[R_EDX];
    for (int k = 0; k < 4; k++) {
        args[2 + k] = ct_safe_ldl(env, esp + 4u * (uint32_t)k);
    }
    /* call_site holds next-EIP: the address the CALL pushes. It both
     * identifies the call site and is the RET-match key (see the design
     * note under Step 3). */
    uint32_t ret_addr = (uint32_t)call_site;
    xemu_frameinspect_record_call(key, ret_addr, (uint32_t)callee,
                                  ret_addr, esp, args);
    if (xemu_frameinspect_callee_watched((uint32_t)callee)) {
        uint32_t regs[8];
        for (int r = 0; r < 8; r++) {
            regs[r] = (uint32_t)env->regs[r];
        }
        uint32_t stack16[16];
        for (int k = 0; k < 16; k++) {
            stack16[k] = ct_safe_ldl(env, esp + 4u * (uint32_t)k);
        }
        xemu_frameinspect_record_call_watched(key, (uint32_t)callee, regs,
                                              esp, stack16);
    }
}

void HELPER(xemu_fi_ret)(CPUX86State *env, target_ulong ret_target)
{
    uint32_t key = fi_thread_key(env);
    xemu_frameinspect_record_ret(key, (uint32_t)ret_target,
                                 (uint32_t)env->regs[R_EAX]);
}

/*
 * Post-store tagging: runs only after the store retired (the helper call
 * is emitted after the qemu_st op, so a faulting store unwinds first and
 * never reaches it) — tags are published only for successful stores.
 * Linear→physical goes through a 1-entry page cache; misses walk the
 * page tables non-faulting. The cache is keyed on (page, CR3) so a
 * guest address-space switch (CR3 reload) can't serve a stale physical
 * page under the new tables — worst case is a cache miss, never a wrong
 * tag. Residual limitation: a PTE modified in place without a CR3
 * reload can still leave one stale entry for that page (accepted for a
 * debug tool; cpu_get_phys_page_debug walks the current tables on every
 * miss, so this only affects the single cached entry, not new lookups).
 */
static struct { uint32_t page; uint32_t cr3; uint64_t phys_page; bool valid; } fi_tlb1;

static bool fi_lin_to_phys(CPUX86State *env, uint32_t lin, uint64_t *phys)
{
    uint32_t page = lin & TARGET_PAGE_MASK;
    uint32_t cr3 = (uint32_t)env->cr[3];
    if (!fi_tlb1.valid || fi_tlb1.page != page || fi_tlb1.cr3 != cr3) {
        hwaddr p = cpu_get_phys_page_debug(env_cpu(env), page);
        if (p == -1) {
            return false;
        }
        fi_tlb1.page = page;
        fi_tlb1.cr3 = cr3;
        fi_tlb1.phys_page = p;
        fi_tlb1.valid = true;
    }
    *phys = fi_tlb1.phys_page | (lin & ~TARGET_PAGE_MASK);
    return true;
}

void HELPER(xemu_fi_store_post)(CPUX86State *env, target_ulong addr,
                                uint32_t size)
{
    uint32_t lin = (uint32_t)addr;
    uint32_t done = 0;
    while (done < size) {             /* split page-crossing stores */
        uint32_t chunk = TARGET_PAGE_SIZE - ((lin + done) & ~TARGET_PAGE_MASK);
        if (chunk > size - done) {
            chunk = size - done;
        }
        uint64_t phys;
        if (fi_lin_to_phys(env, lin + done, &phys)) {
            xemu_frameinspect_record_store(fi_thread_key_cached(), phys,
                                           chunk);
        }
        done += chunk;
    }
}

/* Watch mode: the pre-helper stashes the old bytes (single vCPU, so one
 * scratch slot suffices); the post-helper reads the new bytes back from
 * memory and logs old->new. Nothing is published if the store faults
 * between the two (the post-helper never runs; the stash is simply
 * overwritten by the next store). */
static struct {
    uint64_t phys;
    uint32_t size;
    bool is_ram;
    uint8_t old_bytes[8];
} fi_store_stash;

void HELPER(xemu_fi_store_pre)(CPUX86State *env, target_ulong addr,
                               uint32_t size)
{
    uint64_t phys;
    fi_store_stash.size = size;
    fi_store_stash.is_ram = fi_lin_to_phys(env, (uint32_t)addr, &phys);
    if (fi_store_stash.is_ram) {
        fi_store_stash.phys = phys;
        cpu_physical_memory_read(phys, fi_store_stash.old_bytes, size);
    }
}

void HELPER(xemu_fi_store_post_watch)(CPUX86State *env, target_ulong addr,
                                      uint32_t size)
{
    uint8_t new_bytes[8];
    if (fi_store_stash.is_ram && fi_store_stash.size == size) {
        cpu_physical_memory_read(fi_store_stash.phys, new_bytes, size);
        xemu_frameinspect_record_store_watched(fi_thread_key_cached(),
                                               fi_store_stash.phys, size,
                                               fi_store_stash.old_bytes,
                                               new_bytes, true);
    } else {
        xemu_frameinspect_record_store_watched(fi_thread_key_cached(),
                                               (uint32_t)addr, size,
                                               NULL, NULL, false);
    }
}
