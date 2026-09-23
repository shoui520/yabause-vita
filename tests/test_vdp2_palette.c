/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the production pixel fetcher; unused frontend sections are discarded. */
#include "../src/core/vidsoft.c"
#include <assert.h>
#include <stdio.h>
Vdp2Internal_struct Vdp2Internal;

static void test_plane(void *p, int plane, Vdp2 *regs) {
   (void)regs;
   ((vdp2draw_struct *)p)->addr = plane * 0x10000;
}

/* Independent manual oracle, not parity against the same fetcher. VDP2
 * ST-58-R2 table 4.3: the upper five bits cannot make palette zero opaque. */
static unsigned test_indexed_transparency(u8 *ram, u8 *cram) {
   unsigned checks = 0;
   u32 palette[2048];
   for (unsigned mode = 0; mode < 3; ++mode) {
      Vdp2Internal.ColorMode = mode;
      for (unsigned i = 0; i < 2048; ++i)
         palette[i] = Vdp2ColorRamGetColorSoft(i, cram);
      for (unsigned transparent = 0; transparent < 2; ++transparent)
      for (unsigned cached = 0; cached < 2; ++cached)
      for (unsigned value = 0; value < 65536; ++value) {
         vdp2draw_struct info = {0};
         info.colornumber = 2; info.cellw = 8;
         info.transparencyenable = transparent;
         info.coloroffset = 0x700;
         T1WriteWord(ram, 0, value);
         u32 color = 0x12345678, dot = 0;
         int visible = Vdp2FetchPixel(&info, 0, 0, &color, &dot,
            ram, 0, 0, cram, cached ? palette : NULL);
         int expected = !transparent || (value & 0x7ff) != 0;
         assert(visible == expected && dot == value);
         if (visible)
            assert(color == Vdp2ColorRamGetColorSoft(
               (0x700 + (value & 0x7ff)) & 0x7ff, cram));
         else
            assert(color == 0x12345678);
         ++checks;
      }
   }
   return checks;
}

static unsigned test_row_runs(u8 *ram) {
   unsigned checks = 0;
   Vdp2 regs = {0};
   for (int pattern = 0; pattern < 2; ++pattern)
   for (int word = 0; word < 2; ++word)
   for (int plane = 0; plane < 4; ++plane)
   for (int format = 0; format < 5; ++format)
   for (int offset = 0; offset < 16; ++offset) {
      vdp2draw_struct initial = {0};
      initial.mapwh = 2; initial.colornumber = format;
      ReadPlaneSize(&initial, plane);
      ReadPatternData(&initial, (word ? 0x8000 : 0) | 0x23f, pattern);
      for (int line = 0; line < 32; ++line) {
         vdp2draw_struct reference = initial, candidate = initial;
         screeninfo_struct rs = {0}, cs = {0};
         SetupScreenVars(&reference, &rs, test_plane, &regs);
         SetupScreenVars(&candidate, &cs, test_plane, &regs);
         Vdp2RowRun run = {0};
         for (int i = 0; i < 704; ++i) {
            /* Window gaps must not consume mapping state or cross tile runs. */
            if ((i % 17) < 3) continue;
            int sx = (i + rs.xmask - 20 + offset) & rs.xmask;
            int sy = (line + rs.ymask - 16) & rs.ymask;
            int rx = sx, ry = sy;
            Vdp2MapCalcXY(&reference, &rx, &ry, &rs, &regs, ram, 0);
            if (i >= run.end)
               Vdp2BeginRowRun(&run, i, sx, sy, &candidate, &cs, &regs, ram);
            assert(rx == run.x + (i - run.start) * run.step && ry == run.y);
            assert(!memcmp(&reference, &candidate, sizeof(reference)));
            assert(!memcmp(&rs, &cs, sizeof(rs)));
            ++checks;
         }
      }
   }
   return checks;
}

int main(void) {
   u8 *cram = malloc(4096), *vram = malloc(0x80000);
   assert(cram && vram);
   u32 palette[2048];
   unsigned checks = 0;
   for (unsigned i = 0; i < 4096; ++i) cram[i] = (i * 79 + (i >> 3)) & 255;
   for (unsigned i = 0; i < 0x80000; ++i) vram[i] = (i * 31 + (i >> 8)) & 255;
   for (unsigned mode = 0; mode < 4; ++mode) {
      Vdp2Internal.ColorMode = mode;
      for (unsigned p = 0; p < 2048; ++p)
         palette[p] = Vdp2ColorRamGetColorSoft(p, cram);
      for (unsigned address = 0; address < 0x20000; ++address) {
         assert(Vdp2LookupPalette(address, cram, palette) == Vdp2LookupPalette(address, cram, NULL));
         ++checks;
      }
      for (unsigned format = 0; format < 5; ++format)
      for (unsigned transparent = 0; transparent < 2; ++transparent)
      for (unsigned offset = 0; offset <= 0x700; offset += 0x100)
      for (unsigned pos = 0; pos < 2048; ++pos) {
         vdp2draw_struct info = {0};
         info.colornumber = format; info.transparencyenable = transparent;
         info.coloroffset = offset; info.cellw = 8;
         u32 a = 0x12345678, b = a, da = 0, db = 0;
         int va = Vdp2FetchPixel(&info, pos & 7, pos >> 3, &a, &da, vram, 0x7ff00, 0x7f0, cram, NULL);
         int vb = Vdp2FetchPixel(&info, pos & 7, pos >> 3, &b, &db, vram, 0x7ff00, 0x7f0, cram, palette);
         assert(va == vb && a == b && da == db);
         ++checks;
      }
   }
   printf("palette pixel parity: %u checks passed\n", checks);
   printf("tile row mapping parity: %u checks passed\n", test_row_runs(vram));
   printf("indexed transparency manual oracle: %u checks passed\n",
      test_indexed_transparency(vram, cram));
   free(cram); free(vram);
}
