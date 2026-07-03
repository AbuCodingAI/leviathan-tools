#include "citron.h"
#include <string.h>
#include <stdio.h>

int breakpoint_add(debugger_t *dbg, uint64_t addr) {
    if (dbg->breakpoints.count >= MAX_BREAKPOINTS) {
        warn("Maximum breakpoints (%d) reached", MAX_BREAKPOINTS);
        return -1;
    }

    /* Check if already exists */
    if (breakpoint_find_by_addr(dbg, addr)) {
        warn("Breakpoint already exists at 0x%lx", addr);
        return -1;
    }

    /* Read original byte at breakpoint location */
    uint8_t orig_byte;
    if (ptrace_read_mem(dbg->proc.pid, addr, &orig_byte, 1) < 0) {
        warn("Could not read memory at 0x%lx", addr);
        return -1;
    }

    /* Write INT3 (0xcc) instruction */
    uint8_t int3 = 0xcc;
    if (ptrace_write_mem(dbg->proc.pid, addr, &int3, 1) < 0) {
        warn("Could not write breakpoint at 0x%lx", addr);
        return -1;
    }

    /* Add to list */
    breakpoint_t *bp = &dbg->breakpoints.bps[dbg->breakpoints.count];
    bp->id = ++dbg->breakpoints.next_id;
    bp->addr = addr;
    bp->original_byte = orig_byte;
    bp->enabled = 1;
    dbg->breakpoints.count++;

    info("Breakpoint %u set at 0x%lx", bp->id, addr);
    return bp->id;
}

int breakpoint_delete(debugger_t *dbg, uint32_t id) {
    breakpoint_t *bp = breakpoint_find_by_id(dbg, id);
    if (!bp) {
        warn("Breakpoint %u not found", id);
        return -1;
    }

    /* Restore original byte */
    if (ptrace_write_mem(dbg->proc.pid, bp->addr, &bp->original_byte, 1) < 0) {
        warn("Could not restore original byte at 0x%lx", bp->addr);
        return -1;
    }

    /* Remove from list */
    size_t idx = bp - dbg->breakpoints.bps;
    memmove(bp, bp + 1,
            (dbg->breakpoints.count - idx - 1) * sizeof(breakpoint_t));
    dbg->breakpoints.count--;

    info("Breakpoint %u deleted", id);
    return 0;
}

int breakpoint_enable(debugger_t *dbg, uint32_t id) {
    breakpoint_t *bp = breakpoint_find_by_id(dbg, id);
    if (!bp) {
        warn("Breakpoint %u not found", id);
        return -1;
    }

    if (bp->enabled) {
        warn("Breakpoint %u already enabled", id);
        return 0;
    }

    uint8_t int3 = 0xcc;
    if (ptrace_write_mem(dbg->proc.pid, bp->addr, &int3, 1) < 0) {
        warn("Could not enable breakpoint at 0x%lx", bp->addr);
        return -1;
    }

    bp->enabled = 1;
    info("Breakpoint %u enabled", id);
    return 0;
}

int breakpoint_disable(debugger_t *dbg, uint32_t id) {
    breakpoint_t *bp = breakpoint_find_by_id(dbg, id);
    if (!bp) {
        warn("Breakpoint %u not found", id);
        return -1;
    }

    if (!bp->enabled) {
        warn("Breakpoint %u already disabled", id);
        return 0;
    }

    if (ptrace_write_mem(dbg->proc.pid, bp->addr, &bp->original_byte, 1) < 0) {
        warn("Could not disable breakpoint at 0x%lx", bp->addr);
        return -1;
    }

    bp->enabled = 0;
    info("Breakpoint %u disabled", id);
    return 0;
}

breakpoint_t *breakpoint_find_by_addr(debugger_t *dbg, uint64_t addr) {
    for (size_t i = 0; i < dbg->breakpoints.count; i++) {
        if (dbg->breakpoints.bps[i].addr == addr) {
            return &dbg->breakpoints.bps[i];
        }
    }
    return NULL;
}

breakpoint_t *breakpoint_find_by_id(debugger_t *dbg, uint32_t id) {
    for (size_t i = 0; i < dbg->breakpoints.count; i++) {
        if (dbg->breakpoints.bps[i].id == id) {
            return &dbg->breakpoints.bps[i];
        }
    }
    return NULL;
}

/* Evaluate a conditional breakpoint's predicate against live registers.
   Returns 1 if the condition holds (or the breakpoint is unconditional),
   0 if it does not hold (so the caller should auto-continue). */
int breakpoint_cond_met(debugger_t *dbg, breakpoint_t *bp) {
    if (bp->cond_op == COND_NONE) return 1;
    uint64_t lhs = ptrace_get_reg(dbg->proc.pid, bp->cond_reg);
    uint64_t rhs = bp->cond_val;
    switch (bp->cond_op) {
    case COND_EQ: return lhs == rhs;
    case COND_NE: return lhs != rhs;
    case COND_LT: return lhs <  rhs;
    case COND_GT: return lhs >  rhs;
    case COND_LE: return lhs <= rhs;
    case COND_GE: return lhs >= rhs;
    default:      return 1;
    }
}

void breakpoint_list(debugger_t *dbg) {
    if (dbg->breakpoints.count == 0) {
        info("No breakpoints set");
        return;
    }

    printf("ID   Address          Status    Hits  Condition\n");
    printf("--   -------          ------    ----  ---------\n");

    for (size_t i = 0; i < dbg->breakpoints.count; i++) {
        breakpoint_t *bp = &dbg->breakpoints.bps[i];
        printf("%-4u 0x%014lx   %-8s  %-4lu  %s\n", bp->id, bp->addr,
               bp->enabled ? "enabled" : "disabled",
               (unsigned long)bp->hit_count,
               bp->cond_op != COND_NONE ? bp->cond_str : "-");
    }
}
