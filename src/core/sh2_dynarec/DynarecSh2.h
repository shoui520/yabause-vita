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
#include <array>
#include <vector>

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
typedef std::list<dIntcTbl> dlstIntct;


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
        LookupTable[*it] = NULL;
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
  int Execute();
  int ExecuteBlock(Block *block);
  int FinishBlock(Block *block);
  void Undecoded();

  void AddCycle(u32 cycle) {
    addcycle_ += cycle;
  }

  u32 addcycle_ = 0;
  u32 memcycle_ = 0;
  bool counted_slice_active_ = false;
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
