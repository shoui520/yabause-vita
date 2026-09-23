/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Executor contracts for future native execution. These are C68K regression
 * checks, not a claim that its complete timing model matches real hardware. */
#include "../src/core/c68k/c68k.h"
#include <assert.h>
#include <stdio.h>
#ifdef C68K_NATIVE_REGIONS
extern void C68kNativeTestReset(void);
extern unsigned C68kNativeTestHits(void);
extern unsigned C68kNativeTestCalls(void);
#endif

static c68k_struc cpu;
static u16 ram[0x80000 / 2];
static unsigned writes, interrupts;
static s32 seen_remaining, seen_done;
static s32 expected_budget;
enum action { NONE, ADD_CYCLES, RELEASE, IRQ };
static enum action action;

static u32 FASTCALL read_word(u32 address)
{
    assert(address < sizeof(ram) && !(address & 1));
    return ram[address / 2];
}
static u32 FASTCALL read_byte(u32 address)
{
    assert(address < sizeof(ram));
    return (ram[address / 2] >> ((address & 1) ? 0 : 8)) & 255;
}
static void FASTCALL write_word(u32 address, u32 value)
{
    assert(address < sizeof(ram) && !(address & 1));
    ram[address / 2] = value;
    if (address != 0x400) return; /* Exception stack writes are ordinary RAM. */
    ++writes;
    seen_remaining = C68k_Get_CycleRemaining(&cpu);
    seen_done = C68k_Get_CycleDone(&cpu);
    assert(C68k_Get_CycleToDo(&cpu) == expected_budget);
    if (action == ADD_CYCLES) C68k_Add_Cycle(&cpu, 7);
    if (action == RELEASE) C68k_Release_Cycle(&cpu);
    if (action == IRQ) C68k_Set_IRQ(&cpu, 3);
}
static void FASTCALL write_byte(u32 address, u32 value)
{
    assert(address < sizeof(ram));
    unsigned shift = (address & 1) ? 0 : 8;
    ram[address / 2] = (ram[address / 2] & ~(255u << shift)) |
                         ((value & 255) << shift);
}
static s32 FASTCALL interrupt_ack(s32 level)
{
    assert(level == 3);
    ++interrupts;
    return C68K_INT_ACK_AUTOVECTOR;
}
static void prepare(enum action next)
{
    memset(ram, 0, sizeof(ram));
    C68k_Init(&cpu, interrupt_ack);
    C68k_Set_ReadB(&cpu, read_byte);
    C68k_Set_ReadW(&cpu, read_word);
    C68k_Set_WriteB(&cpu, write_byte);
    C68k_Set_WriteW(&cpu, write_word);
    C68k_Set_Fetch(&cpu, 0, sizeof(ram) - 1, (pointer)ram);
    C68k_Set_PC(&cpu, 0x100);
    C68k_Set_SR(&cpu, 0x2000);
    cpu.A[0] = 0x400;
    cpu.A[7] = 0x70000;
    ram[0x100 / 2] = 0x702a; /* MOVEQ #42,D0: 4 cycles */
    ram[0x102 / 2] = 0x4e71; /* NOP: 4 */
    ram[0x104 / 2] = 0x3080; /* MOVE.W D0,(A0): 8 */
    ram[0x106 / 2] = 0x7207; /* Must not execute in these budget tests. */
    ram[(27 * 4) / 2 + 1] = 0x200; /* Level 3 autovector. */
    ram[0x200 / 2] = 0x4e71;
    writes = interrupts = 0;
    seen_remaining = seen_done = -999;
    action = next;
    expected_budget = 16;
}
int main(void)
{
#ifdef C68K_NATIVE_REGIONS
    C68kNativeTestReset();
#endif
    const s32 expected[] = {16, 23, 24, 60};
    for (unsigned mode = NONE; mode <= IRQ; ++mode) {
        prepare((enum action)mode);
        s32 elapsed = C68k_Exec(&cpu, 16);
        assert(elapsed == expected[mode]);
        assert(writes == 1 && ram[0x400 / 2] == 42);
        assert(seen_remaining == 8 && seen_done == 8);
        assert(cpu.D[0] == 42 && cpu.D[1] == 0);
        assert(!(cpu.Status & C68K_RUNNING));
        assert(C68k_Get_CycleRemaining(&cpu) == -1);
        assert(interrupts == (mode == IRQ));
        assert(C68k_Get_PC(&cpu) == (mode == IRQ ? 0x200u : 0x106u));
        if (mode == IRQ) {
            assert(cpu.A[7] == 0x70000 - 6 && cpu.flag_I == 3);
            assert(read_word(cpu.A[7]) == 0x2000);
            assert(read_word(cpu.A[7] + 4) == 0x106);
        }
    }
    prepare(NONE);
    assert(C68k_Exec(&cpu, 4) == 4);
    assert(cpu.D[0] == 42 && C68k_Get_PC(&cpu) == 0x102);
    assert(writes == 0);
#ifdef C68K_NATIVE_REGIONS
    assert(C68kNativeTestHits() == 4); /* Each full-budget case ran MOVEQ+NOP natively. */
#endif
    // Native -> interpreted memory callback -> native, preserving the private
    // executor PC even though the canonical CPU->PC can lag during fallback.
    prepare(NONE);
    ram[0x108 / 2] = 0x4e71;
    expected_budget = 24;
    assert(C68k_Exec(&cpu, expected_budget) == 24);
    assert(cpu.D[1] == 7 && C68k_Get_PC(&cpu) == 0x10a);
    assert(writes == 1 && seen_remaining == 16 && seen_done == 8);
    // Add an interpreted control-flow boundary; the skipped MOVEQ must not
    // enter the next native unit through stale canonical PC state.
    prepare(NONE);
    ram[0x106 / 2] = 0x6002; /* BRA.S to 0x10a */
    ram[0x108 / 2] = 0x727f; /* skipped */
    ram[0x10a / 2] = 0x7207;
    ram[0x10c / 2] = 0x4e71;
    expected_budget = 34;
    assert(C68k_Exec(&cpu, expected_budget) == 34);
    assert(cpu.D[1] == 7 && C68k_Get_PC(&cpu) == 0x10e);
    assert(writes == 1 && seen_remaining == 26 && seen_done == 8);
#ifdef C68K_NATIVE_REGIONS
    assert(C68kNativeTestHits() == 8);
#ifndef C68K_NO_JUMP_TABLE
    prepare(NONE);
    ram[0x100 / 2] = 0x60fe; /* Unsupported BRA.S to itself. */
    C68kNativeTestReset();
    assert(C68k_Exec(&cpu, 100) == 100);
    assert(C68k_Get_PC(&cpu) == 0x100);
    assert(C68kNativeTestCalls() == 0);
#endif
#endif
    puts("C68K executor: budgets, callback cycle observation/modification, release and IRQ entry passed");
}
