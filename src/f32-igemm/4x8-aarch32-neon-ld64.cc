// Copyright 2022 Google LLC
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.


#include <cassert>

#include <xnnpack/aarch32-assembler.h>
#include <xnnpack/allocator.h>
#include <xnnpack/igemm.h>

namespace xnnpack {
namespace aarch32 {
namespace {
class Generator : public Assembler {
  using Assembler::Assembler;
 public:
  void generate(size_t nc_mod_nr, size_t kc, const void* params);
};


// void xnn_f32_igemm_minmax_ukernel_4x8__aarch32_neon_ld64(
//     size_t mr,                            r0
//     size_t nc,                            r1
//     size_t kc,                            r2 -> r5 -> sp + 68
//     size_t ks,                            r3 -> sp + 72 -> r14
//     const float**restrict a,  sp + 112 -> r2
//     const void*restrict w,    sp + 116 -> r9
//     uint8_t*restrict c,       sp + 120 -> r11
//     size_t cm_stride,         sp + 124 -> (r6)
//     size_t cn_stride,         sp + 128 -> (r7)
//     size_t a_offset,          sp + 132 -> (r5)
//     const float* zero,        sp + 136 -> (r7)
//     minmax_params*params,     sp + 140 -> (r5)

// d8-d15, r4-r11,r14(lr) need to be preserved if used. r13(sp),r15(pc) are reserved.

// Register usage

// A0   r3  d0
// A1  r12  d1
// A2  r10  d2
// A3   r0  d3

// B    r9  d8,  d9, d10, d11
// B       d12, d13, d14, d15

// C0  r11 d16-d17  q8  d18-d19  q9
// C1   r4 d20-d21 q10  d22-d23 q11
// C2   r8 d24-d25 q12  d26-d27 q13
// C3   r6 d28-d29 q14  d30-d31 q15

// Clamp (r5) d4 d5 d6 d7

// Converted from: src/f32-igemm/gen/4x8-minmax-aarch32-neon-ld64.S
void Generator::generate(size_t nc_mod_nr, size_t kc, const void* params)
{
  assert(nc_mod_nr < 8);
  assert(kc != 0);
  assert(kc % sizeof(float) == 0);

  Label l0, l1, l2, l3, l4, l5, l6, l7, l8;

  // Push 112 bytes
  // r2 will be reloaded in outer loop.  r3 is ks
  push({r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}); // +44
  sub(sp, sp, 4); // 4
  vpush({d8-d15}); // +64 = 112

  ldr(r11, mem[sp, 120]); // c
  ldr(r6, mem[sp, 124]); // cm_stride
  ldr(r2, mem[sp, 112]); // a
  ldr(r9, mem[sp, 116]); // w
  ldr(r5, mem[sp, 140]); // params
  mov(r14, r3); // p = ks

  // Clamp C pointers
  cmp(r0, 2); // if mr >= 2
  add(r4, r11, r6); //   c1 = c0 + cm_stride
  movlo(r4, r11); // c1
  // if mr > 2
  add(r8, r4, r6); //   c2 = c1 + cm_stride
  movls(r8, r4); // c2
  cmp(r0, 4); // if mr >=4
  add(r6, r8, r6); //   c3 = c2 + cm_stride
  movlo(r6, r8); // c3

  // Load min/max values
  vld1r_32({d4, d5}, mem[r5]++);
  vld1r_32({d6, d7}, mem[r5]);

  bind(l0);
  // Load initial bias from w into accumulators
  vldm(mem[r9]++, {d16-d19}); // Bias
  vmov(q10, q8);
  vmov(q11, q9);
  vmov(q12, q8);
  vmov(q13, q9);
  vmov(q14, q8);
  vmov(q15, q9);

  bind(l1);
  // Load next 4 A pointers
  ldr(r3, mem[r2, 0]);
  ldr(r12, mem[r2, 4]);
  ldr(r10, mem[r2, 8]);
  ldr(r0, mem[r2, 12]);
  add(r2, r2, 16);

  // Add a_offset
  ldr(r5, mem[sp, 132]); // a_offset
  ldr(r7, mem[sp, 136]); // zero
  cmp(r3, r7); // if a0 == zero
  add(r3, r3, r5); // a0 += a_offset
  moveq(r3, r7); //   a0 = zero, else += a0 + a_offset
  cmp(r12, r7); // if a1 == zero
  add(r12, r12, r5); // a1 += a_offset
  moveq(r12, r7); //   a1 = zero, else += a1 + a_offset
  cmp(r10, r7); // if a2 == zero
  add(r10, r10, r5); // a2 += a_offset
  moveq(r10, r7); //   a2 = zero, else += a2 + a_offset
  cmp(r0, r7); // if a3 == zero
  add(r0, r0, r5); // a3 += a_offset
  ldr(r5, mem[sp, 68]); // kc
  moveq(r0, r7); //   a3 = zero, else += a3 + a_offset


  subs(r5, r5, 8); // kc - 8
  blo(l4); // less than 2 channels?

  // Main loop - 2 floats of A (8 bytes)
  bind(l2);
  vld1_32({d0}, mem[r3]++); // A0
  vldm(mem[r9]++, {d8-d11}); // B0
  vld1_32({d1}, mem[r12]++); // A1
  vld1_32({d2}, mem[r10]++); // A2
  vld1_32({d3}, mem[r0]++); // A3
  vldm(mem[r9]++, {d12-d15}); // B1

  vmla_f32(q8, q4, d0[0]);
  vmla_f32(q9, q5, d0[0]);
  vmla_f32(q10, q4, d1[0]);
  vmla_f32(q11, q5, d1[0]);
  vmla_f32(q12, q4, d2[0]);
  vmla_f32(q13, q5, d2[0]);
  vmla_f32(q14, q4, d3[0]);
  vmla_f32(q15, q5, d3[0]);
  vmla_f32(q8, q6, d0[1]);
  vmla_f32(q9, q7, d0[1]);
  vmla_f32(q10, q6, d1[1]);
  vmla_f32(q11, q7, d1[1]);
  subs(r5, r5, 8);
  vmla_f32(q12, q6, d2[1]);
  vmla_f32(q13, q7, d2[1]);
  vmla_f32(q14, q6, d3[1]);
  vmla_f32(q15, q7, d3[1]);
  bhs(l2);

  // Is there a remainder?- 1 float of A (4 bytes)
  tst(r5, 4);
  bne(l4);

  bind(l3);
  // ks loop
  subs(r14, r14, 16); // ks -= MR * sizeof(void*)
  bhi(l1);

  ldr(r7, mem[sp, 128]); // cn_stride
  ldr(r14, mem[sp, 72]); // p = ks

  // Clamp
  vmax_f32(q8, q8, q2);
  subs(r1, r1, 8);
  vmax_f32(q9, q9, q2);
  vmax_f32(q10, q10, q2);
  vmax_f32(q11, q11, q2);
  vmax_f32(q12, q12, q2);
  vmax_f32(q13, q13, q2);
  vmax_f32(q14, q14, q2);
  vmax_f32(q15, q15, q2);
  vmin_f32(q8, q8, q3);
  vmin_f32(q9, q9, q3);
  vmin_f32(q10, q10, q3);
  vmin_f32(q11, q11, q3);
  vmin_f32(q12, q12, q3);
  vmin_f32(q13, q13, q3);
  vmin_f32(q14, q14, q3);
  vmin_f32(q15, q15, q3);

  // Store full 4 x 8
  blo(l5);
  vst1_32({d28-d31}, mem[r6], r7);
  vst1_32({d24-d27}, mem[r8], r7);
  vst1_32({d20-d23}, mem[r4], r7);
  vst1_32({d16-d19}, mem[r11], r7);
  sub(r2, r2, r14); // a -= ks
  bhi(l0);

  vpop({d8-d15});
  add(sp, sp, 12); // skip pad, r2, r3
  pop({r4, r5, r6, r7, r8, r9, r10, r11, pc});

  bind(l4);
  // Remainder- 1 float of A (4 bytes)
  vldm(mem[r3]++, {s0}); // A0
  vldm(mem[r9]++, {d8-d11}); // B0
  vldm(mem[r12]++, {s2}); // A1
  vldm(mem[r10]++, {s4}); // A2
  vldm(mem[r0]++, {s6}); // A3
  vmla_f32(q8, q4, d0[0]);
  vmla_f32(q9, q5, d0[0]);
  vmla_f32(q10, q4, d1[0]);
  vmla_f32(q11, q5, d1[0]);
  vmla_f32(q12, q4, d2[0]);
  vmla_f32(q13, q5, d2[0]);
  vmla_f32(q14, q4, d3[0]);
  vmla_f32(q15, q5, d3[0]);
  b(l3);

  // Store odd width
  bind(l5);
  tst(r1, 4);
  beq(l6);
  vst1_32({d28-d29}, mem[r6]++);
  vst1_32({d24-d25}, mem[r8]++);
  vmov(q14, q15);
  vmov(q12, q13);
  vst1_32({d20-d21}, mem[r4]++);
  vst1_32({d16-d17}, mem[r11]++);
  vmov(q10, q11);
  vmov(q8, q9);

  bind(l6);
  tst(r1, 2);
  beq(l7);
  vst1_32({d28}, mem[r6]++);
  vst1_32({d24}, mem[r8]++);
  vmov(d28, d29);
  vmov(d24, d25);
  vst1_32({d20}, mem[r4]++);
  vst1_32({d16}, mem[r11]++);
  vmov(d20, d21);
  vmov(d16, d17);

  bind(l7);
  tst(r1, 1);
  beq(l8);
  vst1_32({d28[0]}, mem[r6]++);
  vst1_32({d24[0]}, mem[r8]++);
  vst1_32({d20[0]}, mem[r4]++);
  vst1_32({d16[0]}, mem[r11]++);

  bind(l8);
  vpop({d8-d15});
  add(sp, sp, 12); // skip pad, r2, r3
  pop({r4, r5, r6, r7, r8, r9, r10, r11, pc});
}
}  // namespace
}  // aarch32
}  // xnnpack

xnn_status xnn_generate_f32_igemm_ukernel_4x8__aarch32_neon_ld64(xnn_code_buffer* code, size_t nc_mod_nr, size_t kc, size_t ks, const void* params) {
  using namespace xnnpack::aarch32;
  Generator g(code);
  assert(params != nullptr);
  g.generate(nc_mod_nr, kc, nullptr);
  g.finalize();
  if (g.error() != xnnpack::Error::kNoError) {
    return xnn_status_invalid_state;
  }
  return xnn_status_success;
}
