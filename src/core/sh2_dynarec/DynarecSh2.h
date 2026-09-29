/*
        Copyright 2019 devMiyax(smiyaxdev@gmail.com)

This file is part of YabaSanshiro.

        YabaSanshiro is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

YabaSanshiro is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
along with YabaSanshiro; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifndef _DYNAREC_SH2_H_
#define _DYNAREC_SH2_H_

#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <vector>
#include <algorithm>

#include <sys/types.h>
#include <stdint.h>
#include <cstddef>

#include "debug.h"
#include "threads.h"

//****************************************************
// Defiens
//****************************************************

// instruction Format
#define ZERO_F  0
#define N_F     1
#define M_F     2
#define NM_F    3
#define MD_F    4
#define ND4_F   5
#define NMD_F   6
#define D_F     7 
#define D12_F   8
#define ND8_F   9
#define I_F     10
#define NI_F    11

#include <cstdlib>
#include <cstring>
#include <new>
unsigned char *VitaSh2CodeArena();

const int MAX_INSTSIZE = 0xFFFF+1;

using std::list;
using std::map;
using std::string;

// Moves whenever a published high-RAM block is removed or replaced (and on
// compiler reset): a read set may stand for a block's own code words by it.
extern u32 g_code_epoch;
#ifdef VITA_SH2_PARENT_LIST
typedef list<u32> addrs;
#else
/* The block start PCs owning one halfword: the std::list operations used on
 * it (push_back, unique, remove, clear, size, forward iteration) with the
 * same order, without a heap node per entry. One entry is held inline; the
 * table holds one of these per halfword of high work RAM. */
class addrs {
  u32 n_ = 0, cap_ = 1;
  union { u32 one_; u32 *heap_; };
  u32 *data() { return cap_ > 1 ? heap_ : &one_; }
  const u32 *data() const { return cap_ > 1 ? heap_ : &one_; }
 public:
  addrs() : one_(0) {}
  ~addrs() { if (cap_ > 1) free(heap_); }
  addrs(const addrs &) = delete;
  addrs &operator=(const addrs &) = delete;
  size_t size() const { return n_; }
  bool empty() const { return n_ == 0; }
  const u32 *begin() const { return data(); }
  const u32 *end() const { return data() + n_; }
  void push_back(u32 v) {
    if (n_ == cap_) {
      const u32 cap = cap_ * 2;
      u32 *p = static_cast<u32 *>(malloc(cap * sizeof(u32)));
      if (!p) abort();
      memcpy(p, data(), n_ * sizeof(u32));
      if (cap_ > 1) free(heap_);
      heap_ = p;
      cap_ = cap;
    }
    data()[n_++] = v;
  }
  // Collapse runs of equal entries to one, as std::list::unique.
  void unique() {
    if (n_ < 2) return;
    u32 *d = data(), k = 1;
    for (u32 i = 1; i < n_; ++i)
      if (d[i] != d[k - 1]) d[k++] = d[i];
    n_ = k;
  }
  // Drop every entry equal to v, keeping the others' order.
  void remove(u32 v) {
    u32 *d = data(), k = 0;
    for (u32 i = 0; i < n_; ++i)
      if (d[i] != v) d[k++] = d[i];
    n_ = k;
  }
  void clear() { n_ = 0; }
};
#endif

struct CompileStaticsNode {
  u32 time;
  u32 count;
  u32 end_addr;
};

typedef map<u32, CompileStaticsNode> MapCompileStatics;

//****************************************************
// Structs
//****************************************************

#include "../../vita/code_arena_layout.h"
const int NUMOFBLOCKS = vitacode::Layout::Sh2Blocks;
//const int MAXBLOCKSIZE = 3072-(4*4);
const int MAXBLOCKSIZE = vitacode::Layout::Block;
#define MAINMEMORY_SIZE (0x100000);
#define ROM_SIZE (0x80000);

struct Block
{
  u8 *code;
  u32 b_addr; //beginning PC
  u32 e_addr; //ending PC
  u32 id;
  u32 flags;
  u32 poll;
  u32 poll_step; // Exact single-iteration recipe; no added wait-skip authority.
  u32 link_pc;   // b_addr while this is LookupTable's entry for it (high RAM), else 0
};

#define BLOCK_LOOP  (0x01)
#define BLOCK_RESIDENT_LOOP (0x04)
#define BLOCK_WRITE (0x02)
#define BLOCK_POLL_FUSED (0x08)
#define BLOCK_MAY_WRITE (0x10) // some instruction may write memory (conservative decode)

#define IN_INFINITY_LOOP (-1)

extern "C" void sh2_block_exit(void);  // dynalib_arm.s: plain block return
extern "C" void sh2_dispatch(void);
extern "C" void sh2_dispatch_end(void);  // end of sh2_dispatch (position independent)
extern "C" void sh2_macw_saturate(void);  // dynalib_arm.s: MAC.W S=1 tail
extern "C" void sh2_macl_saturate(void);  // dynalib_arm.s: MAC.L S=1 tail
extern "C" void sh2_macl_region(void);    // dynalib_arm.s: MAC_L template as a call
extern "C" void sh2_macw_region(void);    // dynalib_arm.s: MAC_W template as a call

// Sh2 Registris
struct tagSH2
{
  u32 GenReg[16];
  u32 CtrlReg[3];
  u32 SysReg[6];
  uintptr_t getmembyte;
  uintptr_t getmemword;
  uintptr_t getmemlong;
  uintptr_t setmembyte;
  uintptr_t setmemword;
  uintptr_t setmemlong;
  uintptr_t eachclock;
  u32 exitcount;
  u32 spec_bail;   // stack speculation: nonzero PC = entry validation failed, nothing executed
  uintptr_t spec_high;   // #136 HighWram (stack speculation entry check)
  uintptr_t spec_pages;  // #140 CompileBlocks::code_pages
  // Native dispatch (VITA_SH2_NATIVE_DISPATCH): every block exit is
  // LDR pc,[r7,#144]. sh2_block_exit returns to C; sh2_dispatch enters the
  // next cached high-RAM block directly while chain_budget allows it and the
  // C dispatcher would do nothing but look that block up.
  uintptr_t dispatch = reinterpret_cast<uintptr_t>(&sh2_block_exit); // #144
  u32 chain_budget = 0;       // #148 further blocks sh2_dispatch may enter
  uintptr_t chain_table = 0;  // #152 CompileBlocks::LookupTable (high RAM)
  uintptr_t chain_cur = 0;    // #156 Block last entered (by C or sh2_dispatch)
  u32 memcycle = 0;             // #160 DynarecSh2::memcycle_ itself (one load from r7)
  uintptr_t macw_saturate = 0;  // #164 sh2_macw_saturate (VITA_SH2_MACW_WRAM)
  uintptr_t macl_saturate = 0;  // #168 sh2_macl_saturate (VITA_SH2_MACL_WRAM)
};

#ifdef VITA
// dynalib_arm.s uses fixed byte offsets, not C++ member access. Reject ABI
// drift at build time rather than corrupting guest state in generated code.
static_assert(sizeof(uintptr_t) == 4, "ARM templates require 32-bit pointers");
static_assert(offsetof(tagSH2, CtrlReg) == 64, "ARM SR/GBR/VBR offsets");
static_assert(offsetof(tagSH2, SysReg) == 76, "ARM MACH/MACL/PR/PC offsets");
static_assert(offsetof(tagSH2, getmembyte) == 100, "ARM memory callback offsets");
static_assert(offsetof(tagSH2, eachclock) == 124, "ARM clock callback offset");
static_assert(offsetof(tagSH2, exitcount) == 128, "ARM exit counter offset");
#endif

// Instruction
struct i_desc
{
  s32 format;
  const char *mnem;
  u16 mask;
  u16 bits;
  u8 dat;
  void (FASTCALL *func)(tagSH2*);
} ;

 
extern i_desc opcode_list[];


inline u32 adress_mask(u32 addr) {
  return (addr & 0x000FFFFF)>>1;
}

// Interrupt Table
struct dIntcTbl
{
  u8 Vector;
  u8 level;
};
#ifdef VITA_SH2_INTC_VECTOR
/* std::list's interface and exact results (stable sort by operator<, level
 * descending; unique drops adjacent equal vectors; order-preserving
 * remove_if) on reserved contiguous storage: no allocation per interrupt. */
bool operator < (const dIntcTbl & data1 , const dIntcTbl & data2 );
bool operator == (const dIntcTbl & data1 , const dIntcTbl & data2 );
class dlstIntct {
  std::vector<dIntcTbl> v_;
public:
  typedef std::vector<dIntcTbl>::iterator iterator;
  dlstIntct() { v_.reserve(32); }
  iterator begin() { return v_.begin(); }
  iterator end() { return v_.end(); }
  size_t size() const { return v_.size(); }
  void clear() { v_.clear(); }
  void push_back(const dIntcTbl &x) { v_.push_back(x); }
  void pop_front() { v_.erase(v_.begin()); }
  template <class Pred> void remove_if(Pred p) { v_.erase(std::remove_if(v_.begin(), v_.end(), p), v_.end()); }
  void sort() {
    for (size_t i = 1; i < v_.size(); ++i) {
      const dIntcTbl x = v_[i];
      size_t j = i;
      for (; j > 0 && x < v_[j - 1]; --j) v_[j] = v_[j - 1];
      v_[j] = x;
    }
  }
  void unique() { v_.erase(std::unique(v_.begin(), v_.end()), v_.end()); }
};
#else
typedef std::list<dIntcTbl> dlstIntct;
#endif


struct x86op_desc
{
  void(*func)();
  const unsigned short *size;
  const unsigned char *src;
  const unsigned char *dest;
  const unsigned char *off1;
  const unsigned char *imm;
  const unsigned char *off3;
  unsigned char delay;
  unsigned char cycle;
  unsigned char write_count;
  unsigned char build_count;

  x86op_desc(void(*ifunc)(), const unsigned short *isize, const unsigned char *isrc,
    const unsigned char *idest, const unsigned char *ioff1, const unsigned char *iimm,
    const unsigned char *ioff3, const unsigned char idelay, const unsigned char icycle, const unsigned char iwrite_count = 0)
  {
    func = ifunc;
    size = isize;
    src = isrc;
    dest = idest;
    off1 = ioff1;
    imm = iimm;
    off3 = ioff3;
    delay = idelay;
    cycle = icycle;
    write_count = iwrite_count;
    build_count = 0;
  };

};

#define SET_DIRTY
extern "C" {
  void DebugLog(const char * format, ...);
}

class CompileBlocks {
private:
  CompileBlocks(){
    debug_mode_ = false;
    BuildInstructionList();
    Init();
#ifdef SET_DIRTY
    LookupParentTable = new addrs[0x100000 >> 1];
#else
    LookupParentTable = NULL;
#endif
    compile_count_ = 0;
    exec_count_ = 0;
    remove_count_ = 0;
  }
  ~CompileBlocks(){
    free(dCode);
  }
  static CompileBlocks * instance_;
  bool show_code_ = false;
public:
  static CompileBlocks *existingInstance() { return instance_; }
  void setShowCode( bool b ){ show_code_ = b; }
  static CompileBlocks * getInstance(){
    if( instance_ == NULL ){
      instance_ = new CompileBlocks();
    }
    return instance_;
  }

  int blockCount;
  int LastMakeBlock; 
  bool debug_mode_;
  Block * g_CompleBlock;
  
  u8 dsh2_instructions[MAX_INSTSIZE];
  Block* LookupTable[0x100000>>1];    
  //addrs LookupParentTable[0x100000>>1];
  addrs * LookupParentTable = nullptr;
  // One byte per KiB of high work RAM: set whenever an owner is added to
  // LookupParentTable in that KiB, cleared only with the whole table. A zero
  // entry proves every setDirty in the KiB is a no-op (guarded region stores).
  u8 code_pages[1024] = {};
  void MarkCode(u32 keep) { code_pages[keep >> 9] = 1; }
  // Dispatch indexes the full 1 MiB ROM address window, including mirrors.
  // Do not collapse aliases: block metadata contains the original guest PC.
  Block* LookupTableRom[0x100000>>1];
  Block* LookupTableLow[0x100000>>1];
  std::array<std::vector<Block*>, 256> low_code_pages;
  void InvalidateLow(u32 address, u32 length);
  // Each SH-2 has its own cache RAM. Keep both CPU identity and full guest PC.
  std::unordered_map<u64, Block*> LookupTableC;
  Block * dCode = nullptr;
  
  std::unordered_map<u32, int> self_modify_block;
  // VITA_SH2_SMC_VARIANTS: high-RAM PCs whose code the guest rewrote keep up
  // to kSmcVariants compiled slots. A slot is published again, in place of a
  // compile, only while every source word the compiler read for it (and the
  // PC's stack-speculation denial) is unchanged, i.e. when a compile now
  // would emit that same code. (Members unconditional: one class layout.)
  struct SmcVariant { u32 pc = 0; bool denied = false; std::vector<u16> words; };
  static constexpr unsigned kSmcVariants = 8;
  std::vector<SmcVariant> smc_slot_;                  // by slot; pc 0: none
  std::unordered_map<u32, std::vector<int>> smc_pcs_; // PC -> its variant slots
  std::vector<u16> SmcSourceWords(u32 pc, u32 e_addr);
  Block *SmcReuse(u32 pc, addrs *ParentT);
  std::vector<u16> smc_words_;  // SmcReuse's scratch: the current source words
  int SmcSlot(u32 pc);
  // VITA_SH2_PACKED_CODE: slot code is placed at a cursor in the arena, each
  // block right after the previous one, instead of at a fixed 4 KiB slot. A
  // compile still has MAXBLOCKSIZE bytes of room, so blocks end where they
  // did; live blocks whose code the room overlaps are evicted as a recycled
  // slot is. (Members unconditional: one class layout.)
  std::vector<u32> code_off_, code_bytes_;             // by slot; bytes 0: none
  std::vector<std::vector<int>> code_page_slots_;      // by 4 KiB arena page
  size_t code_cursor_ = 0;
  u32 last_code_bytes_ = 0;                            // set by EmmitCode
  void EvictSlot(int slot);
  void CodeRelease(int slot);
  void PlaceCode(int slot);
  void PlacedCode(int slot);
  std::unordered_set<u32> spec_deny;   // block starts compiled without stack speculation
  void SpecDeny(u32 pc) {
#ifdef VITA_STACK_PROFILE
    extern u32 g_prof_spec[4]; ++g_prof_spec[3];
#endif
    spec_deny.insert(pc);
    if ((pc & 0x0FF00000) == 0x06000000) SetHigh((pc & 0x000FFFFF) >> 1, NULL);
  }

  // Every high-RAM LookupTable write. Keeps Block::link_pc equal to b_addr
  // exactly while sh2_dispatch would find the block for that PC, and relinks
  // the block exits waiting for a newly published block (VITA_SH2_LINK).
  void SetHigh(u32 index, Block *b) {
    if (Block *old = LookupTable[index]) { old->link_pc = 0; ++g_code_epoch; }
    LookupTable[index] = b;
    if (b && (b->b_addr & 0xFFF00001u) == 0x06000000u && ((b->b_addr & 0x000FFFFFu) >> 1) == index) {
      b->link_pc = b->b_addr;
      Relink(b);
    }
  }
  // Block linking: a block's static exits (at most two) into high RAM, as
  // byte offsets of their link stubs, and the reverse index by target PC.
  struct LinkExit { u32 target, offset; };
  LinkExit (*link_out)[2] = nullptr;
  std::unordered_multimap<u32, u32> link_in;  // target PC -> slot * 2 + exit
  u8 *link_check = nullptr;                   // shared check routine (code arena)
  u8 *arena_dispatch = nullptr;               // sh2_dispatch copy (code arena), see EmmitCode
  void Relink(Block *b);
  void UnlinkSlot(int slot);
  void PatchLink(int slot, int exit, const Block *target);

  inline void setDirty(u32 addr) {
    addr = adress_mask(addr);
    if (LookupParentTable[addr].size() == 0) return;
    for (auto it = LookupParentTable[addr].begin(); it != LookupParentTable[addr].end(); it++) {
      if (LookupTable[*it] != NULL) {
        for (u32 i = adress_mask(LookupTable[*it]->b_addr) ; i <= adress_mask(LookupTable[*it]->e_addr); i++ ) {
          if (i != addr) {
            LookupParentTable[i].remove(*it);
          }
        }
         LOG("%d %08X is removed", LookupTable[*it]->id, (*it) << 1);
        remove_count_++;
        self_modify_block[ (((*it) << 1) | 0x06000000) ] = LookupTable[*it]->id;
        SetHigh(*it, NULL);
      }
    }
    LookupParentTable[addr].clear();
  }

  void Init();

  Block * CompileBlock( u32 pc, addrs * ParentT );

  void opcodePass(x86op_desc *op, u16 opcode, u8 *ptr);  
  int  opcodeIndex(u16 code );
  void FindOpCode(u16 opcode, u8 * instindex);
  void BuildInstructionList();

  int findFreeBlock(u32 pc);
  int EmmitCode(Block *page, addrs * ParentT = NULL);
  // Second compile pass of a block holding MAC operations (VITA_SH2_MAC_REGIONS):
  // admit them into register regions, ending exactly after forced_end_.
  bool mac_regions_ = false;
  u32 forced_end_ = 0;

  // statics
  u32 compile_count_ ;
  u32 exec_count_;
  u32 remove_count_;


  void ShowStatics();
  void SetDebugMode(bool debug) { debug_mode_ = debug;  }
};

typedef void(*dynaFunc)(tagSH2*);

class DynarecSh2
{
protected:
  SH2_struct *parent;
  tagSH2 *  m_pDynaSh2;
  CompileBlocks * m_pCompiler;
  int       m_ClockCounter;
  bool      m_bIntruptSort;
  bool one_step_;
  u32 pre_exe_count_;
  bool is_slave_ = false;
  u32 pre_PC_;
  SH2_struct * ctx_;
  YabMutex * mtx_;
  bool logenable_;

  u32 pre_cnt_;
  u32 interruput_chk_cnt_;
  u32 interruput_cnt_;
  u32 loopskip_cnt_;

  enum enDebugState {
    NORMAL,
    REQUESTED,
    COLLECTING,
    FINISHED,
  };
  
  enDebugState statics_trigger_ = NORMAL;
  MapCompileStatics compie_statics_;
  string message_buf;

public:
  DynarecSh2();
  ~DynarecSh2();
  static DynarecSh2 * CurrentContext;
  void SetCurrentContext(){ CurrentContext = this; }
  void SetSlave(bool is_slave) { is_slave_ = is_slave; }
  void SetContext(SH2_struct * ctx) { ctx_= ctx;}
  bool IsSlave() { return is_slave_;  }

  dlstIntct m_IntruptTbl;
  void RemoveInterrupt(u8 Vector, u8 level);
  void AddInterrupt( u8 Vector, u8 level );
  int CheckInterupt();
  int InterruptRutine(u8 Vector, u8 level);
  int CheckOneStep();

  void ResetCPU();  
  void ExecuteCount(u32 Count );
  void ExecuteCountDebug(u32 Count);
#ifdef VITA_SH2_LEAN_DISPATCH
  void ExecuteCountLean(u32 Count);
  void ExecuteCountLeanFull(u32 Count);
#endif
  int Execute();
  int ExecuteBlock(Block *block);
  int FinishBlock(Block *block);
  void Undecoded();

  void AddCycle(u32 cycle) {
    addcycle_ += cycle;
  }

  u32 addcycle_ = 0;
  u32 &memcycle_;               // m_pDynaSh2->memcycle
  bool counted_slice_active_ = false;
  // Spin fast-forward (VITA_SH2_SPIN_FORWARD); unconditional for one layout.
  // Recorded architectural state (IdleRegs order) and slice count after each
  // block of the current recording; ring of recent slice-end states.
  // Blocks one recording may hold: a delay loop of 64 DT passes inside a
  // polling loop is ~70 blocks per period.
  enum { kSpinRecMax = 96 };
  u32 spin_regs_[kSpinRecMax + 1][23];
  u32 spin_count_[kSpinRecMax + 1];
  // Per recorded state: hash of all 23 words, and of the words an affine
  // period must leave unchanged (R0, SR..PC); cheap rejects for the searches.
  u32 spin_hash_[kSpinRecMax + 1][2];
  void SpinHash(unsigned k) {
    const u32 *r = spin_regs_[k];
    u32 h = r[0];
    for (int w = 15; w < 23; ++w) h = (h << 5 | h >> 27) ^ r[w];
    u32 f = h;
    for (int w = 1; w < 15; ++w) f = (f << 5 | f >> 27) ^ r[w];
    spin_hash_[k][0] = f; spin_hash_[k][1] = h;
  }
  u32 spin_end_pc_[4] = {}, spin_end_hash_[4] = {};
  unsigned spin_end_next_ = 0;
  bool spin_candidate_ = false;
  unsigned spin_cooldown_ = 0;
  unsigned spin_fails_ = 0;   // consecutive recordings without a forward (backoff)
  // A recording still open at a slice end (VITA_SH2_SPIN_CARRY): its length,
  // the offset that makes the next slice's counts continue spin_count_, and
  // what must be unchanged for the next slice to continue it.
  bool spin_carry_ = false, spin_carry_rs_ = false;  // rs: spin_read_* holds the recorded blocks' read set
  unsigned spin_carry_n_ = 0, spin_carry_slices_ = 0;
  u32 spin_carry_wram_ = 0, spin_carry_native_ = 0, spin_carry_pending_ = 0;
  // Proven cycle (positions j..n of spin_regs_/spin_count_) usable by later
  // slices while nothing can have changed memory or the pending interrupt.
  // hint: recorded position where the last forward ended; div_*: cached
  // (remaining - 1) -> whole-cycle cycles for the last remaining count.
  // ftcsr_dep: the recorded cycle read this CPU's FTCSR, which then must
  // still hold ftcsr_val for the proof to apply in a later slice.
  struct { bool valid; unsigned j, n; u32 pending, epoch, native_epoch; unsigned hint; u32 div_rem, div_whole;
           bool ftcsr_dep; u32 ftcsr_val; u32 pc_lo, pc_hi; } spin_proof_ = {};
  // pc_lo..pc_hi: PC range of the proof's recorded states (a cheap miss test
  // for slices that start elsewhere).
  void SpinProofRange() {
    u32 lo = ~0u, hi = 0;
    for (unsigned p = spin_proof_.j; p <= spin_proof_.n; ++p) { const u32 pc = spin_regs_[p][22]; if (pc < lo) lo = pc; if (pc > hi) hi = pc; }
    spin_proof_.pc_lo = lo; spin_proof_.pc_hi = hi;
  }
  // Read set of a proven cycle (VITA_SH2_SPIN_READSET): when every memory read
  // of the cycle's blocks has a statically known address (PC-relative
  // literals, register-indirect loads from the recorded entry state) and
  // lands in work RAM or is this CPU's FTCSR, the proof depends on memory only
  // through those words and the blocks' own code: the aligned host words are
  // kept and compared instead of the global write epochs.
  enum { kSpinReadMax = 48 };
  bool spin_read_self_ = false;
  unsigned spin_read_n_ = 0;
  const u32 *spin_read_ptr_[kSpinReadMax] = {};
  u32 spin_read_val_[kSpinReadMax] = {};
  bool SpinReadSetBuild(unsigned j, unsigned n);
  // Affine cycle (VITA_SH2_SPIN_AFFINE): recorded states j..n-1 are period 0,
  // state n = state j + delta, where delta is nonzero only for general
  // registers the cycle touches solely through ADD #imm to themselves (so
  // nothing in the cycle depends on them). After m periods the state at
  // position q is spin_regs_[q] + m * delta.
  bool spin_affine_ = false;
  u32 spin_delta_[16] = {};
  u32 spin_div_periods_ = 0;
  int SpinAffineDetect(unsigned spin_n);
  bool SpinAffineIndependent(unsigned j, unsigned n) const;
  bool last_selfloop_ = false;
  bool last_resident_ = false;  // the slice ended in a resident loop at its deadline
  bool SpinReadSetSame() const {
    for (unsigned i = 0; i < spin_read_n_; ++i) if (*spin_read_ptr_[i] != spin_read_val_[i]) return false;
    return true;
  }
  // Unconditional so every translation unit agrees on the object layout.
  // valid: the last executed slice was one idle-loop run that ended in the
  // same complete register state as the idle run before it (a fixed point),
  // and nothing has changed the registers since (every executed slice
  // re-derives this; external setters and reset clear it).
  // native_epoch/io_*: see the SPIN_FORWARD generalization in ExecuteCountLean.
  // affine (VITA_SH2_IDLE_AFFINE): the two runs ended in states differing
  // by delta in R1..R14 only, registers the idle block uses only as ADD
  // #imm to themselves; a skip adds delta (to the registers and to regs).
  // mc: the idle block's memory cycles per run (the slice ends target + mc).
  struct { bool valid, cand; u32 pc, epoch; u32 regs[23]; u32 native_epoch, io_addr, io_val;
           bool affine, readset; u32 delta[16], mc; } idle_ = {};
  inline void InvalidateIdle() { idle_.valid = idle_.cand = false; }
  // readset (VITA_SH2_IDLE_AFFINE): the words the idle block can read (code,
  // literals, known-address loads), valid instead of the global write epochs.
  enum { kIdleReadMax = 16 };
  const u32 *idle_read_ptr_[kIdleReadMax] = {};
  u32 idle_read_val_[kIdleReadMax] = {};
  unsigned idle_read_n_ = 0;
  void IdleReadSetBuild(const u32 *regs);
  bool IdleReadSetSame() const {
    for (unsigned i = 0; i < idle_read_n_; ++i) if (*idle_read_ptr_[i] != idle_read_val_[i]) return false;
    return true;
  }
#ifdef VITA_SH2_IDLE_SLICE_SKIP
#define VITA_IDLE_INVALIDATE() InvalidateIdle()
#else
#define VITA_IDLE_INVALIDATE() ((void)0)
#endif
  void ShowStatics();
  void ShowCompileInfo();
  void ResetCompileInfo();

  void onFrame(){
    m_pCompiler->self_modify_block.clear();
  }

  tagSH2 * getDynaSh(){ return m_pDynaSh2; }; 

  inline u32 * GetGenRegPtr() { return m_pDynaSh2->GenReg; }
  inline u32 GET_MACH() { return m_pDynaSh2->SysReg[0]; }
  inline u32 GET_MACL() { return m_pDynaSh2->SysReg[1]; }
  inline u32 GET_PR() { return m_pDynaSh2->SysReg[2]; }
  inline u32 GET_PC() { return m_pDynaSh2->SysReg[3]; }
  inline u32 GET_COUNT() { return m_pDynaSh2->SysReg[4]; } 
  inline u32 GET_ICOUNT() { return m_pDynaSh2->SysReg[5]; } 
  inline u32 GET_SR() { return m_pDynaSh2->CtrlReg[0]; }
  inline u32 GET_GBR() { return m_pDynaSh2->CtrlReg[1]; }
  inline u32 GET_VBR() { return m_pDynaSh2->CtrlReg[2]; }
  inline void SET_MACH( u32 v ) { m_pDynaSh2->SysReg[0] = v; }
  inline void SET_MACL( u32 v ) { m_pDynaSh2->SysReg[1] = v; }
  inline void SET_PR( u32 v ) { m_pDynaSh2->SysReg[2] = v; }
  inline void SET_PC( u32 v ) { m_pDynaSh2->SysReg[3] = v; }
  inline void SET_COUNT( u32 v ) { m_pDynaSh2->SysReg[4] = v; } 
  inline void SET_ICOUNT(u32 v ) { m_pDynaSh2->SysReg[5] = v; } 
  inline void SET_SR(u32 v ) { m_pDynaSh2->CtrlReg[0] = v; }
  inline void SET_GBR( u32 v ) { m_pDynaSh2->CtrlReg[1] = v; }
  inline void SET_VBR( u32 v ) { m_pDynaSh2->CtrlReg[2] = v; }  

  int GetCurrentStatics(MapCompileStatics & buf);
  int Resume();
};


// callback from cpu emulation
extern "C"
{
  int EachClock();
  int DelayEachClock();
  int DebugEachClock();
  int DebugDelayClock();

  void memSetByte(u32, u8 );
  void memSetWord(u32, u16);
  void memSetLong(u32, u32);

  u8 memGetByte(u32);
  u16 memGetWord(u32);
  u32 memGetLong(u32);  
  
}

#ifdef _WINDOWS

#define dynaLock()	__asm \
{                         \
    __asm push edx         \
   /*__asm push ebx*/         \
}
#define dynaFree()	__asm \
{                         \
   /*__asm pop ebx*/          \
   __asm pop edx          \
}

#else
#define dynaLock()
#define dynaFree()
#endif


#endif // _DYNAREC_SH2_H_
