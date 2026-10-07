/*  Copyright 2015 Theo Berkau

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/
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

#include "scsp.h"
#include "scspdsp.h"
#include "c68k/native_guard.h"


//saturate 24 bit signed integer
static INLINE s32 saturate_24(s32 value)
{
   if (value > 8388607)
      value = 8388607;

   if (value < (-8388608))
      value = (-8388608);

   return value;
}


#define sign_x_to_s32(_bits, _value) (((int)((u32)(_value) << (32 - _bits))) >> (32 - _bits))
#define min(a,b) (a<b)?a:b

static INLINE unsigned clz(u32 v)
{
#if defined(__GNUC__) || defined(__clang__) || defined(__ICC) || defined(__INTEL_COMPILER)
  return __builtin_clz(v);
#elif defined(_MSC_VER)
  unsigned long idx;

  _BitScanReverse(&idx, v);

  return 31 ^ idx;
#else
  unsigned ret = 0;
  unsigned tmp;

  tmp = !(v & 0xFFFF0000) << 4; v <<= tmp; ret += tmp;
  tmp = !(v & 0xFF000000) << 3; v <<= tmp; ret += tmp;
  tmp = !(v & 0xF0000000) << 2; v <<= tmp; ret += tmp;
  tmp = !(v & 0xC0000000) << 1; v <<= tmp; ret += tmp;
  tmp = !(v & 0x80000000) << 0;            ret += tmp;

  return(ret);
#endif
}

//sign extended to 32 bits instead of 24
static s32 INLINE float_to_int(u16 f_val)
{
   u32 sign = (f_val >> 15) & 1;
   u32 sign_inverse = (!sign) & 1;
   u32 exponent = (f_val >> 11) & 0xf;
   u32 mantissa = f_val & 0x7FF;

   s32 ret_val = sign << 31;

   if (exponent > 11)
   {
      exponent = 11;
      ret_val |= (sign << 30);
   }
   else
      ret_val |= (sign_inverse << 30);

   ret_val |= mantissa << 19;

   ret_val = ret_val >> (exponent + (1 << 3));

   return ret_val;
}

static u16 INLINE int_to_float(u32 i_val)
{
   u32 sign = (i_val >> 23) & 1;
   u32 exponent = 0;

   if (sign != 0)
      i_val = (~i_val) & 0x7FFFFF;

   if (i_val <= 0x1FFFF)
   {
      i_val *= 64;
      exponent += 0x3000;
   }

   if (i_val <= 0xFFFFF)
   {
      i_val *= 8;
      exponent += 0x1800;
   }

   if (i_val <= 0x3FFFFF)
   {
      i_val *= 2;
      exponent += 0x800;
   }

   if (i_val <= 0x3FFFFF)
   {
      i_val *= 2;
      exponent += 0x800;
   }

   if (i_val <= 0x3FFFFF)
      exponent += 0x800;

   i_val >>= 11;
   i_val &= 0x7ff;
   i_val |= exponent;

   if (sign != 0)
      i_val ^= (0x7ff | (1 << 15));

   return i_val;
}


void ScspDspExec(ScspDsp* dsp, int addr, u8 * sound_ram)
{
  u16* sound_ram_16 = (u16*)sound_ram;
  u64 mul_temp = 0;
  int nofl = 0;
  u32 x_temp = 0;
  s32 y_extended = 0;
  union ScspDspInstruction inst;
  u32 address = 0;
  s32 shift_temp = 0;

  inst.all = scsp_dsp.mpro[addr];

  const unsigned TEMPWriteAddr = (inst.part.twa + dsp->mdec_ct) & 0x7F;
  const unsigned TEMPReadAddr = (inst.part.tra + dsp->mdec_ct) & 0x7F;

  if (inst.part.ira & 0x20) {
    if (inst.part.ira & 0x10) {
      if (!(inst.part.ira & 0xE))
        dsp->inputs = dsp->exts[inst.part.ira & 0x1] << 8;
    }else{
      dsp->inputs = dsp->mixs[inst.part.ira & 0xF] << 4;
    }
  }else{
    dsp->inputs = dsp->mems[inst.part.ira & 0x1F];
  }

  const int INPUTS = sign_x_to_s32(24, dsp->inputs);
  const int TEMP = sign_x_to_s32(24, dsp->temp[TEMPReadAddr]);
  const int X_SEL_Inputs[2] = { TEMP, INPUTS };
  const u16 Y_SEL_Inputs[4] = { 
    dsp->frc_reg, dsp->coef[inst.part.coef],
    (u16)((dsp->y_reg >> 11) & 0x1FFF), 
    (u16)((dsp->y_reg >> 4) & 0x0FFF) 
  };
  const u32 SGA_Inputs[2] = { (u32)TEMP, dsp->shift_reg }; // ToDO:?

  if (inst.part.yrl) {
    dsp->y_reg = INPUTS & 0xFFFFFF;
  }

  int ShifterOutput;

  ShifterOutput = (u32)sign_x_to_s32(26, dsp->shift_reg) << (inst.part.shift0 ^ inst.part.shift1);

  if (!inst.part.shift1)
  {
    if(ShifterOutput > 0x7FFFFF)
      ShifterOutput = 0x7FFFFF;
    else if(ShifterOutput < -0x800000)
      ShifterOutput = 0x800000;
  }
  ShifterOutput &= 0xFFFFFF;

  if (inst.part.ewt)
    dsp->efreg[inst.part.ewa] = (ShifterOutput >> 8);

  if (inst.part.twt)
    dsp->temp[TEMPWriteAddr] = ShifterOutput;

  if (inst.part.frcl)
  {
    const unsigned F_SEL_Inputs[2] = { (unsigned)(ShifterOutput >> 11), (unsigned)(ShifterOutput & 0xFFF) };

    dsp->frc_reg = F_SEL_Inputs[inst.part.shift0 & inst.part.shift1];
    //printf("FRCL: 0x%08x\n", DSP.FRC_REG);
  }

  dsp->product = ((s64)sign_x_to_s32(13, Y_SEL_Inputs[inst.part.ysel]) * X_SEL_Inputs[inst.part.xsel]) >> 12;

  u32 SGAOutput;

  SGAOutput = SGA_Inputs[inst.part.bsel];

  if (inst.part.negb)
    SGAOutput = -SGAOutput;

  if (inst.part.zero)
    SGAOutput = 0;

  dsp->shift_reg = (dsp->product + SGAOutput) & 0x3FFFFFF;
  //
  //
  if (inst.part.iwt)
  {
    dsp->mems[inst.part.iwa] = dsp->read_value;
  }

  if (dsp->read_pending)
  {
    u16 tmp = sound_ram_16[dsp->io_addr];
    dsp->read_value = (dsp->read_pending == 2) ? (tmp << 8) : float_to_int(tmp);
    dsp->read_pending = 0;
  }
  else if (dsp->write_pending)
  {
    if (!(dsp->io_addr & 0x40000)) {
      C68K_NATIVE_GUARD;
      sound_ram_16[dsp->io_addr] = dsp->write_value;
      M68KWriteNotify(dsp->io_addr * 2, 2);
    }
    dsp->write_pending = 0;
  }
  {
    u16 addr;

    addr = dsp->madrs[inst.part.masa];
    addr += inst.part.nxadr;

    if (inst.part.adreb)
    {
      addr += sign_x_to_s32(12, dsp->adrs_reg);
    }

    if (!inst.part.table)
    {
      addr += dsp->mdec_ct;
      addr &= (0x2000 << dsp->rbl) - 1;
    }

    dsp->io_addr = (addr + (dsp->rbp << 12)) & 0x3FFFF;

    if (inst.part.mrd)
    {
      dsp->read_pending = 1 + inst.part.nofl;
    }
    if (inst.part.mwt)
    {
      dsp->write_pending = 1;
      dsp->write_value = inst.part.nofl ? (ShifterOutput >> 8) : int_to_float(ShifterOutput);
    }
    if (inst.part.adrl)
    {
      const u16 A_SEL_Inputs[2] = { (u16)((INPUTS >> 16) & 0xFFF), (u16)(ShifterOutput >> 12) };

      dsp->adrs_reg = A_SEL_Inputs[inst.part.shift0 & inst.part.shift1];
    }
  }
}


int ScspDspAssembleGetValue(char* instruction)
{
   char temp[512] = { 0 };
   int value = 0;
   sscanf(instruction, "%s %d", temp, &value);
   return value;
}

u64 ScspDspAssembleLine(char* line)
{
   union ScspDspInstruction instruction = { 0 };

   char* temp = NULL;

   if ((temp = strstr(line, "tra")))
   {
      instruction.part.tra = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "twt"))
   {
      instruction.part.twt = 1;
   }

   if ((temp = strstr(line, "twa")))
   {
      instruction.part.twa = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "xsel"))
   {
      instruction.part.xsel = 1;
   }

   if ((temp = strstr(line, "ysel")))
   {
      instruction.part.ysel = ScspDspAssembleGetValue(temp);
   }

   if ((temp = strstr(line, "ira")))
   {
      instruction.part.ira = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "iwt"))
   {
      instruction.part.iwt = 1;
   }

   if ((temp = strstr(line, "iwa")))
   {
      instruction.part.iwa = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "table"))
   {
      instruction.part.table = 1;
   }

   if (strstr(line, "mwt"))
   {
      instruction.part.mwt = 1;
   }

   if (strstr(line, "mrd"))
   {
      instruction.part.mrd = 1;
   }

   if (strstr(line, "ewt"))
   {
      instruction.part.ewt = 1;
   }

   if ((temp = strstr(line, "ewa")))
   {
      instruction.part.ewa = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "adrl"))
   {
      instruction.part.adrl = 1;
   }

   if (strstr(line, "frcl"))
   {
      instruction.part.frcl = 1;
   }

   if ((temp = strstr(line, "shift")))
   {
      instruction.part.shift1 = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "yrl"))
   {
      instruction.part.yrl = 1;
   }

   if (strstr(line, "negb"))
   {
      instruction.part.negb = 1;
   }

   if (strstr(line, "zero"))
   {
      instruction.part.zero = 1;
   }

   if (strstr(line, "bsel"))
   {
      instruction.part.bsel = 1;
   }

   if (strstr(line, "nofl"))
   {
      instruction.part.nofl = 1;
   }

   if ((temp = strstr(line, "coef")))
   {
      instruction.part.coef = ScspDspAssembleGetValue(temp);
   }

   if ((temp = strstr(line, "masa")))
   {
      instruction.part.masa = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "adreb"))
   {
      instruction.part.adreb = 1;
   }

   if (strstr(line, "nxadr"))
   {
      instruction.part.adreb = 1;
   }

   if (strstr(line, "nop"))
   {
      instruction.all = 0;
   }

   return instruction.all;
}

void ScspDspAssembleFromFile(char * filename, u64* output)
{
   int i;
   char line[1024] = { 0 };

   FILE * fp = fopen(filename, "r");

   if (!fp)
   {
      return;
   }

   for (i = 0; i < 128; i++)
   {
      char * result = fgets(line, sizeof(line), fp);
      output[i] = ScspDspAssembleLine(line);
   }
   fclose(fp);
}

void ScspDspDisasm(u8 addr, char *outstring)
{
   union ScspDspInstruction instruction;

   instruction.all = scsp_dsp.mpro[addr];

   sprintf(outstring, "%02X: ", addr);
   outstring += strlen(outstring);

   if (instruction.all == 0)
   {
      sprintf(outstring, "nop ");
      outstring += strlen(outstring);
      return;
   }

   if (instruction.part.nofl)
   {
      sprintf(outstring, "nofl ");
      outstring += strlen(outstring);
   }

   if (instruction.part.coef)
   {
      sprintf(outstring, "coef %02X ", (unsigned int)(instruction.part.coef & 0x3F));
      outstring += strlen(outstring);
   }

   if (instruction.part.masa)
   {
      sprintf(outstring, "masa %02X ", (unsigned int)(instruction.part.masa & 0x1F));
      outstring += strlen(outstring);
   }

   if (instruction.part.adreb)
   {
      sprintf(outstring, "adreb ");
      outstring += strlen(outstring);
   }

   if (instruction.part.nxadr)
   {
      sprintf(outstring, "nxadr ");
      outstring += strlen(outstring);
   }

   if (instruction.part.table)
   {
      sprintf(outstring, "table ");
      outstring += strlen(outstring);
   }

   if (instruction.part.mwt)
   {
      sprintf(outstring, "mwt ");
      outstring += strlen(outstring);
   }

   if (instruction.part.mrd)
   {
      sprintf(outstring, "mrd ");
      outstring += strlen(outstring);
   }

   if (instruction.part.ewt)
   {
      sprintf(outstring, "ewt ");
      outstring += strlen(outstring);
   }

   if (instruction.part.ewa)
   {
      sprintf(outstring, "ewa %01X ", (unsigned int)(instruction.part.ewa & 0xf));
      outstring += strlen(outstring);
   }

   if (instruction.part.adrl)
   {
      sprintf(outstring, "adrl ");
      outstring += strlen(outstring);
   }

   if (instruction.part.frcl)
   {
      sprintf(outstring, "frcl ");
      outstring += strlen(outstring);
   }

   if (instruction.part.shift1)
   {
      sprintf(outstring, "shift %d ", (int)(instruction.part.shift1 & 3));
      outstring += strlen(outstring);
   }

   if (instruction.part.yrl)
   {
      sprintf(outstring, "yrl ");
      outstring += strlen(outstring);
   }

   if (instruction.part.negb)
   {
      sprintf(outstring, "negb ");
      outstring += strlen(outstring);
   }

   if (instruction.part.zero)
   {
      sprintf(outstring, "zero ");
      outstring += strlen(outstring);
   }

   if (instruction.part.bsel)
   {
      sprintf(outstring, "bsel ");
      outstring += strlen(outstring);
   }

   if (instruction.part.xsel)
   {
      sprintf(outstring, "xsel ");
      outstring += strlen(outstring);
   }

   if (instruction.part.ysel)
   {
      sprintf(outstring, "ysel %d ", (int)(instruction.part.ysel & 3));
      outstring += strlen(outstring);
   }

   if (instruction.part.ira)
   {
      sprintf(outstring, "ira %02X ", (int)(instruction.part.ira & 0x3F));
      outstring += strlen(outstring);
   }

   if (instruction.part.iwt)
   {
      sprintf(outstring, "iwt ");
      outstring += strlen(outstring);
   }

   if (instruction.part.iwa)
   {
      sprintf(outstring, "iwa %02X ", (unsigned int)(instruction.part.iwa & 0x1F));
      outstring += strlen(outstring);
   }

   if (instruction.part.tra)
   {
      sprintf(outstring, "tra %02X ", (unsigned int)(instruction.part.tra & 0x7F));
      outstring += strlen(outstring);
   }

   if (instruction.part.twt)
   {
      sprintf(outstring, "twt ");
      outstring += strlen(outstring);
   }

   if (instruction.part.twa)
   {
      sprintf(outstring, "twa %02X ", (unsigned int)(instruction.part.twa & 0x7F));
      outstring += strlen(outstring);
   }

   if (instruction.part.unknown)
   {
      sprintf(outstring, "unknown ");
      outstring += strlen(outstring);
   }

   if (instruction.part.unknown2)
   {
      sprintf(outstring, "unknown2 ");
      outstring += strlen(outstring);
   }

//   if (instruction.part.unknown3)
 //  {
 //     sprintf(outstring, "unknown3 %d", (int)(instruction.part.unknown3 & 3));
 //     outstring += strlen(outstring);
 //  }
}

void ScspDspDisassembleToFile(char * filename)
{
   int i;
   FILE * fp = fopen(filename, "w");

   if (!fp)
   {
      return;
   }

   for (i = 0; i < 128; i++)
   {
      char output[1024] = { 0 };
      ScspDspDisasm(i, output);
      fprintf(fp, "%s\n", output);
   }

   fclose(fp);
}

#ifdef VITA_SCSP_DSP
/* ScspDspExec over a whole mixing batch for the legacy SCSP (scsp_update).
 * The program is decoded once per change of MPRO/COEF/MADRS (compared at
 * every batch, so register writes, resets, state loads and early-frame
 * rollbacks all redecode), and the registers live in locals. Per sample, as
 * generate_sample: steps 0..last_step-1, then MDEC_CT. The shift register
 * result of a step is computed only when the next step reads it (or when
 * it is the last step), the shifter output only when a step uses it, and
 * the memory address only for a pending or new memory access. */
typedef struct {
   u8 tra, twa, in_kind, in_idx;
   u8 xsel, ysel, yrl, shl, sat, use_so, need_prod;
   u8 ewt, ewa, twt, frcl, frc_lo;
   u8 sga, negb, iwt, iwa;
   u8 table, adreb, mrd, mwt, nofl, adrl, adr_hi, mem;
   s32 coef;
   u16 base;
} ScspDspOp;

static struct {
   u64 mpro[128];
   u16 coef[64], madrs[32];
   int valid, steps;
   ScspDspOp op[128];
} dsp_dec;

/* Returns the number of steps (trailing NOPs dropped). */
static int ScspDspDecodeOps(const u64 *mpro, const u16 *coef, const u16 *madrs, ScspDspOp *op)
{
   int n, i;
   for (n = 128; n > 0 && !mpro[n - 1]; --n);
   for (i = 0; i < n; i++)
   {
      union ScspDspInstruction inst;
      ScspDspOp *o = &op[i];
      inst.all = mpro[i];
      memset(o, 0, sizeof(*o));
      o->tra = inst.part.tra;
      o->twa = inst.part.twa;
      if (inst.part.ira & 0x20)
      {
         if (inst.part.ira & 0x10)
         {
            o->in_kind = (inst.part.ira & 0xE) ? 3 : 2;
            o->in_idx = inst.part.ira & 1;
         }
         else
            o->in_kind = 1, o->in_idx = inst.part.ira & 0xF;
      }
      else
         o->in_kind = 0, o->in_idx = inst.part.ira & 0x1F;
      o->xsel = inst.part.xsel;
      o->ysel = inst.part.ysel;
      o->coef = sign_x_to_s32(13, coef[inst.part.coef]);
      o->yrl = inst.part.yrl;
      o->shl = inst.part.shift0 ^ inst.part.shift1;
      o->sat = !inst.part.shift1;
      o->ewt = inst.part.ewt;
      o->ewa = inst.part.ewa;
      o->twt = inst.part.twt;
      o->frcl = inst.part.frcl;
      o->frc_lo = inst.part.shift0 & inst.part.shift1;
      o->sga = inst.part.zero ? 0 : inst.part.bsel ? 2 : 1;
      o->negb = inst.part.negb;
      o->iwt = inst.part.iwt;
      o->iwa = inst.part.iwa;
      o->table = inst.part.table;
      o->adreb = inst.part.adreb;
      o->mrd = inst.part.mrd;
      o->mwt = inst.part.mwt;
      o->nofl = inst.part.nofl;
      o->adrl = inst.part.adrl;
      o->adr_hi = inst.part.shift0 & inst.part.shift1;
      o->mem = o->mrd | o->mwt;
      o->base = (u16)(madrs[inst.part.masa] + inst.part.nxadr);
      o->use_so = o->ewt | o->twt | o->frcl | o->mwt | o->adrl;
   }
   for (i = 0; i < n; i++)
   {
      const ScspDspOp *next = &op[(i + 1) % n];
      op[i].need_prod = i == n - 1 || next->use_so || next->sga == 2;
   }
   return n;
}

static void ScspDspDecode(const ScspDsp *dsp)
{
   memcpy(dsp_dec.mpro, dsp->mpro, sizeof(dsp_dec.mpro));
   memcpy(dsp_dec.coef, dsp->coef, sizeof(dsp_dec.coef));
   memcpy(dsp_dec.madrs, dsp->madrs, sizeof(dsp_dec.madrs));
   dsp_dec.steps = ScspDspDecodeOps(dsp->mpro, dsp->coef, dsp->madrs, dsp_dec.op);
   dsp_dec.valid = 1;
}

int ScspDspSteps(const ScspDsp *dsp)
{
   if (!dsp_dec.valid || memcmp(dsp_dec.mpro, dsp->mpro, sizeof(dsp_dec.mpro)) ||
       memcmp(dsp_dec.coef, dsp->coef, sizeof(dsp_dec.coef)) ||
       memcmp(dsp_dec.madrs, dsp->madrs, sizeof(dsp_dec.madrs)))
      ScspDspDecode(dsp);
   return dsp_dec.steps;
}

/* Samples pos0..pos1-1; the 4 KB sound RAM pages written are set in dirty. */
static void ScspDspRunC(ScspDsp *dsp, u16 *const ram, const s32 *const mix[16], const s16 *const ext[2],
                        u32 pos0, u32 pos1, s32 *bufL, s32 *bufR, const u8 out_l[16], const u8 out_r[16],
                        u32 dirty[4])
{
   const int n = dsp_dec.steps;
   const ScspDspOp *const ops = dsp_dec.op;
   s32 *const temp = dsp->temp, *const mems = dsp->mems;
   s16 *const efreg = dsp->efreg;
   s32 inputs = dsp->inputs;
   u32 shift_reg = dsp->shift_reg, read_value = dsp->read_value, io_addr = dsp->io_addr;
   s32 y_reg = dsp->y_reg;
   u16 frc_reg = dsp->frc_reg, adrs_reg = dsp->adrs_reg, write_value = (u16)dsp->write_value;
   int read_pending = dsp->read_pending, write_pending = dsp->write_pending;
   u32 mdec_ct = dsp->mdec_ct;
   const u32 rb_mask = (0x2000u << dsp->rbl) - 1, rb_base = (u32)dsp->rbp << 12;
   u8 outs[16], nout = 0;
   u32 pos;
   int i;

   if (!n)
      return;
   for (i = 0; i < 16; i++)
      if (out_l[i] != 31 || out_r[i] != 31)
         outs[nout++] = (u8)i;

   for (pos = pos0; pos < pos1; pos++)
   {
      const ScspDspOp *o = ops, *const end = ops + n;
      for (; o < end; o++)
      {
         s32 INPUTS, TEMP, so = 0, y;
         const unsigned tra = (o->tra + mdec_ct) & 0x7F;

         switch (o->in_kind)
         {
         case 0: inputs = mems[o->in_idx]; break;
         case 1: inputs = mix[o->in_idx][pos] << 8; break;
         case 2: inputs = ext[o->in_idx][pos] << 8; break;
         default: break;
         }
         INPUTS = sign_x_to_s32(24, inputs);
         TEMP = sign_x_to_s32(24, temp[tra]);

         switch (o->ysel)
         {
         case 0: y = sign_x_to_s32(13, frc_reg); break;
         case 1: y = o->coef; break;
         case 2: y = sign_x_to_s32(13, (y_reg >> 11) & 0x1FFF); break;
         default: y = (y_reg >> 4) & 0x0FFF; break;
         }
         if (o->yrl)
            y_reg = INPUTS & 0xFFFFFF;

         if (o->use_so)
         {
            so = (s32)((u32)sign_x_to_s32(26, shift_reg) << o->shl);
            if (o->sat)
            {
               if (so > 0x7FFFFF) so = 0x7FFFFF;
               else if (so < -0x800000) so = 0x800000;
            }
            so &= 0xFFFFFF;
            if (o->ewt)
               efreg[o->ewa] = (s16)(so >> 8);
            if (o->twt)
               temp[(o->twa + mdec_ct) & 0x7F] = so;
            if (o->frcl)
               frc_reg = (u16)(o->frc_lo ? (so & 0xFFF) : (so >> 11));
         }

         if (o->need_prod)
         {
            const s32 product = (s32)(((s64)y * (o->xsel ? INPUTS : TEMP)) >> 12);
            u32 sga = o->sga == 2 ? shift_reg : o->sga ? (u32)TEMP : 0;
            if (o->negb && o->sga)
               sga = -sga;
            shift_reg = ((u32)product + sga) & 0x3FFFFFF;
         }

         if (o->iwt)
            mems[o->iwa] = read_value;

         if (read_pending)
         {
            const u16 tmp = ram[io_addr];
            read_value = (read_pending == 2) ? ((u32)tmp << 8) : (u32)float_to_int(tmp);
            read_pending = 0;
         }
         else if (write_pending)
         {
            ram[io_addr] = write_value;
            dirty[io_addr >> 16] |= 1u << ((io_addr >> 11) & 31);
            write_pending = 0;
         }

         if (o->mem || write_pending)
         {
            u16 addr = o->base;
            if (o->adreb)
               addr += sign_x_to_s32(12, adrs_reg);
            if (!o->table)
            {
               addr += mdec_ct;
               addr &= rb_mask;
            }
            io_addr = (addr + rb_base) & 0x3FFFF;
            if (o->mrd)
               read_pending = 1 + o->nofl;
            if (o->mwt)
            {
               write_pending = 1;
               write_value = o->nofl ? (u16)(so >> 8) : int_to_float(so);
            }
         }
         if (o->adrl)
            adrs_reg = o->adr_hi ? (u16)(so >> 12) : (u16)((INPUTS >> 16) & 0xFFF);
      }

      if (!mdec_ct)
         mdec_ct = 0x2000u << dsp->rbl;
      mdec_ct--;

      for (i = 0; i < nout; i++)
      {
         const int e = outs[i];
         const s32 v = efreg[e];
         if (out_l[e] != 31) bufL[pos] += v >> out_l[e];
         if (out_r[e] != 31) bufR[pos] += v >> out_r[e];
      }
   }

   dsp->inputs = inputs;
   dsp->shift_reg = shift_reg;
   dsp->read_value = read_value;
   dsp->io_addr = io_addr;
   dsp->y_reg = y_reg;
   dsp->frc_reg = frc_reg;
   dsp->adrs_reg = adrs_reg;
   dsp->write_value = write_value;
   dsp->read_pending = read_pending;
   dsp->write_pending = write_pending;
   dsp->mdec_ct = mdec_ct;
}

#ifdef VITA_SCSP_DSP_JIT
static int ScspDspJitRun(ScspDsp *dsp, u16 *ram, const s32 *const mix[16], const s16 *const ext[2],
                         u32 len, s32 *bufL, s32 *bufR, const u8 out_l[16], const u8 out_r[16],
                         u32 dirty[4]);
#endif

void ScspDspRun(ScspDsp *dsp, u8 *sound_ram, const s32 *const mix[16], const s16 *const ext[2],
                u32 len, s32 *bufL, s32 *bufR, const u8 out_l[16], const u8 out_r[16])
{
   u16 *const ram = (u16 *)sound_ram;
   u32 dirty[4] = {0, 0, 0, 0};
   int i;

   if (!ScspDspSteps(dsp) || !len)
      return;
#ifdef VITA_SCSP_DSP_JIT
   if (!ScspDspJitRun(dsp, ram, mix, ext, len, bufL, bufR, out_l, out_r, dirty))
#endif
      ScspDspRunC(dsp, ram, mix, ext, 0, len, bufL, bufR, out_l, out_r, dirty);
   dsp->exts[0] = ext[0][len - 1];
   dsp->exts[1] = ext[1][len - 1];

   for (i = 0; i < 128; i++)
      if (dirty[i >> 5] & (1u << (i & 31)))
      {
         int j = i;
         while (j + 1 < 128 && (dirty[(j + 1) >> 5] & (1u << ((j + 1) & 31)))) j++;
         M68KWriteNotify((u32)i << 12, (u32)(j - i + 1) << 12);
         i = j;
      }
}
#ifdef VITA_SCSP_DSP_JIT
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
/* ScspDspRunC translated to ARMv7 A32 for one program, ring buffer and set
 * of output levels. Each step is straight-line code with its fields decoded;
 * the DSP registers live in r5-r10 (SHIFTED, Y_REG, FRC_REG, ADRS_REG,
 * MDEC_CT, INPUTS) and the sample position in r11. The memory pipeline is
 * resolved at translation time: the pending read/write state on entry to
 * each step is a fixed point of the program (ScspDspRunC runs the samples
 * until the state reaches it).
 *
 * The mixer runs on the sound thread, but code is published only by the
 * emulation thread, as for the SH-2 and SCU DSP code. A mixer that has no
 * translation of its current key requests one and runs ScspDspRunC; the
 * emulation thread translates it once per frame (ScspDspJitService) into the
 * buffer the mixer is not running, which the mixer adopts at its next batch:
 *   IDLE -> REQUEST (mixer: req, active) -> READY (emulation: buffer ready_buf)
 *   -> IDLE (mixer: active = ready_buf). */
typedef struct {
   s32 temp[128];            /* 0: must stay first (indexed from r4) */
   s32 mems[32];             /* 512 */
   s32 efreg[16];            /* 640: sign-extended */
   u32 read_value, io_addr, write_value;
   u16 *ram;
   const s32 *mix[16];
   const s16 *ext[2];
   s32 *bufL, *bufR;
   u32 len, pos0;
   u32 shift_reg, y_reg, frc_reg, adrs_reg, mdec_ct, inputs;
   u32 dirty[4];
} ScspDspJitCtx;
#define JOFF(f) ((u32)offsetof(ScspDspJitCtx, f))

typedef void (*ScspDspJitFn)(ScspDspJitCtx *ctx);

typedef struct {             /* no padding: compared with memcmp */
   u64 mpro[128];
   u16 coef[64], madrs[32];
   s32 rbl, rbp;
   u8 out_l[16], out_r[16];
} ScspDspJitKey;

enum { SJIT_IDLE, SJIT_REQUEST, SJIT_READY };
#define SJIT_BUF_BYTES (32 * 1024)

static struct {
   _Atomic int state;
   ScspDspJitKey req;        /* written by the mixer while IDLE */
   int active;               /* mixer's buffer; -1 none (set before REQUEST) */
   int ready_buf;
   ScspDspJitKey key[2];
   ScspDspJitFn fn[2];       /* NULL: the key cannot be translated */
   u8 pend_rp[2], pend_wp[2];
   u32 *mem;
   int failed;
   ScspDspOp op[128];        /* emulation thread */
} sjit = { SJIT_IDLE, {{0}}, -1 };

#ifdef VITA
unsigned char *VitaScspDspCodeArena(size_t *capacity);
void VitaScspDspCodeWriteBegin(void *p, size_t n);
void VitaScspDspCodeWriteEnd(void *p, size_t n);
static u32 *ScspDspJitAlloc(void)
{
   size_t cap;
   unsigned char *p = VitaScspDspCodeArena(&cap);
   return p && cap >= 2 * SJIT_BUF_BYTES ? (u32 *)p : NULL;
}
static void ScspDspJitWriteBegin(void *p, u32 n) { VitaScspDspCodeWriteBegin(p, n); }
static void ScspDspJitWriteEnd(void *p, u32 n) { VitaScspDspCodeWriteEnd(p, n); }
void VitaScspDspCodeAdopt(void *p, size_t n);
static void ScspDspJitAdopt(void *p, u32 n) { VitaScspDspCodeAdopt(p, n); }
#else
#include <sys/mman.h>
static u32 *ScspDspJitAlloc(void)
{
   void *p = mmap(NULL, 2 * SJIT_BUF_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   return p == MAP_FAILED ? NULL : (u32 *)p;
}
static void ScspDspJitWriteBegin(void *p, u32 n) { (void)p; (void)n; }
static void ScspDspJitWriteEnd(void *p, u32 n) { __builtin___clear_cache((char *)p, (char *)p + n); }
static void ScspDspJitAdopt(void *p, u32 n) { __builtin___clear_cache((char *)p, (char *)p + n); }
#endif

static u32 ScspDspJitF2I(u32 f) { return (u32)float_to_int((u16)f); }
static u32 ScspDspJitI2F(u32 v) { return int_to_float(v); }
static void ScspDspJitWrite(ScspDspJitCtx *c)
{
   const u32 a = c->io_addr;
   c->ram[a] = (u16)c->write_value;
   c->dirty[a >> 16] |= 1u << ((a >> 11) & 31);
}

/* ---- A32 emitter ---- */
typedef struct { u32 *cur, *end; int overflow; } SjEmit;
enum { SR0, SR1, SR2, SR3, SR4, SR5, SR6, SR7, SR8, SR9, SR10, SR11, SIP, SSP, SLR, SPC };
#define SJ_AL 0xE0000000u
#define SJ_EQ 0x00000000u
#define SJ_LT 0xB0000000u
enum { SJ_AND = 0, SJ_SUB = 2, SJ_RSB = 3, SJ_ADD = 4, SJ_CMP = 10, SJ_ORR = 12, SJ_MOV = 13, SJ_BIC = 14 };
enum { SJ_LSL = 0, SJ_LSR = 1, SJ_ASR = 2 };
static void sj(SjEmit *e, u32 w) { if (e->cur < e->end) *e->cur++ = w; else e->overflow = 1; }
static int sj_imm(u32 v)
{
   unsigned rot;
   for (rot = 0; rot < 16; rot++)
   {
      const u32 r = rot ? (v << (2 * rot)) | (v >> (32 - 2 * rot)) : v;
      if (r <= 0xFF) return (int)((rot << 8) | r);
   }
   return -1;
}
static void sj_dpi(SjEmit *e, u32 cond, unsigned op, unsigned rn, unsigned rd, u32 imm)
{
   const int enc = sj_imm(imm);
   if (enc < 0) { e->overflow = 1; return; }
   sj(e, cond | (1u << 25) | (op << 21) | ((op == SJ_CMP) << 20) | (rn << 16) | (rd << 12) | (u32)enc);
}
static void sj_dpr(SjEmit *e, unsigned op, unsigned rn, unsigned rd, unsigned rm, unsigned type, unsigned amount)
{
   if (type == SJ_ASR && amount == 0) type = SJ_LSL;   /* ASR #0 encodes ASR #32 */
   sj(e, SJ_AL | (op << 21) | ((op == SJ_CMP) << 20) | (rn << 16) | (rd << 12) | ((amount & 31) << 7) | (type << 5) | rm);
}
static void sj_mov(SjEmit *e, unsigned rd, unsigned rm, unsigned type, unsigned amount)
{
   sj_dpr(e, SJ_MOV, 0, rd, rm, type, amount);
}
static void sj_const(SjEmit *e, unsigned rd, u32 v)
{
   if (sj_imm(v) >= 0) { sj_dpi(e, SJ_AL, SJ_MOV, 0, rd, v); return; }
   sj(e, SJ_AL | 0x03000000u | ((v & 0xF000) << 4) | (rd << 12) | (v & 0xFFF));             /* MOVW */
   if (v >> 16)
      sj(e, SJ_AL | 0x03400000u | (((v >> 16) & 0xF000) << 4) | (rd << 12) | ((v >> 16) & 0xFFF)); /* MOVT */
}
static void sj_ldr(SjEmit *e, unsigned rd, unsigned rn, u32 off) { sj(e, SJ_AL | 0x05900000u | (rn << 16) | (rd << 12) | off); }
static void sj_str(SjEmit *e, unsigned rd, unsigned rn, u32 off) { sj(e, SJ_AL | 0x05800000u | (rn << 16) | (rd << 12) | off); }
/* [rn, rm, lsl #sh] */
static void sj_ldrr(SjEmit *e, unsigned rd, unsigned rn, unsigned rm, unsigned sh) { sj(e, SJ_AL | 0x07900000u | (rn << 16) | (rd << 12) | (sh << 7) | rm); }
static void sj_strr(SjEmit *e, unsigned rd, unsigned rn, unsigned rm, unsigned sh) { sj(e, SJ_AL | 0x07800000u | (rn << 16) | (rd << 12) | (sh << 7) | rm); }
static void sj_ldrshr(SjEmit *e, unsigned rd, unsigned rn, unsigned rm) { sj(e, SJ_AL | 0x019000F0u | (rn << 16) | (rd << 12) | rm); }
static void sj_ldrhr(SjEmit *e, unsigned rd, unsigned rn, unsigned rm) { sj(e, SJ_AL | 0x019000B0u | (rn << 16) | (rd << 12) | rm); }
static void sj_sbfx(SjEmit *e, unsigned rd, unsigned rn, unsigned lsb, unsigned width)
{
   sj(e, SJ_AL | 0x07A00050u | ((width - 1) << 16) | (rd << 12) | (lsb << 7) | rn);
}
static void sj_ubfx(SjEmit *e, unsigned rd, unsigned rn, unsigned lsb, unsigned width)
{
   sj(e, SJ_AL | 0x07E00050u | ((width - 1) << 16) | (rd << 12) | (lsb << 7) | rn);
}
static void sj_call(SjEmit *e, const void *fn)
{
   sj_const(e, SIP, (u32)(uintptr_t)fn);
   sj(e, SJ_AL | 0x012FFF30u | SIP);                                   /* BLX ip */
}

/* Translates k into buffer b; 0 when it does not fit or has no fixed point. */
static int ScspDspJitTranslate(const ScspDspJitKey *k, int b)
{
   const int n = ScspDspDecodeOps(k->mpro, k->coef, k->madrs, sjit.op);
   const ScspDspOp *ops = sjit.op;
   const u8 *const out_l = k->out_l, *const out_r = k->out_r;
   u32 *const mem = sjit.mem + b * (SJIT_BUF_BYTES / 4);
   u8 do_read[128], do_write[128], do_addr[128];
   int rp, wp, i, ch, pass;
   SjEmit e;
   u32 *loop;

   /* Pending state on entry to step 0: iterate the program to a fixed point. */
   rp = wp = 0;
   for (pass = 0; pass < 8; pass++)
   {
      const int rp0 = rp, wp0 = wp;
      for (i = 0; i < n; i++)
      {
         do_read[i] = (u8)rp;
         do_write[i] = 0;
         if (rp) rp = 0;
         else if (wp) wp = 0, do_write[i] = 1;
         do_addr[i] = ops[i].mem || wp;
         if (ops[i].mrd) rp = 1 + ops[i].nofl;
         if (ops[i].mwt) wp = 1;
      }
      if (rp == rp0 && wp == wp0)
         break;
   }
   if (!n || pass == 8)
      return 0;
   sjit.pend_rp[b] = (u8)rp;
   sjit.pend_wp[b] = (u8)wp;

   e.cur = mem;
   e.end = mem + SJIT_BUF_BYTES / 4;
   e.overflow = 0;
   ScspDspJitWriteBegin(mem, SJIT_BUF_BYTES);
   sj(&e, SJ_AL | 0x092D4FF8u);                                        /* PUSH {r3-r11, lr} */
   sj_mov(&e, SR4, SR0, SJ_LSL, 0);
   sj_ldr(&e, SR5, SR4, JOFF(shift_reg));
   sj_ldr(&e, SR6, SR4, JOFF(y_reg));
   sj_ldr(&e, SR7, SR4, JOFF(frc_reg));
   sj_ldr(&e, SR8, SR4, JOFF(adrs_reg));
   sj_ldr(&e, SR9, SR4, JOFF(mdec_ct));
   sj_ldr(&e, SR10, SR4, JOFF(inputs));
   sj_ldr(&e, SR11, SR4, JOFF(pos0));
   loop = e.cur;
   for (i = 0; i < n; i++)
   {
      const ScspDspOp *o = &ops[i];
      const int need_in = (o->need_prod && o->xsel) || (o->adrl && !o->adr_hi);
      const int need_t = o->need_prod && (!o->xsel || o->sga == 1);
      const int y_const = o->ysel == 1;

      /* INPUTS (r10) */
      if (o->in_kind == 0)
         sj_ldr(&e, SR10, SR4, JOFF(mems) + 4 * o->in_idx);
      else if (o->in_kind == 1)
      {
         sj_ldr(&e, SR0, SR4, JOFF(mix) + 4 * o->in_idx);
         sj_ldrr(&e, SR0, SR0, SR11, 2);
         sj_mov(&e, SR10, SR0, SJ_LSL, 8);
      }
      else if (o->in_kind == 2)
      {
         sj_ldr(&e, SR0, SR4, JOFF(ext) + 4 * o->in_idx);
         sj_dpr(&e, SJ_ADD, SR11, SR1, SR11, SJ_LSL, 0);
         sj_ldrshr(&e, SR0, SR0, SR1);
         sj_mov(&e, SR10, SR0, SJ_LSL, 8);
      }
      /* MEMS write from the last read, then the pending memory access */
      if (o->iwt)
      {
         sj_ldr(&e, SR0, SR4, JOFF(read_value));
         sj_str(&e, SR0, SR4, JOFF(mems) + 4 * o->iwa);
      }
      if (do_read[i])
      {
         sj_ldr(&e, SR1, SR4, JOFF(ram));
         sj_ldr(&e, SR2, SR4, JOFF(io_addr));
         sj_dpr(&e, SJ_ADD, SR2, SR2, SR2, SJ_LSL, 0);
         sj_ldrhr(&e, SR0, SR1, SR2);
         if (do_read[i] == 2)
            sj_mov(&e, SR0, SR0, SJ_LSL, 8);
         else
            sj_call(&e, (const void *)ScspDspJitF2I);
         sj_str(&e, SR0, SR4, JOFF(read_value));
      }
      else if (do_write[i])
      {
         sj_mov(&e, SR0, SR4, SJ_LSL, 0);
         sj_call(&e, (const void *)ScspDspJitWrite);
      }
      /* sign-extended INPUTS (r1), TEMP (r2), Y (r3) */
      if (need_in)
         sj_sbfx(&e, SR1, SR10, 0, 24);
      if (need_t)
      {
         sj_dpi(&e, SJ_AL, SJ_ADD, SR9, SR2, o->tra);
         sj_dpi(&e, SJ_AL, SJ_AND, SR2, SR2, 0x7F);
         sj_ldrr(&e, SR2, SR4, SR2, 2);
         sj_sbfx(&e, SR2, SR2, 0, 24);
      }
      if (o->need_prod && !(y_const && o->coef == 0))
      {
         switch (o->ysel)
         {
         case 0: sj_sbfx(&e, SR3, SR7, 0, 13); break;
         case 1: sj_const(&e, SR3, (u32)o->coef); break;
         case 2: sj_sbfx(&e, SR3, SR6, 11, 13); break;
         default: sj_ubfx(&e, SR3, SR6, 4, 12); break;
         }
      }
      if (o->yrl)
         sj_dpi(&e, SJ_AL, SJ_BIC, SR10, SR6, 0xFF000000u);
      /* shifter output (ip) */
      if (o->use_so)
      {
         sj_sbfx(&e, SIP, SR5, 0, 26);
         if (o->sat)
            sj(&e, SJ_AL | 0x06A00010u | (23u << 16) | (SIP << 12) | ((u32)o->shl << 7) | SIP); /* SSAT ip, #24, ip, lsl #shl */
         else if (o->shl)
            sj_mov(&e, SIP, SIP, SJ_LSL, 1);
         sj_dpi(&e, SJ_AL, SJ_BIC, SIP, SIP, 0xFF000000u);
         if (o->ewt)
         {
            sj_sbfx(&e, SR0, SIP, 8, 16);
            sj_str(&e, SR0, SR4, JOFF(efreg) + 4 * o->ewa);
         }
         if (o->twt)
         {
            sj_dpi(&e, SJ_AL, SJ_ADD, SR9, SR0, o->twa);
            sj_dpi(&e, SJ_AL, SJ_AND, SR0, SR0, 0x7F);
            sj_strr(&e, SIP, SR4, SR0, 2);
         }
         if (o->frcl)
         {
            if (o->frc_lo) sj_ubfx(&e, SR7, SIP, 0, 12);
            else sj_mov(&e, SR7, SIP, SJ_LSR, 11);
         }
      }
      /* SHIFTED = (Y * X >> 12) +- SGA */
      if (o->need_prod)
      {
         const unsigned x = o->xsel ? SR1 : SR2;
         if (y_const && o->coef == 0)
            sj_dpi(&e, SJ_AL, SJ_MOV, 0, SR0, 0);
         else
         {
            sj(&e, SJ_AL | 0x00C00090u | (SLR << 16) | (SR0 << 12) | (x << 8) | SR3); /* SMULL r0, lr, r3, x */
            sj_mov(&e, SR0, SR0, SJ_LSR, 12);
            sj_dpr(&e, SJ_ORR, SR0, SR0, SLR, SJ_LSL, 20);
         }
         if (o->sga)
            sj_dpr(&e, o->negb ? SJ_SUB : SJ_ADD, SR0, SR5, o->sga == 2 ? SR5 : SR2, SJ_LSL, 0);
         else
            sj_mov(&e, SR5, SR0, SJ_LSL, 0);
         sj_dpi(&e, SJ_AL, SJ_BIC, SR5, SR5, 0xFC000000u);
      }
      /* memory address, ADRS_REG, memory write data */
      if (do_addr[i])
      {
         sj_const(&e, SR0, o->base);
         if (o->adreb)
         {
            sj_sbfx(&e, SR3, SR8, 0, 12);
            sj_dpr(&e, SJ_ADD, SR0, SR0, SR3, SJ_LSL, 0);
         }
         if (!o->table)
         {
            sj_dpr(&e, SJ_ADD, SR0, SR0, SR9, SJ_LSL, 0);
            sj_ubfx(&e, SR0, SR0, 0, 13 + k->rbl);
         }
         else
            sj_ubfx(&e, SR0, SR0, 0, 16);
         if (k->rbp)
            sj_dpi(&e, SJ_AL, SJ_ADD, SR0, SR0, (u32)k->rbp << 12);
         sj_ubfx(&e, SR0, SR0, 0, 18);
         sj_str(&e, SR0, SR4, JOFF(io_addr));
      }
      if (o->adrl)
      {
         if (o->adr_hi) sj_mov(&e, SR8, SIP, SJ_LSR, 12);
         else sj_ubfx(&e, SR8, SR1, 16, 12);
      }
      if (o->mwt)
      {
         if (o->nofl)
            sj_mov(&e, SR0, SIP, SJ_LSR, 8);
         else
         {
            sj_mov(&e, SR0, SIP, SJ_LSL, 0);
            sj_call(&e, (const void *)ScspDspJitI2F);
         }
         sj_str(&e, SR0, SR4, JOFF(write_value));
      }
   }
   /* MDEC_CT */
   sj_dpi(&e, SJ_AL, SJ_CMP, SR9, 0, 0);
   sj_dpi(&e, SJ_EQ, SJ_MOV, 0, SR9, 0x2000u << k->rbl);
   sj_dpi(&e, SJ_AL, SJ_SUB, SR9, SR9, 1);
   /* EFREG out */
   for (ch = 0; ch < 2; ch++)
   {
      const u8 *lv = ch ? out_r : out_l;
      int any = 0;
      for (i = 0; i < 16; i++) any |= lv[i] != 31;
      if (!any) continue;
      sj_ldr(&e, SR1, SR4, ch ? JOFF(bufR) : JOFF(bufL));
      sj_ldrr(&e, SR2, SR1, SR11, 2);
      for (i = 0; i < 16; i++)
         if (lv[i] != 31)
         {
            sj_ldr(&e, SR0, SR4, JOFF(efreg) + 4 * i);
            sj_dpr(&e, SJ_ADD, SR2, SR2, SR0, SJ_ASR, lv[i]);
         }
      sj_strr(&e, SR2, SR1, SR11, 2);
   }
   /* next sample */
   sj_dpi(&e, SJ_AL, SJ_ADD, SR11, SR11, 1);
   sj_ldr(&e, SR0, SR4, JOFF(len));
   sj_dpr(&e, SJ_CMP, SR11, 0, SR0, SJ_LSL, 0);
   sj(&e, SJ_LT | 0x0A000000u | ((u32)(loop - (e.cur + 2)) & 0xFFFFFF));
   sj_str(&e, SR5, SR4, JOFF(shift_reg));
   sj_str(&e, SR6, SR4, JOFF(y_reg));
   sj_str(&e, SR7, SR4, JOFF(frc_reg));
   sj_str(&e, SR8, SR4, JOFF(adrs_reg));
   sj_str(&e, SR9, SR4, JOFF(mdec_ct));
   sj_str(&e, SR10, SR4, JOFF(inputs));
   sj(&e, SJ_AL | 0x08BD8FF8u);                                        /* POP {r3-r11, pc} */
   ScspDspJitWriteEnd(mem, SJIT_BUF_BYTES);
   return !e.overflow;
}

/* Emulation thread, once per frame. */
void ScspDspJitService(void)
{
   int b;
   if (atomic_load_explicit(&sjit.state, memory_order_acquire) != SJIT_REQUEST)
      return;
   b = sjit.active == 0;
   if (!sjit.mem && !sjit.failed && !(sjit.mem = ScspDspJitAlloc()))
      sjit.failed = 1;
   sjit.key[b] = sjit.req;
   sjit.fn[b] = !sjit.failed && ScspDspJitTranslate(&sjit.req, b)
                   ? (ScspDspJitFn)(uintptr_t)(sjit.mem + b * (SJIT_BUF_BYTES / 4)) : NULL;
   sjit.ready_buf = b;
#ifdef VITA
   {
      extern void YuiMsg(const char *, ...);
      static unsigned count;
      if (++count <= 64)
         YuiMsg("scsp_dsp_jit buffer=%d translated=%d", b, sjit.fn[b] != NULL);
   }
#endif
   atomic_store_explicit(&sjit.state, SJIT_READY, memory_order_release);
}

static int ScspDspJitRun(ScspDsp *dsp, u16 *ram, const s32 *const mix[16], const s16 *const ext[2],
                         u32 len, s32 *bufL, s32 *bufR, const u8 out_l[16], const u8 out_r[16],
                         u32 dirty[4])
{
   static ScspDspJitCtx c;
   static ScspDspJitKey k;
   int state = atomic_load_explicit(&sjit.state, memory_order_acquire);
   u32 pos = 0;
   int i, a;

   memcpy(k.mpro, dsp->mpro, sizeof(k.mpro));
   memcpy(k.coef, dsp->coef, sizeof(k.coef));
   memcpy(k.madrs, dsp->madrs, sizeof(k.madrs));
   k.rbl = dsp->rbl;
   k.rbp = dsp->rbp;
   memcpy(k.out_l, out_l, 16);
   memcpy(k.out_r, out_r, 16);
   if (state == SJIT_READY)
   {
      if (!memcmp(&sjit.key[sjit.ready_buf], &k, sizeof(k)))
      {
         /* Written on another core: this core's instruction cache may hold
          * the buffer's previous code. */
         sjit.active = sjit.ready_buf;
         if (sjit.fn[sjit.active])
            ScspDspJitAdopt(sjit.mem + sjit.active * (SJIT_BUF_BYTES / 4), SJIT_BUF_BYTES);
      }
      atomic_store_explicit(&sjit.state, state = SJIT_IDLE, memory_order_relaxed);
   }
   a = sjit.active;
   if (a < 0 || memcmp(&sjit.key[a], &k, sizeof(k)))
   {
      if (state == SJIT_IDLE)
      {
         sjit.req = k;
         atomic_store_explicit(&sjit.state, SJIT_REQUEST, memory_order_release);
      }
      return 0;
   }
   if (!sjit.fn[a])
      return 0;
   /* Reach the translated pending state with the C steps. */
   while (pos < len && (dsp->read_pending != sjit.pend_rp[a] || dsp->write_pending != sjit.pend_wp[a]))
   {
      ScspDspRunC(dsp, ram, mix, ext, pos, pos + 1, bufL, bufR, out_l, out_r, dirty);
      pos++;
   }
   if (pos >= len)
      return 1;

   memcpy(c.temp, dsp->temp, sizeof(c.temp));
   memcpy(c.mems, dsp->mems, sizeof(c.mems));
   for (i = 0; i < 16; i++) c.efreg[i] = dsp->efreg[i];
   c.read_value = dsp->read_value;
   c.io_addr = dsp->io_addr;
   c.write_value = dsp->write_value;
   c.ram = ram;
   for (i = 0; i < 16; i++) c.mix[i] = mix[i];
   c.ext[0] = ext[0];
   c.ext[1] = ext[1];
   c.bufL = bufL;
   c.bufR = bufR;
   c.len = len;
   c.pos0 = pos;
   c.shift_reg = dsp->shift_reg;
   c.y_reg = (u32)dsp->y_reg;
   c.frc_reg = dsp->frc_reg;
   c.adrs_reg = dsp->adrs_reg;
   c.mdec_ct = dsp->mdec_ct;
   c.inputs = (u32)dsp->inputs;
   memcpy(c.dirty, dirty, sizeof(c.dirty));

   sjit.fn[a](&c);

   memcpy(dsp->temp, c.temp, sizeof(c.temp));
   memcpy(dsp->mems, c.mems, sizeof(c.mems));
   for (i = 0; i < 16; i++) dsp->efreg[i] = (s16)c.efreg[i];
   dsp->read_value = c.read_value;
   dsp->io_addr = c.io_addr;
   dsp->write_value = c.write_value;
   dsp->shift_reg = c.shift_reg;
   dsp->y_reg = (s32)c.y_reg;
   dsp->frc_reg = (u16)c.frc_reg;
   dsp->adrs_reg = (u16)c.adrs_reg;
   dsp->mdec_ct = c.mdec_ct;
   dsp->inputs = (s32)c.inputs;
   dsp->read_pending = sjit.pend_rp[a];
   dsp->write_pending = sjit.pend_wp[a];
   memcpy(dirty, c.dirty, sizeof(c.dirty));
   return 1;
}
#endif
#endif
