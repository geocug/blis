/*

   BLIS    
   An object-based framework for developing high-performance BLAS-like
   libraries.

   Copyright (C) 2014, The University of Texas at Austin
   Copyright (C) 2017, Advanced Micro Devices, Inc.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:
    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    - Neither the name of The University of Texas at Austin nor the names
      of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "blis.h"


#define SGEMM_INPUT_GS_BETA_NZ \
	"vmovlps    (%%rcx        ),  %%xmm0,  %%xmm0  \n\t" \
	"vmovhps    (%%rcx,%%rsi,1),  %%xmm0,  %%xmm0  \n\t" \
	"vmovlps    (%%rcx,%%rsi,2),  %%xmm1,  %%xmm1  \n\t" \
	"vmovhps    (%%rcx,%%r13  ),  %%xmm1,  %%xmm1  \n\t" \
	"vshufps    $0x88,   %%xmm1,  %%xmm0,  %%xmm0  \n\t" \
	"vmovlps    (%%rcx,%%rsi,4),  %%xmm2,  %%xmm2  \n\t" \
	"vmovhps    (%%rcx,%%r15  ),  %%xmm2,  %%xmm2  \n\t" \
	/* We can't use vmovhps for loading the last element becauase that
	   might result in reading beyond valid memory. (vmov[lh]psd load
	   pairs of adjacent floats at a time.) So we need to use vmovss
	   instead. But since we're limited to using ymm0 through ymm2
	   (ymm3 contains beta and ymm4 through ymm15 contain the microtile)
	   and due to the way vmovss zeros out all bits above 31, we have to
	   load element 7 before element 6. */ \
	"vmovss     (%%rcx,%%r10  ),  %%xmm1           \n\t" \
	"vpermilps  $0xcf,   %%xmm1,  %%xmm1           \n\t" \
	"vmovlps    (%%rcx,%%r13,2),  %%xmm1,  %%xmm1  \n\t" \
	/*"vmovhps    (%%rcx,%%r10  ),  %%xmm1,  %%xmm1  \n\t"*/ \
	"vshufps    $0x88,   %%xmm1,  %%xmm2,  %%xmm2  \n\t" \
	"vperm2f128 $0x20,   %%ymm2,  %%ymm0,  %%ymm0  \n\t"

#define SGEMM_OUTPUT_GS_BETA_NZ \
	"vextractf128  $1, %%ymm0,  %%xmm2           \n\t" \
	"vmovss            %%xmm0, (%%rcx        )   \n\t" \
	"vpermilps  $0x39, %%xmm0,  %%xmm1           \n\t" \
	"vmovss            %%xmm1, (%%rcx,%%rsi,1)   \n\t" \
	"vpermilps  $0x39, %%xmm1,  %%xmm0           \n\t" \
	"vmovss            %%xmm0, (%%rcx,%%rsi,2)   \n\t" \
	"vpermilps  $0x39, %%xmm0,  %%xmm1           \n\t" \
	"vmovss            %%xmm1, (%%rcx,%%r13  )   \n\t" \
	"vmovss            %%xmm2, (%%rcx,%%rsi,4)   \n\t" \
	"vpermilps  $0x39, %%xmm2,  %%xmm1           \n\t" \
	"vmovss            %%xmm1, (%%rcx,%%r15  )   \n\t" \
	"vpermilps  $0x39, %%xmm1,  %%xmm2           \n\t" \
	"vmovss            %%xmm2, (%%rcx,%%r13,2)   \n\t" \
	"vpermilps  $0x39, %%xmm2,  %%xmm1           \n\t" \
	"vmovss            %%xmm1, (%%rcx,%%r10  )   \n\t"

void bli_sgemm_zen_asm_6x16
     (
       dim_t               k,
       float*     restrict alpha,
       float*     restrict a,
       float*     restrict b,
       float*     restrict beta,
       float*     restrict c, inc_t rs_c, inc_t cs_c,
       auxinfo_t* restrict data,
       cntx_t*    restrict cntx
     )
{
	//void*   a_next = bli_auxinfo_next_a( data );
	//void*   b_next = bli_auxinfo_next_b( data );

	uint64_t   k_iter = k / 4;
	uint64_t   k_left = k % 4;

	__asm__ volatile
	(
	"                                            \n\t"
	"vzeroall                                    \n\t" // zero all xmm/ymm registers.
	"                                            \n\t"
	"                                            \n\t"
	"movq                %2, %%rax               \n\t" // load address of a.
	"movq                %3, %%rbx               \n\t" // load address of b.
	//"movq                %9, %%r15               \n\t" // load address of b_next.
	"                                            \n\t"
	"addq           $32 * 4, %%rbx               \n\t"
	"                                            \n\t" // initialize loop by pre-loading
	"vmovaps           -4 * 32(%%rbx), %%ymm0    \n\t"
	"vmovaps           -3 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"movq                %6, %%rcx               \n\t" // load address of c
	"movq                %7, %%rdi               \n\t" // load rs_c
	"leaq        (,%%rdi,4), %%rdi               \n\t" // rs_c *= sizeof(float)
	"                                            \n\t"
	"leaq   (%%rdi,%%rdi,2), %%r13               \n\t" // r13 = 3*rs_c;
	"leaq   (%%rcx,%%r13,1), %%rdx               \n\t" // rdx = c + 3*rs_c;
	"prefetcht0   7 * 8(%%rcx)                   \n\t" // prefetch c + 0*rs_c
	"prefetcht0   7 * 8(%%rcx,%%rdi)             \n\t" // prefetch c + 1*rs_c
	"prefetcht0   7 * 8(%%rcx,%%rdi,2)           \n\t" // prefetch c + 2*rs_c
	"prefetcht0   7 * 8(%%rdx)                   \n\t" // prefetch c + 3*rs_c
	"prefetcht0   7 * 8(%%rdx,%%rdi)             \n\t" // prefetch c + 4*rs_c
	"prefetcht0   7 * 8(%%rdx,%%rdi,2)           \n\t" // prefetch c + 5*rs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"movq      %0, %%rsi                         \n\t" // i = k_iter;
	"testq  %%rsi, %%rsi                         \n\t" // check i via logical AND.
	"je     .SCONSIDKLEFT                        \n\t" // if i == 0, jump to code that
	"                                            \n\t" // contains the k_left loop.
	"                                            \n\t"
	"                                            \n\t"
	".SLOOPKITER:                                \n\t" // MAIN LOOP
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t" // iteration 0
	"prefetcht0   64 * 4(%%rax)                  \n\t"
	"                                            \n\t"
	"vbroadcastss       0 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss       1 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastss       2 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss       3 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastss       4 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss       5 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"vmovaps           -2 * 32(%%rbx), %%ymm0    \n\t"
	"vmovaps           -1 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t" // iteration 1
	"vbroadcastss       6 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss       7 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastss       8 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss       9 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastss      10 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss      11 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"vmovaps            0 * 32(%%rbx), %%ymm0    \n\t"
	"vmovaps            1 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t" // iteration 2
	"prefetcht0   76 * 4(%%rax)                  \n\t"
	"                                            \n\t"
	"vbroadcastss      12 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss      13 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastss      14 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss      15 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastss      16 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss      17 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"vmovaps            2 * 32(%%rbx), %%ymm0    \n\t"
	"vmovaps            3 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t" // iteration 3
	"vbroadcastss      18 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss      19 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastss      20 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss      21 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastss      22 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss      23 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"addq          $4 *  6 * 4, %%rax            \n\t" // a += 4*6  (unroll x mr)
	"addq          $4 * 16 * 4, %%rbx            \n\t" // b += 4*16 (unroll x nr)
	"                                            \n\t"
	"vmovaps           -4 * 32(%%rbx), %%ymm0    \n\t"
	"vmovaps           -3 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"decq   %%rsi                                \n\t" // i -= 1;
	"jne    .SLOOPKITER                          \n\t" // iterate again if i != 0.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".SCONSIDKLEFT:                              \n\t"
	"                                            \n\t"
	"movq      %1, %%rsi                         \n\t" // i = k_left;
	"testq  %%rsi, %%rsi                         \n\t" // check i via logical AND.
	"je     .SPOSTACCUM                          \n\t" // if i == 0, we're done; jump to end.
	"                                            \n\t" // else, we prepare to enter k_left loop.
	"                                            \n\t"
	"                                            \n\t"
	".SLOOPKLEFT:                                \n\t" // EDGE LOOP
	"                                            \n\t"
	"prefetcht0   64 * 4(%%rax)                  \n\t"
	"                                            \n\t"
	"vbroadcastss       0 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss       1 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastss       2 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss       3 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastss       4 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss       5 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"addq          $1 *  6 * 4, %%rax            \n\t" // a += 1*6  (unroll x mr)
	"addq          $1 * 16 * 4, %%rbx            \n\t" // b += 1*16 (unroll x nr)
	"                                            \n\t"
	"vmovaps           -4 * 32(%%rbx), %%ymm0    \n\t"
	"vmovaps           -3 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"decq   %%rsi                                \n\t" // i -= 1;
	"jne    .SLOOPKLEFT                          \n\t" // iterate again if i != 0.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".SPOSTACCUM:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"movq         %4, %%rax                      \n\t" // load address of alpha
	"movq         %5, %%rbx                      \n\t" // load address of beta 
	"vbroadcastss    (%%rax), %%ymm0             \n\t" // load alpha and duplicate
	"vbroadcastss    (%%rbx), %%ymm3             \n\t" // load beta and duplicate
	"                                            \n\t"
	"vmulps           %%ymm0,  %%ymm4,  %%ymm4   \n\t" // scale by alpha
	"vmulps           %%ymm0,  %%ymm5,  %%ymm5   \n\t"
	"vmulps           %%ymm0,  %%ymm6,  %%ymm6   \n\t"
	"vmulps           %%ymm0,  %%ymm7,  %%ymm7   \n\t"
	"vmulps           %%ymm0,  %%ymm8,  %%ymm8   \n\t"
	"vmulps           %%ymm0,  %%ymm9,  %%ymm9   \n\t"
	"vmulps           %%ymm0,  %%ymm10, %%ymm10  \n\t"
	"vmulps           %%ymm0,  %%ymm11, %%ymm11  \n\t"
	"vmulps           %%ymm0,  %%ymm12, %%ymm12  \n\t"
	"vmulps           %%ymm0,  %%ymm13, %%ymm13  \n\t"
	"vmulps           %%ymm0,  %%ymm14, %%ymm14  \n\t"
	"vmulps           %%ymm0,  %%ymm15, %%ymm15  \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"movq                %8, %%rsi               \n\t" // load cs_c
	"leaq        (,%%rsi,4), %%rsi               \n\t" // rsi = cs_c * sizeof(float)
	"                                            \n\t"
	"leaq   (%%rcx,%%rsi,8), %%rdx               \n\t" // load address of c +  8*cs_c;
	"leaq   (%%rcx,%%rdi,4), %%r14               \n\t" // load address of c +  4*rs_c;
	"                                            \n\t"
	"leaq   (%%rsi,%%rsi,2), %%r13               \n\t" // r13 = 3*cs_c;
	"leaq   (%%rsi,%%rsi,4), %%r15               \n\t" // r15 = 5*cs_c;
	"leaq   (%%r13,%%rsi,4), %%r10               \n\t" // r10 = 7*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t" // now avoid loading C if beta == 0
	"                                            \n\t"
	"vxorps    %%ymm0,  %%ymm0,  %%ymm0          \n\t" // set ymm0 to zero.
	"vucomiss  %%xmm0,  %%xmm3                   \n\t" // set ZF if beta == 0.
	"je      .SBETAZERO                          \n\t" // if ZF = 1, jump to beta == 0 case
	"                                            \n\t"
	"                                            \n\t"
	"cmpq       $4, %%rsi                        \n\t" // set ZF if (4*cs_c) == 4.
	"jz      .SROWSTORED                         \n\t" // jump to row storage case
	"                                            \n\t"
	"                                            \n\t"
	"cmpq       $4, %%rdi                        \n\t" // set ZF if (4*cs_c) == 4.
	"jz      .SCOLSTORED                         \n\t" // jump to column storage case
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".SGENSTORED:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	SGEMM_INPUT_GS_BETA_NZ
	"vfmadd213ps      %%ymm4,  %%ymm3,  %%ymm0   \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	SGEMM_INPUT_GS_BETA_NZ
	"vfmadd213ps      %%ymm6,  %%ymm3,  %%ymm0   \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	SGEMM_INPUT_GS_BETA_NZ
	"vfmadd213ps      %%ymm8,  %%ymm3,  %%ymm0   \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	SGEMM_INPUT_GS_BETA_NZ
	"vfmadd213ps      %%ymm10, %%ymm3,  %%ymm0   \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	SGEMM_INPUT_GS_BETA_NZ
	"vfmadd213ps      %%ymm12, %%ymm3,  %%ymm0   \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	SGEMM_INPUT_GS_BETA_NZ
	"vfmadd213ps      %%ymm14, %%ymm3,  %%ymm0   \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	//"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"movq      %%rdx, %%rcx                      \n\t" // rcx = c + 8*cs_c
	"                                            \n\t"
	"                                            \n\t"
	SGEMM_INPUT_GS_BETA_NZ
	"vfmadd213ps      %%ymm5,  %%ymm3,  %%ymm0   \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	SGEMM_INPUT_GS_BETA_NZ
	"vfmadd213ps      %%ymm7,  %%ymm3,  %%ymm0   \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	SGEMM_INPUT_GS_BETA_NZ
	"vfmadd213ps      %%ymm9,  %%ymm3,  %%ymm0   \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	SGEMM_INPUT_GS_BETA_NZ
	"vfmadd213ps      %%ymm11, %%ymm3,  %%ymm0   \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	SGEMM_INPUT_GS_BETA_NZ
	"vfmadd213ps      %%ymm13, %%ymm3,  %%ymm0   \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	SGEMM_INPUT_GS_BETA_NZ
	"vfmadd213ps      %%ymm15, %%ymm3,  %%ymm0   \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	//"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"jmp    .SDONE                               \n\t" // jump to end.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".SROWSTORED:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vfmadd231ps      (%%rcx), %%ymm3,  %%ymm4   \n\t"
	"vmovups          %%ymm4,  (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vfmadd231ps      (%%rdx), %%ymm3,  %%ymm5   \n\t"
	"vmovups          %%ymm5,  (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vfmadd231ps      (%%rcx), %%ymm3,  %%ymm6   \n\t"
	"vmovups          %%ymm6,  (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vfmadd231ps      (%%rdx), %%ymm3,  %%ymm7   \n\t"
	"vmovups          %%ymm7,  (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vfmadd231ps      (%%rcx), %%ymm3,  %%ymm8   \n\t"
	"vmovups          %%ymm8,  (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vfmadd231ps      (%%rdx), %%ymm3,  %%ymm9   \n\t"
	"vmovups          %%ymm9,  (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vfmadd231ps      (%%rcx), %%ymm3,  %%ymm10  \n\t"
	"vmovups          %%ymm10, (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vfmadd231ps      (%%rdx), %%ymm3,  %%ymm11  \n\t"
	"vmovups          %%ymm11, (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vfmadd231ps      (%%rcx), %%ymm3,  %%ymm12  \n\t"
	"vmovups          %%ymm12, (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vfmadd231ps      (%%rdx), %%ymm3,  %%ymm13  \n\t"
	"vmovups          %%ymm13, (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vfmadd231ps      (%%rcx), %%ymm3,  %%ymm14  \n\t"
	"vmovups          %%ymm14, (%%rcx)           \n\t"
	//"addq      %%rdi, %%rcx                      \n\t"
	"vfmadd231ps      (%%rdx), %%ymm3,  %%ymm15  \n\t"
	"vmovups          %%ymm15, (%%rdx)           \n\t"
	//"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"jmp    .SDONE                               \n\t" // jump to end.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".SCOLSTORED:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vbroadcastss      (%%rbx), %%ymm3           \n\t"
	"                                            \n\t"
	"vunpcklps         %%ymm6,  %%ymm4,  %%ymm0  \n\t"
	"vunpcklps         %%ymm10, %%ymm8,  %%ymm1  \n\t"
	"vshufps    $0x4e, %%ymm1,  %%ymm0,  %%ymm2  \n\t"
	"vblendps   $0xcc, %%ymm2,  %%ymm0,  %%ymm0  \n\t"
	"vblendps   $0x33, %%ymm2,  %%ymm1,  %%ymm1  \n\t"
	"                                            \n\t"
	"vextractf128 $0x1, %%ymm0, %%xmm2           \n\t"
	"vfmadd231ps (%%rcx        ),%%xmm3, %%xmm0  \n\t"
	"vfmadd231ps (%%rcx,%%rsi,4),%%xmm3, %%xmm2  \n\t"
	"vmovups           %%xmm0, (%%rcx        )   \n\t" // store ( gamma00..gamma30 )
	"vmovups           %%xmm2, (%%rcx,%%rsi,4)   \n\t" // store ( gamma04..gamma34 )
	"                                            \n\t"
	"vextractf128 $0x1, %%ymm1, %%xmm2           \n\t"
	"vfmadd231ps (%%rcx,%%rsi,1),%%xmm3, %%xmm1  \n\t"
	"vfmadd231ps (%%rcx,%%r15  ),%%xmm3, %%xmm2  \n\t"
	"vmovups           %%xmm1, (%%rcx,%%rsi,1)   \n\t" // store ( gamma01..gamma31 )
	"vmovups           %%xmm2, (%%rcx,%%r15  )   \n\t" // store ( gamma05..gamma35 )
	"                                            \n\t"
	"                                            \n\t"
	"vunpckhps         %%ymm6,  %%ymm4,  %%ymm0  \n\t"
	"vunpckhps         %%ymm10, %%ymm8,  %%ymm1  \n\t"
	"vshufps    $0x4e, %%ymm1,  %%ymm0,  %%ymm2  \n\t"
	"vblendps   $0xcc, %%ymm2,  %%ymm0,  %%ymm0  \n\t"
	"vblendps   $0x33, %%ymm2,  %%ymm1,  %%ymm1  \n\t"
	"                                            \n\t"
	"vextractf128 $0x1, %%ymm0, %%xmm2           \n\t"
	"vfmadd231ps (%%rcx,%%rsi,2),%%xmm3, %%xmm0  \n\t"
	"vfmadd231ps (%%rcx,%%r13,2),%%xmm3, %%xmm2  \n\t"
	"vmovups           %%xmm0, (%%rcx,%%rsi,2)   \n\t" // store ( gamma02..gamma32 )
	"vmovups           %%xmm2, (%%rcx,%%r13,2)   \n\t" // store ( gamma06..gamma36 )
	"                                            \n\t"
	"vextractf128 $0x1, %%ymm1, %%xmm2           \n\t"
	"vfmadd231ps (%%rcx,%%r13  ),%%xmm3, %%xmm1  \n\t"
	"vfmadd231ps (%%rcx,%%r10  ),%%xmm3, %%xmm2  \n\t"
	"vmovups           %%xmm1, (%%rcx,%%r13  )   \n\t" // store ( gamma03..gamma33 )
	"vmovups           %%xmm2, (%%rcx,%%r10  )   \n\t" // store ( gamma07..gamma37 )
	"                                            \n\t"
	"leaq   (%%rcx,%%rsi,8), %%rcx               \n\t" // rcx += 8*cs_c
	"                                            \n\t"
	"vunpcklps         %%ymm14, %%ymm12, %%ymm0  \n\t"
	"vextractf128 $0x1, %%ymm0, %%xmm2           \n\t"
	"vmovlpd   (%%r14        ), %%xmm1,  %%xmm1  \n\t"
	"vmovhpd   (%%r14,%%rsi,1), %%xmm1,  %%xmm1  \n\t"
	"vfmadd231ps       %%xmm1,  %%xmm3,  %%xmm0  \n\t"
	"vmovlpd           %%xmm0, (%%r14        )   \n\t" // store ( gamma40..gamma50 )
	"vmovhpd           %%xmm0, (%%r14,%%rsi,1)   \n\t" // store ( gamma41..gamma51 )
	"vmovlpd   (%%r14,%%rsi,4), %%xmm1,  %%xmm1  \n\t"
	"vmovhpd   (%%r14,%%r15  ), %%xmm1,  %%xmm1  \n\t"
	"vfmadd231ps       %%xmm1,  %%xmm3,  %%xmm2  \n\t"
	"vmovlpd           %%xmm2, (%%r14,%%rsi,4)   \n\t" // store ( gamma44..gamma54 )
	"vmovhpd           %%xmm2, (%%r14,%%r15  )   \n\t" // store ( gamma45..gamma55 )
	"                                            \n\t"
	"vunpckhps         %%ymm14, %%ymm12, %%ymm0  \n\t"
	"vextractf128 $0x1, %%ymm0, %%xmm2           \n\t"
	"vmovlpd   (%%r14,%%rsi,2), %%xmm1,  %%xmm1  \n\t"
	"vmovhpd   (%%r14,%%r13  ), %%xmm1,  %%xmm1  \n\t"
	"vfmadd231ps       %%xmm1,  %%xmm3,  %%xmm0  \n\t"
	"vmovlpd           %%xmm0, (%%r14,%%rsi,2)   \n\t" // store ( gamma42..gamma52 )
	"vmovhpd           %%xmm0, (%%r14,%%r13  )   \n\t" // store ( gamma43..gamma53 )
	"vmovlpd   (%%r14,%%r13,2), %%xmm1,  %%xmm1  \n\t"
	"vmovhpd   (%%r14,%%r10  ), %%xmm1,  %%xmm1  \n\t"
	"vfmadd231ps       %%xmm1,  %%xmm3,  %%xmm2  \n\t"
	"vmovlpd           %%xmm2, (%%r14,%%r13,2)   \n\t" // store ( gamma46..gamma56 )
	"vmovhpd           %%xmm2, (%%r14,%%r10  )   \n\t" // store ( gamma47..gamma57 )
	"                                            \n\t"
	"leaq   (%%r14,%%rsi,8), %%r14               \n\t" // r14 += 8*cs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vunpcklps         %%ymm7,  %%ymm5,  %%ymm0  \n\t"
	"vunpcklps         %%ymm11, %%ymm9,  %%ymm1  \n\t"
	"vshufps    $0x4e, %%ymm1,  %%ymm0,  %%ymm2  \n\t"
	"vblendps   $0xcc, %%ymm2,  %%ymm0,  %%ymm0  \n\t"
	"vblendps   $0x33, %%ymm2,  %%ymm1,  %%ymm1  \n\t"
	"                                            \n\t"
	"vextractf128 $0x1, %%ymm0, %%xmm2           \n\t"
	"vfmadd231ps (%%rcx        ),%%xmm3, %%xmm0  \n\t"
	"vfmadd231ps (%%rcx,%%rsi,4),%%xmm3, %%xmm2  \n\t"
	"vmovups           %%xmm0, (%%rcx        )   \n\t" // store ( gamma00..gamma30 )
	"vmovups           %%xmm2, (%%rcx,%%rsi,4)   \n\t" // store ( gamma04..gamma34 )
	"                                            \n\t"
	"vextractf128 $0x1, %%ymm1, %%xmm2           \n\t"
	"vfmadd231ps (%%rcx,%%rsi,1),%%xmm3, %%xmm1  \n\t"
	"vfmadd231ps (%%rcx,%%r15  ),%%xmm3, %%xmm2  \n\t"
	"vmovups           %%xmm1, (%%rcx,%%rsi,1)   \n\t" // store ( gamma01..gamma31 )
	"vmovups           %%xmm2, (%%rcx,%%r15  )   \n\t" // store ( gamma05..gamma35 )
	"                                            \n\t"
	"                                            \n\t"
	"vunpckhps         %%ymm7,  %%ymm5,  %%ymm0  \n\t"
	"vunpckhps         %%ymm11, %%ymm9,  %%ymm1  \n\t"
	"vshufps    $0x4e, %%ymm1,  %%ymm0,  %%ymm2  \n\t"
	"vblendps   $0xcc, %%ymm2,  %%ymm0,  %%ymm0  \n\t"
	"vblendps   $0x33, %%ymm2,  %%ymm1,  %%ymm1  \n\t"
	"                                            \n\t"
	"vextractf128 $0x1, %%ymm0, %%xmm2           \n\t"
	"vfmadd231ps (%%rcx,%%rsi,2),%%xmm3, %%xmm0  \n\t"
	"vfmadd231ps (%%rcx,%%r13,2),%%xmm3, %%xmm2  \n\t"
	"vmovups           %%xmm0, (%%rcx,%%rsi,2)   \n\t" // store ( gamma02..gamma32 )
	"vmovups           %%xmm2, (%%rcx,%%r13,2)   \n\t" // store ( gamma06..gamma36 )
	"                                            \n\t"
	"vextractf128 $0x1, %%ymm1, %%xmm2           \n\t"
	"vfmadd231ps (%%rcx,%%r13  ),%%xmm3, %%xmm1  \n\t"
	"vfmadd231ps (%%rcx,%%r10  ),%%xmm3, %%xmm2  \n\t"
	"vmovups           %%xmm1, (%%rcx,%%r13  )   \n\t" // store ( gamma03..gamma33 )
	"vmovups           %%xmm2, (%%rcx,%%r10  )   \n\t" // store ( gamma07..gamma37 )
	"                                            \n\t"
	//"leaq   (%%rcx,%%rsi,8), %%rcx               \n\t" // rcx += 8*cs_c
	"                                            \n\t"
	"vunpcklps         %%ymm15, %%ymm13, %%ymm0  \n\t"
	"vextractf128 $0x1, %%ymm0, %%xmm2           \n\t"
	"vmovlpd   (%%r14        ), %%xmm1,  %%xmm1  \n\t"
	"vmovhpd   (%%r14,%%rsi,1), %%xmm1,  %%xmm1  \n\t"
	"vfmadd231ps       %%xmm1,  %%xmm3,  %%xmm0  \n\t"
	"vmovlpd           %%xmm0, (%%r14        )   \n\t" // store ( gamma40..gamma50 )
	"vmovhpd           %%xmm0, (%%r14,%%rsi,1)   \n\t" // store ( gamma41..gamma51 )
	"vmovlpd   (%%r14,%%rsi,4), %%xmm1,  %%xmm1  \n\t"
	"vmovhpd   (%%r14,%%r15  ), %%xmm1,  %%xmm1  \n\t"
	"vfmadd231ps       %%xmm1,  %%xmm3,  %%xmm2  \n\t"
	"vmovlpd           %%xmm2, (%%r14,%%rsi,4)   \n\t" // store ( gamma44..gamma54 )
	"vmovhpd           %%xmm2, (%%r14,%%r15  )   \n\t" // store ( gamma45..gamma55 )
	"                                            \n\t"
	"vunpckhps         %%ymm15, %%ymm13, %%ymm0  \n\t"
	"vextractf128 $0x1, %%ymm0, %%xmm2           \n\t"
	"vmovlpd   (%%r14,%%rsi,2), %%xmm1,  %%xmm1  \n\t"
	"vmovhpd   (%%r14,%%r13  ), %%xmm1,  %%xmm1  \n\t"
	"vfmadd231ps       %%xmm1,  %%xmm3,  %%xmm0  \n\t"
	"vmovlpd           %%xmm0, (%%r14,%%rsi,2)   \n\t" // store ( gamma42..gamma52 )
	"vmovhpd           %%xmm0, (%%r14,%%r13  )   \n\t" // store ( gamma43..gamma53 )
	"vmovlpd   (%%r14,%%r13,2), %%xmm1,  %%xmm1  \n\t"
	"vmovhpd   (%%r14,%%r10  ), %%xmm1,  %%xmm1  \n\t"
	"vfmadd231ps       %%xmm1,  %%xmm3,  %%xmm2  \n\t"
	"vmovlpd           %%xmm2, (%%r14,%%r13,2)   \n\t" // store ( gamma46..gamma56 )
	"vmovhpd           %%xmm2, (%%r14,%%r10  )   \n\t" // store ( gamma47..gamma57 )
	"                                            \n\t"
	//"leaq   (%%r14,%%rsi,8), %%r14               \n\t" // r14 += 8*cs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"jmp    .SDONE                               \n\t" // jump to end.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".SBETAZERO:                                 \n\t"
	"                                            \n\t"
	"cmpq       $4, %%rsi                        \n\t" // set ZF if (4*cs_c) == 4.
	"jz      .SROWSTORBZ                         \n\t" // jump to row storage case
	"                                            \n\t"
	"cmpq       $4, %%rdi                        \n\t" // set ZF if (4*cs_c) == 4.
	"jz      .SCOLSTORBZ                         \n\t" // jump to column storage case
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".SGENSTORBZ:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps           %%ymm4,  %%ymm0           \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps           %%ymm6,  %%ymm0           \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps           %%ymm8,  %%ymm0           \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps           %%ymm10, %%ymm0           \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps           %%ymm12, %%ymm0           \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps           %%ymm14, %%ymm0           \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	//"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"movq      %%rdx, %%rcx                      \n\t" // rcx = c + 8*cs_c
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps           %%ymm5,  %%ymm0           \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps           %%ymm7,  %%ymm0           \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps           %%ymm9,  %%ymm0           \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps           %%ymm11, %%ymm0           \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps           %%ymm13, %%ymm0           \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps           %%ymm15, %%ymm0           \n\t"
	SGEMM_OUTPUT_GS_BETA_NZ
	//"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"jmp    .SDONE                               \n\t" // jump to end.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".SROWSTORBZ:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovups          %%ymm4,  (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vmovups          %%ymm5,  (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"vmovups          %%ymm6,  (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vmovups          %%ymm7,  (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovups          %%ymm8,  (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vmovups          %%ymm9,  (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovups          %%ymm10, (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vmovups          %%ymm11, (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovups          %%ymm12, (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vmovups          %%ymm13, (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovups          %%ymm14, (%%rcx)           \n\t"
	//"addq      %%rdi, %%rcx                      \n\t"
	"vmovups          %%ymm15, (%%rdx)           \n\t"
	//"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"jmp    .SDONE                               \n\t" // jump to end.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".SCOLSTORBZ:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vunpcklps         %%ymm6,  %%ymm4,  %%ymm0  \n\t"
	"vunpcklps         %%ymm10, %%ymm8,  %%ymm1  \n\t"
	"vshufps    $0x4e, %%ymm1,  %%ymm0,  %%ymm2  \n\t"
	"vblendps   $0xcc, %%ymm2,  %%ymm0,  %%ymm0  \n\t"
	"vblendps   $0x33, %%ymm2,  %%ymm1,  %%ymm1  \n\t"
	"                                            \n\t"
	"vextractf128 $0x1, %%ymm0, %%xmm2           \n\t"
	"vmovups           %%xmm0, (%%rcx        )   \n\t" // store ( gamma00..gamma30 )
	"vmovups           %%xmm2, (%%rcx,%%rsi,4)   \n\t" // store ( gamma04..gamma34 )
	"                                            \n\t"
	"vextractf128 $0x1, %%ymm1, %%xmm2           \n\t"
	"vmovups           %%xmm1, (%%rcx,%%rsi,1)   \n\t" // store ( gamma01..gamma31 )
	"vmovups           %%xmm2, (%%rcx,%%r15  )   \n\t" // store ( gamma05..gamma35 )
	"                                            \n\t"
	"                                            \n\t"
	"vunpckhps         %%ymm6,  %%ymm4,  %%ymm0  \n\t"
	"vunpckhps         %%ymm10, %%ymm8,  %%ymm1  \n\t"
	"vshufps    $0x4e, %%ymm1,  %%ymm0,  %%ymm2  \n\t"
	"vblendps   $0xcc, %%ymm2,  %%ymm0,  %%ymm0  \n\t"
	"vblendps   $0x33, %%ymm2,  %%ymm1,  %%ymm1  \n\t"
	"                                            \n\t"
	"vextractf128 $0x1, %%ymm0, %%xmm2           \n\t"
	"vmovups           %%xmm0, (%%rcx,%%rsi,2)   \n\t" // store ( gamma02..gamma32 )
	"vmovups           %%xmm2, (%%rcx,%%r13,2)   \n\t" // store ( gamma06..gamma36 )
	"                                            \n\t"
	"vextractf128 $0x1, %%ymm1, %%xmm2           \n\t"
	"vmovups           %%xmm1, (%%rcx,%%r13  )   \n\t" // store ( gamma03..gamma33 )
	"vmovups           %%xmm2, (%%rcx,%%r10  )   \n\t" // store ( gamma07..gamma37 )
	"                                            \n\t"
	"leaq   (%%rcx,%%rsi,8), %%rcx               \n\t" // rcx += 8*cs_c
	"                                            \n\t"
	"vunpcklps         %%ymm14, %%ymm12, %%ymm0  \n\t"
	"vextractf128 $0x1, %%ymm0, %%xmm2           \n\t"
	"vmovlpd           %%xmm0, (%%r14        )   \n\t" // store ( gamma40..gamma50 )
	"vmovhpd           %%xmm0, (%%r14,%%rsi,1)   \n\t" // store ( gamma41..gamma51 )
	"vmovlpd           %%xmm2, (%%r14,%%rsi,4)   \n\t" // store ( gamma44..gamma54 )
	"vmovhpd           %%xmm2, (%%r14,%%r15  )   \n\t" // store ( gamma45..gamma55 )
	"                                            \n\t"
	"vunpckhps         %%ymm14, %%ymm12, %%ymm0  \n\t"
	"vextractf128 $0x1, %%ymm0, %%xmm2           \n\t"
	"vmovlpd           %%xmm0, (%%r14,%%rsi,2)   \n\t" // store ( gamma42..gamma52 )
	"vmovhpd           %%xmm0, (%%r14,%%r13  )   \n\t" // store ( gamma43..gamma53 )
	"vmovlpd           %%xmm2, (%%r14,%%r13,2)   \n\t" // store ( gamma46..gamma56 )
	"vmovhpd           %%xmm2, (%%r14,%%r10  )   \n\t" // store ( gamma47..gamma57 )
	"                                            \n\t"
	"leaq   (%%r14,%%rsi,8), %%r14               \n\t" // r14 += 8*cs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vunpcklps         %%ymm7,  %%ymm5,  %%ymm0  \n\t"
	"vunpcklps         %%ymm11, %%ymm9,  %%ymm1  \n\t"
	"vshufps    $0x4e, %%ymm1,  %%ymm0,  %%ymm2  \n\t"
	"vblendps   $0xcc, %%ymm2,  %%ymm0,  %%ymm0  \n\t"
	"vblendps   $0x33, %%ymm2,  %%ymm1,  %%ymm1  \n\t"
	"                                            \n\t"
	"vextractf128 $0x1, %%ymm0, %%xmm2           \n\t"
	"vmovups           %%xmm0, (%%rcx        )   \n\t" // store ( gamma00..gamma30 )
	"vmovups           %%xmm2, (%%rcx,%%rsi,4)   \n\t" // store ( gamma04..gamma34 )
	"                                            \n\t"
	"vextractf128 $0x1, %%ymm1, %%xmm2           \n\t"
	"vmovups           %%xmm1, (%%rcx,%%rsi,1)   \n\t" // store ( gamma01..gamma31 )
	"vmovups           %%xmm2, (%%rcx,%%r15  )   \n\t" // store ( gamma05..gamma35 )
	"                                            \n\t"
	"                                            \n\t"
	"vunpckhps         %%ymm7,  %%ymm5,  %%ymm0  \n\t"
	"vunpckhps         %%ymm11, %%ymm9,  %%ymm1  \n\t"
	"vshufps    $0x4e, %%ymm1,  %%ymm0,  %%ymm2  \n\t"
	"vblendps   $0xcc, %%ymm2,  %%ymm0,  %%ymm0  \n\t"
	"vblendps   $0x33, %%ymm2,  %%ymm1,  %%ymm1  \n\t"
	"                                            \n\t"
	"vextractf128 $0x1, %%ymm0, %%xmm2           \n\t"
	"vmovups           %%xmm0, (%%rcx,%%rsi,2)   \n\t" // store ( gamma02..gamma32 )
	"vmovups           %%xmm2, (%%rcx,%%r13,2)   \n\t" // store ( gamma06..gamma36 )
	"                                            \n\t"
	"vextractf128 $0x1, %%ymm1, %%xmm2           \n\t"
	"vmovups           %%xmm1, (%%rcx,%%r13  )   \n\t" // store ( gamma03..gamma33 )
	"vmovups           %%xmm2, (%%rcx,%%r10  )   \n\t" // store ( gamma07..gamma37 )
	"                                            \n\t"
	//"leaq   (%%rcx,%%rsi,8), %%rcx               \n\t" // rcx += 8*cs_c
	"                                            \n\t"
	"vunpcklps         %%ymm15, %%ymm13, %%ymm0  \n\t"
	"vextractf128 $0x1, %%ymm0, %%xmm2           \n\t"
	"vmovlpd           %%xmm0, (%%r14        )   \n\t" // store ( gamma40..gamma50 )
	"vmovhpd           %%xmm0, (%%r14,%%rsi,1)   \n\t" // store ( gamma41..gamma51 )
	"vmovlpd           %%xmm2, (%%r14,%%rsi,4)   \n\t" // store ( gamma44..gamma54 )
	"vmovhpd           %%xmm2, (%%r14,%%r15  )   \n\t" // store ( gamma45..gamma55 )
	"                                            \n\t"
	"vunpckhps         %%ymm15, %%ymm13, %%ymm0  \n\t"
	"vextractf128 $0x1, %%ymm0, %%xmm2           \n\t"
	"vmovlpd           %%xmm0, (%%r14,%%rsi,2)   \n\t" // store ( gamma42..gamma52 )
	"vmovhpd           %%xmm0, (%%r14,%%r13  )   \n\t" // store ( gamma43..gamma53 )
	"vmovlpd           %%xmm2, (%%r14,%%r13,2)   \n\t" // store ( gamma46..gamma56 )
	"vmovhpd           %%xmm2, (%%r14,%%r10  )   \n\t" // store ( gamma47..gamma57 )
	"                                            \n\t"
	//"leaq   (%%r14,%%rsi,8), %%r14               \n\t" // r14 += 8*cs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".SDONE:                                     \n\t"
	"                                            \n\t"
	"                                            \n\t"

	: // output operands (none)
	: // input operands
	  "m" (k_iter), // 0
	  "m" (k_left), // 1
	  "m" (a),      // 2
	  "m" (b),      // 3
	  "m" (alpha),  // 4
	  "m" (beta),   // 5
	  "m" (c),      // 6
	  "m" (rs_c),   // 7
	  "m" (cs_c)/*,   // 8
	  "m" (b_next), // 9
	  "m" (a_next)*/  // 10
	: // register clobber list
	  "rax", "rbx", "rcx", "rdx", "rsi", "rdi", 
	  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
	  "xmm0", "xmm1", "xmm2", "xmm3",
	  "xmm4", "xmm5", "xmm6", "xmm7",
	  "xmm8", "xmm9", "xmm10", "xmm11",
	  "xmm12", "xmm13", "xmm14", "xmm15",
	  "memory"
	);
}




#define DGEMM_INPUT_GS_BETA_NZ \
	"vmovlpd    (%%rcx        ),  %%xmm0,  %%xmm0  \n\t" \
	"vmovhpd    (%%rcx,%%rsi,1),  %%xmm0,  %%xmm0  \n\t" \
	"vmovlpd    (%%rcx,%%rsi,2),  %%xmm1,  %%xmm1  \n\t" \
	"vmovhpd    (%%rcx,%%r13  ),  %%xmm1,  %%xmm1  \n\t" \
	"vperm2f128 $0x20,   %%ymm1,  %%ymm0,  %%ymm0  \n\t" /*\
	"vmovlpd    (%%rcx,%%rsi,4),  %%xmm2,  %%xmm2  \n\t" \
	"vmovhpd    (%%rcx,%%r15  ),  %%xmm2,  %%xmm2  \n\t" \
	"vmovlpd    (%%rcx,%%r13,2),  %%xmm1,  %%xmm1  \n\t" \
	"vmovhpd    (%%rcx,%%r10  ),  %%xmm1,  %%xmm1  \n\t" \
	"vperm2f128 $0x20,   %%ymm1,  %%ymm2,  %%ymm2  \n\t"*/

#define DGEMM_OUTPUT_GS_BETA_NZ \
	"vextractf128  $1, %%ymm0,  %%xmm1           \n\t" \
	"vmovlpd           %%xmm0,  (%%rcx        )  \n\t" \
	"vmovhpd           %%xmm0,  (%%rcx,%%rsi  )  \n\t" \
	"vmovlpd           %%xmm1,  (%%rcx,%%rsi,2)  \n\t" \
	"vmovhpd           %%xmm1,  (%%rcx,%%r13  )  \n\t" /*\
	"vextractf128  $1, %%ymm2,  %%xmm1           \n\t" \
	"vmovlpd           %%xmm2,  (%%rcx,%%rsi,4)  \n\t" \
	"vmovhpd           %%xmm2,  (%%rcx,%%r15  )  \n\t" \
	"vmovlpd           %%xmm1,  (%%rcx,%%r13,2)  \n\t" \
	"vmovhpd           %%xmm1,  (%%rcx,%%r10  )  \n\t"*/

void bli_dgemm_zen_asm_6x8
     (
       dim_t               k,
       double*    restrict alpha,
       double*    restrict a,
       double*    restrict b,
       double*    restrict beta,
       double*    restrict c, inc_t rs_c, inc_t cs_c,
       auxinfo_t* restrict data,
       cntx_t*    restrict cntx
     )
{
	//void*   a_next = bli_auxinfo_next_a( data );
	//void*   b_next = bli_auxinfo_next_b( data );

    uint64_t   k_iter = k / 4;
    uint64_t   k_left = k % 4;

	__asm__ volatile
	(
	"                                            \n\t"
	"vzeroall                                    \n\t" // zero all xmm/ymm registers.
	"                                            \n\t"
	"                                            \n\t"
	"movq                %2, %%rax               \n\t" // load address of a.
	"movq                %3, %%rbx               \n\t" // load address of b.
	//"movq                %9, %%r15               \n\t" // load address of b_next.
	"                                            \n\t"
	"addq           $32 * 4, %%rbx               \n\t"
	"                                            \n\t" // initialize loop by pre-loading
	"vmovapd           -4 * 32(%%rbx), %%ymm0    \n\t"
	"vmovapd           -3 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"movq                %6, %%rcx               \n\t" // load address of c
	"movq                %7, %%rdi               \n\t" // load rs_c
	"leaq        (,%%rdi,8), %%rdi               \n\t" // rs_c *= sizeof(double)
	"                                            \n\t"
	"leaq   (%%rdi,%%rdi,2), %%r13               \n\t" // r13 = 3*rs_c;
	"leaq   (%%rcx,%%r13,1), %%rdx               \n\t" // rdx = c + 3*rs_c;
	"prefetcht0   7 * 8(%%rcx)                   \n\t" // prefetch c + 0*rs_c
	"prefetcht0   7 * 8(%%rcx,%%rdi)             \n\t" // prefetch c + 1*rs_c
	"prefetcht0   7 * 8(%%rcx,%%rdi,2)           \n\t" // prefetch c + 2*rs_c
	"prefetcht0   7 * 8(%%rdx)                   \n\t" // prefetch c + 3*rs_c
	"prefetcht0   7 * 8(%%rdx,%%rdi)             \n\t" // prefetch c + 4*rs_c
	"prefetcht0   7 * 8(%%rdx,%%rdi,2)           \n\t" // prefetch c + 5*rs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"movq      %0, %%rsi                         \n\t" // i = k_iter;
	"testq  %%rsi, %%rsi                         \n\t" // check i via logical AND.
	"je     .DCONSIDKLEFT                        \n\t" // if i == 0, jump to code that
	"                                            \n\t" // contains the k_left loop.
	"                                            \n\t"
	"                                            \n\t"
	".DLOOPKITER:                                \n\t" // MAIN LOOP
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t" // iteration 0
	"prefetcht0   64 * 8(%%rax)                  \n\t"
	"                                            \n\t"
	"vbroadcastsd       0 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd       1 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastsd       2 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd       3 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastsd       4 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd       5 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"vmovapd           -2 * 32(%%rbx), %%ymm0    \n\t"
	"vmovapd           -1 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t" // iteration 1
	"vbroadcastsd       6 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd       7 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastsd       8 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd       9 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastsd      10 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd      11 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"vmovapd            0 * 32(%%rbx), %%ymm0    \n\t"
	"vmovapd            1 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t" // iteration 2
	"prefetcht0   76 * 8(%%rax)                  \n\t"
	"                                            \n\t"
	"vbroadcastsd      12 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd      13 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastsd      14 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd      15 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastsd      16 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd      17 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"vmovapd            2 * 32(%%rbx), %%ymm0    \n\t"
	"vmovapd            3 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t" // iteration 3
	"vbroadcastsd      18 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd      19 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastsd      20 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd      21 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastsd      22 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd      23 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"addq           $4 * 6 * 8, %%rax            \n\t" // a += 4*6 (unroll x mr)
	"addq           $4 * 8 * 8, %%rbx            \n\t" // b += 4*8 (unroll x nr)
	"                                            \n\t"
	"vmovapd           -4 * 32(%%rbx), %%ymm0    \n\t"
	"vmovapd           -3 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"decq   %%rsi                                \n\t" // i -= 1;
	"jne    .DLOOPKITER                          \n\t" // iterate again if i != 0.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".DCONSIDKLEFT:                              \n\t"
	"                                            \n\t"
	"movq      %1, %%rsi                         \n\t" // i = k_left;
	"testq  %%rsi, %%rsi                         \n\t" // check i via logical AND.
	"je     .DPOSTACCUM                          \n\t" // if i == 0, we're done; jump to end.
	"                                            \n\t" // else, we prepare to enter k_left loop.
	"                                            \n\t"
	"                                            \n\t"
	".DLOOPKLEFT:                                \n\t" // EDGE LOOP
	"                                            \n\t"
	"prefetcht0   64 * 8(%%rax)                  \n\t"
	"                                            \n\t"
	"vbroadcastsd       0 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd       1 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastsd       2 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd       3 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastsd       4 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd       5 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"addq           $1 * 6 * 8, %%rax            \n\t" // a += 1*6 (unroll x mr)
	"addq           $1 * 8 * 8, %%rbx            \n\t" // b += 1*8 (unroll x nr)
	"                                            \n\t"
	"vmovapd           -4 * 32(%%rbx), %%ymm0    \n\t"
	"vmovapd           -3 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"decq   %%rsi                                \n\t" // i -= 1;
	"jne    .DLOOPKLEFT                          \n\t" // iterate again if i != 0.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".DPOSTACCUM:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"movq         %4, %%rax                      \n\t" // load address of alpha
	"movq         %5, %%rbx                      \n\t" // load address of beta 
	"vbroadcastsd    (%%rax), %%ymm0             \n\t" // load alpha and duplicate
	"vbroadcastsd    (%%rbx), %%ymm3             \n\t" // load beta and duplicate
	"                                            \n\t"
	"vmulpd           %%ymm0,  %%ymm4,  %%ymm4   \n\t" // scale by alpha
	"vmulpd           %%ymm0,  %%ymm5,  %%ymm5   \n\t"
	"vmulpd           %%ymm0,  %%ymm6,  %%ymm6   \n\t"
	"vmulpd           %%ymm0,  %%ymm7,  %%ymm7   \n\t"
	"vmulpd           %%ymm0,  %%ymm8,  %%ymm8   \n\t"
	"vmulpd           %%ymm0,  %%ymm9,  %%ymm9   \n\t"
	"vmulpd           %%ymm0,  %%ymm10, %%ymm10  \n\t"
	"vmulpd           %%ymm0,  %%ymm11, %%ymm11  \n\t"
	"vmulpd           %%ymm0,  %%ymm12, %%ymm12  \n\t"
	"vmulpd           %%ymm0,  %%ymm13, %%ymm13  \n\t"
	"vmulpd           %%ymm0,  %%ymm14, %%ymm14  \n\t"
	"vmulpd           %%ymm0,  %%ymm15, %%ymm15  \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"movq                %8, %%rsi               \n\t" // load cs_c
	"leaq        (,%%rsi,8), %%rsi               \n\t" // rsi = cs_c * sizeof(double)
	"                                            \n\t"
	"leaq   (%%rcx,%%rsi,4), %%rdx               \n\t" // load address of c +  4*cs_c;
	"leaq   (%%rcx,%%rdi,4), %%r14               \n\t" // load address of c +  4*rs_c;
	"                                            \n\t"
	"leaq   (%%rsi,%%rsi,2), %%r13               \n\t" // r13 = 3*cs_c;
	//"leaq   (%%rsi,%%rsi,4), %%r15               \n\t" // r15 = 5*cs_c;
	//"leaq   (%%r13,%%rsi,4), %%r10               \n\t" // r10 = 7*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t" // now avoid loading C if beta == 0
	"                                            \n\t"
	"vxorpd    %%ymm0,  %%ymm0,  %%ymm0          \n\t" // set ymm0 to zero.
	"vucomisd  %%xmm0,  %%xmm3                   \n\t" // set ZF if beta == 0.
	"je      .DBETAZERO                          \n\t" // if ZF = 1, jump to beta == 0 case
	"                                            \n\t"
	"                                            \n\t"
	"cmpq       $8, %%rsi                        \n\t" // set ZF if (8*cs_c) == 8.
	"jz      .DROWSTORED                         \n\t" // jump to row storage case
	"                                            \n\t"
	"                                            \n\t"
	"cmpq       $8, %%rdi                        \n\t" // set ZF if (8*rs_c) == 8.
	"jz      .DCOLSTORED                         \n\t" // jump to column storage case
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".DGENSTORED:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	DGEMM_INPUT_GS_BETA_NZ
	"vfmadd213pd      %%ymm4,  %%ymm3,  %%ymm0   \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	DGEMM_INPUT_GS_BETA_NZ
	"vfmadd213pd      %%ymm6,  %%ymm3,  %%ymm0   \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	DGEMM_INPUT_GS_BETA_NZ
	"vfmadd213pd      %%ymm8,  %%ymm3,  %%ymm0   \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	DGEMM_INPUT_GS_BETA_NZ
	"vfmadd213pd      %%ymm10, %%ymm3,  %%ymm0   \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	DGEMM_INPUT_GS_BETA_NZ
	"vfmadd213pd      %%ymm12, %%ymm3,  %%ymm0   \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	DGEMM_INPUT_GS_BETA_NZ
	"vfmadd213pd      %%ymm14, %%ymm3,  %%ymm0   \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"                                            \n\t"
	"                                            \n\t"
	"movq      %%rdx, %%rcx                      \n\t" // rcx = c + 4*cs_c
	"                                            \n\t"
	"                                            \n\t"
	DGEMM_INPUT_GS_BETA_NZ
	"vfmadd213pd      %%ymm5,  %%ymm3,  %%ymm0   \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	DGEMM_INPUT_GS_BETA_NZ
	"vfmadd213pd      %%ymm7,  %%ymm3,  %%ymm0   \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	DGEMM_INPUT_GS_BETA_NZ
	"vfmadd213pd      %%ymm9,  %%ymm3,  %%ymm0   \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	DGEMM_INPUT_GS_BETA_NZ
	"vfmadd213pd      %%ymm11, %%ymm3,  %%ymm0   \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	DGEMM_INPUT_GS_BETA_NZ
	"vfmadd213pd      %%ymm13, %%ymm3,  %%ymm0   \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	DGEMM_INPUT_GS_BETA_NZ
	"vfmadd213pd      %%ymm15, %%ymm3,  %%ymm0   \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"jmp    .DDONE                               \n\t" // jump to end.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".DROWSTORED:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vfmadd231pd      (%%rcx), %%ymm3, %%ymm4    \n\t"
	"vmovupd          %%ymm4,  (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vfmadd231pd      (%%rdx), %%ymm3, %%ymm5    \n\t"
	"vmovupd          %%ymm5,  (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vfmadd231pd      (%%rcx), %%ymm3, %%ymm6    \n\t"
	"vmovupd          %%ymm6,  (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vfmadd231pd      (%%rdx), %%ymm3, %%ymm7    \n\t"
	"vmovupd          %%ymm7,  (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vfmadd231pd      (%%rcx), %%ymm3, %%ymm8    \n\t"
	"vmovupd          %%ymm8,  (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vfmadd231pd      (%%rdx), %%ymm3, %%ymm9    \n\t"
	"vmovupd          %%ymm9,  (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vfmadd231pd      (%%rcx), %%ymm3, %%ymm10   \n\t"
	"vmovupd          %%ymm10, (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vfmadd231pd      (%%rdx), %%ymm3, %%ymm11   \n\t"
	"vmovupd          %%ymm11, (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vfmadd231pd      (%%rcx), %%ymm3, %%ymm12   \n\t"
	"vmovupd          %%ymm12, (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vfmadd231pd      (%%rdx), %%ymm3, %%ymm13   \n\t"
	"vmovupd          %%ymm13, (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vfmadd231pd      (%%rcx), %%ymm3, %%ymm14   \n\t"
	"vmovupd          %%ymm14, (%%rcx)           \n\t"
	//"addq      %%rdi, %%rcx                      \n\t"
	"vfmadd231pd      (%%rdx), %%ymm3, %%ymm15   \n\t"
	"vmovupd          %%ymm15, (%%rdx)           \n\t"
	//"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"jmp    .DDONE                               \n\t" // jump to end.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".DCOLSTORED:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vunpcklpd         %%ymm6,  %%ymm4,  %%ymm0  \n\t"
	"vunpckhpd         %%ymm6,  %%ymm4,  %%ymm1  \n\t"
	"vunpcklpd         %%ymm10, %%ymm8,  %%ymm2  \n\t"
	"vunpckhpd         %%ymm10, %%ymm8,  %%ymm3  \n\t"
	"vinsertf128 $0x1, %%xmm2,  %%ymm0,  %%ymm4  \n\t"
	"vinsertf128 $0x1, %%xmm3,  %%ymm1,  %%ymm6  \n\t"
	"vperm2f128 $0x31, %%ymm2,  %%ymm0,  %%ymm8  \n\t"
	"vperm2f128 $0x31, %%ymm3,  %%ymm1,  %%ymm10 \n\t"
	"                                            \n\t"
	"vbroadcastsd    (%%rbx), %%ymm3             \n\t"
	"                                            \n\t"
	"vfmadd231pd (%%rcx        ),%%ymm3, %%ymm4  \n\t"
	"vfmadd231pd (%%rcx,%%rsi  ),%%ymm3, %%ymm6  \n\t"
	"vfmadd231pd (%%rcx,%%rsi,2),%%ymm3, %%ymm8  \n\t"
	"vfmadd231pd (%%rcx,%%r13  ),%%ymm3, %%ymm10 \n\t"
	"vmovupd          %%ymm4,  (%%rcx        )   \n\t"
	"vmovupd          %%ymm6,  (%%rcx,%%rsi  )   \n\t"
	"vmovupd          %%ymm8,  (%%rcx,%%rsi,2)   \n\t"
	"vmovupd          %%ymm10, (%%rcx,%%r13  )   \n\t"
	"                                            \n\t"
	"leaq   (%%rcx,%%rsi,4), %%rcx               \n\t"
	"                                            \n\t"
	"vunpcklpd         %%ymm14, %%ymm12, %%ymm0  \n\t"
	"vunpckhpd         %%ymm14, %%ymm12, %%ymm1  \n\t"
	"vextractf128         $0x1, %%ymm0,  %%xmm2  \n\t"
	"vextractf128         $0x1, %%ymm1,  %%xmm4  \n\t"
	"                                            \n\t"
	"vfmadd231pd (%%r14        ),%%xmm3, %%xmm0  \n\t"
	"vfmadd231pd (%%r14,%%rsi  ),%%xmm3, %%xmm1  \n\t"
	"vfmadd231pd (%%r14,%%rsi,2),%%xmm3, %%xmm2  \n\t"
	"vfmadd231pd (%%r14,%%r13  ),%%xmm3, %%xmm4  \n\t"
	"vmovupd          %%xmm0,  (%%r14        )   \n\t"
	"vmovupd          %%xmm1,  (%%r14,%%rsi  )   \n\t"
	"vmovupd          %%xmm2,  (%%r14,%%rsi,2)   \n\t"
	"vmovupd          %%xmm4,  (%%r14,%%r13  )   \n\t"
	"                                            \n\t"
	"leaq   (%%r14,%%rsi,4), %%r14               \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vunpcklpd         %%ymm7,  %%ymm5,  %%ymm0  \n\t"
	"vunpckhpd         %%ymm7,  %%ymm5,  %%ymm1  \n\t"
	"vunpcklpd         %%ymm11, %%ymm9,  %%ymm2  \n\t"
	"vunpckhpd         %%ymm11, %%ymm9,  %%ymm3  \n\t"
	"vinsertf128 $0x1, %%xmm2,  %%ymm0,  %%ymm5  \n\t"
	"vinsertf128 $0x1, %%xmm3,  %%ymm1,  %%ymm7  \n\t"
	"vperm2f128 $0x31, %%ymm2,  %%ymm0,  %%ymm9  \n\t"
	"vperm2f128 $0x31, %%ymm3,  %%ymm1,  %%ymm11 \n\t"
	"                                            \n\t"
	"vbroadcastsd    (%%rbx), %%ymm3             \n\t"
	"                                            \n\t"
	"vfmadd231pd (%%rcx        ),%%ymm3, %%ymm5  \n\t"
	"vfmadd231pd (%%rcx,%%rsi  ),%%ymm3, %%ymm7  \n\t"
	"vfmadd231pd (%%rcx,%%rsi,2),%%ymm3, %%ymm9  \n\t"
	"vfmadd231pd (%%rcx,%%r13  ),%%ymm3, %%ymm11 \n\t"
	"vmovupd          %%ymm5,  (%%rcx        )   \n\t"
	"vmovupd          %%ymm7,  (%%rcx,%%rsi  )   \n\t"
	"vmovupd          %%ymm9,  (%%rcx,%%rsi,2)   \n\t"
	"vmovupd          %%ymm11, (%%rcx,%%r13  )   \n\t"
	"                                            \n\t"
	//"leaq   (%%rcx,%%rsi,4), %%rcx               \n\t"
	"                                            \n\t"
	"vunpcklpd         %%ymm15, %%ymm13, %%ymm0  \n\t"
	"vunpckhpd         %%ymm15, %%ymm13, %%ymm1  \n\t"
	"vextractf128         $0x1, %%ymm0,  %%xmm2  \n\t"
	"vextractf128         $0x1, %%ymm1,  %%xmm4  \n\t"
	"                                            \n\t"
	"vfmadd231pd (%%r14        ),%%xmm3, %%xmm0  \n\t"
	"vfmadd231pd (%%r14,%%rsi  ),%%xmm3, %%xmm1  \n\t"
	"vfmadd231pd (%%r14,%%rsi,2),%%xmm3, %%xmm2  \n\t"
	"vfmadd231pd (%%r14,%%r13  ),%%xmm3, %%xmm4  \n\t"
	"vmovupd          %%xmm0,  (%%r14        )   \n\t"
	"vmovupd          %%xmm1,  (%%r14,%%rsi  )   \n\t"
	"vmovupd          %%xmm2,  (%%r14,%%rsi,2)   \n\t"
	"vmovupd          %%xmm4,  (%%r14,%%r13  )   \n\t"
	"                                            \n\t"
	//"leaq   (%%r14,%%rsi,4), %%r14               \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"jmp    .DDONE                               \n\t" // jump to end.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".DBETAZERO:                                 \n\t"
	"                                            \n\t"
	"cmpq       $8, %%rsi                        \n\t" // set ZF if (8*cs_c) == 8.
	"jz      .DROWSTORBZ                         \n\t" // jump to row storage case
	"                                            \n\t"
	"cmpq       $8, %%rdi                        \n\t" // set ZF if (8*rs_c) == 8.
	"jz      .DCOLSTORBZ                         \n\t" // jump to column storage case
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".DGENSTORBZ:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd           %%ymm4,  %%ymm0           \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd           %%ymm6,  %%ymm0           \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd           %%ymm8,  %%ymm0           \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd           %%ymm10, %%ymm0           \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd           %%ymm12, %%ymm0           \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd           %%ymm14, %%ymm0           \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"                                            \n\t"
	"                                            \n\t"
	"movq      %%rdx, %%rcx                      \n\t" // rcx = c + 4*cs_c
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd           %%ymm5,  %%ymm0           \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd           %%ymm7,  %%ymm0           \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd           %%ymm9,  %%ymm0           \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd           %%ymm11, %%ymm0           \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd           %%ymm13, %%ymm0           \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"addq      %%rdi, %%rcx                      \n\t" // c += rs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd           %%ymm15, %%ymm0           \n\t"
	DGEMM_OUTPUT_GS_BETA_NZ
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"jmp    .DDONE                               \n\t" // jump to end.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".DROWSTORBZ:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovupd          %%ymm4,  (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vmovupd          %%ymm5,  (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"vmovupd          %%ymm6,  (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vmovupd          %%ymm7,  (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovupd          %%ymm8,  (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vmovupd          %%ymm9,  (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovupd          %%ymm10, (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vmovupd          %%ymm11, (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovupd          %%ymm12, (%%rcx)           \n\t"
	"addq      %%rdi, %%rcx                      \n\t"
	"vmovupd          %%ymm13, (%%rdx)           \n\t"
	"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovupd          %%ymm14, (%%rcx)           \n\t"
	//"addq      %%rdi, %%rcx                      \n\t"
	"vmovupd          %%ymm15, (%%rdx)           \n\t"
	//"addq      %%rdi, %%rdx                      \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"jmp    .DDONE                               \n\t" // jump to end.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".DCOLSTORBZ:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vunpcklpd         %%ymm6,  %%ymm4,  %%ymm0  \n\t"
	"vunpckhpd         %%ymm6,  %%ymm4,  %%ymm1  \n\t"
	"vunpcklpd         %%ymm10, %%ymm8,  %%ymm2  \n\t"
	"vunpckhpd         %%ymm10, %%ymm8,  %%ymm3  \n\t"
	"vinsertf128 $0x1, %%xmm2,  %%ymm0,  %%ymm4  \n\t"
	"vinsertf128 $0x1, %%xmm3,  %%ymm1,  %%ymm6  \n\t"
	"vperm2f128 $0x31, %%ymm2,  %%ymm0,  %%ymm8  \n\t"
	"vperm2f128 $0x31, %%ymm3,  %%ymm1,  %%ymm10 \n\t"
	"                                            \n\t"
	"vmovupd          %%ymm4,  (%%rcx        )   \n\t"
	"vmovupd          %%ymm6,  (%%rcx,%%rsi  )   \n\t"
	"vmovupd          %%ymm8,  (%%rcx,%%rsi,2)   \n\t"
	"vmovupd          %%ymm10, (%%rcx,%%r13  )   \n\t"
	"                                            \n\t"
	"leaq   (%%rcx,%%rsi,4), %%rcx               \n\t"
	"                                            \n\t"
	"vunpcklpd         %%ymm14, %%ymm12, %%ymm0  \n\t"
	"vunpckhpd         %%ymm14, %%ymm12, %%ymm1  \n\t"
	"vextractf128         $0x1, %%ymm0,  %%xmm2  \n\t"
	"vextractf128         $0x1, %%ymm1,  %%xmm4  \n\t"
	"                                            \n\t"
	"vmovupd          %%xmm0,  (%%r14        )   \n\t"
	"vmovupd          %%xmm1,  (%%r14,%%rsi  )   \n\t"
	"vmovupd          %%xmm2,  (%%r14,%%rsi,2)   \n\t"
	"vmovupd          %%xmm4,  (%%r14,%%r13  )   \n\t"
	"                                            \n\t"
	"leaq   (%%r14,%%rsi,4), %%r14               \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vunpcklpd         %%ymm7,  %%ymm5,  %%ymm0  \n\t"
	"vunpckhpd         %%ymm7,  %%ymm5,  %%ymm1  \n\t"
	"vunpcklpd         %%ymm11, %%ymm9,  %%ymm2  \n\t"
	"vunpckhpd         %%ymm11, %%ymm9,  %%ymm3  \n\t"
	"vinsertf128 $0x1, %%xmm2,  %%ymm0,  %%ymm5  \n\t"
	"vinsertf128 $0x1, %%xmm3,  %%ymm1,  %%ymm7  \n\t"
	"vperm2f128 $0x31, %%ymm2,  %%ymm0,  %%ymm9  \n\t"
	"vperm2f128 $0x31, %%ymm3,  %%ymm1,  %%ymm11 \n\t"
	"                                            \n\t"
	"vmovupd          %%ymm5,  (%%rcx        )   \n\t"
	"vmovupd          %%ymm7,  (%%rcx,%%rsi  )   \n\t"
	"vmovupd          %%ymm9,  (%%rcx,%%rsi,2)   \n\t"
	"vmovupd          %%ymm11, (%%rcx,%%r13  )   \n\t"
	"                                            \n\t"
	//"leaq   (%%rcx,%%rsi,4), %%rcx               \n\t"
	"                                            \n\t"
	"vunpcklpd         %%ymm15, %%ymm13, %%ymm0  \n\t"
	"vunpckhpd         %%ymm15, %%ymm13, %%ymm1  \n\t"
	"vextractf128         $0x1, %%ymm0,  %%xmm2  \n\t"
	"vextractf128         $0x1, %%ymm1,  %%xmm4  \n\t"
	"                                            \n\t"
	"vmovupd          %%xmm0,  (%%r14        )   \n\t"
	"vmovupd          %%xmm1,  (%%r14,%%rsi  )   \n\t"
	"vmovupd          %%xmm2,  (%%r14,%%rsi,2)   \n\t"
	"vmovupd          %%xmm4,  (%%r14,%%r13  )   \n\t"
	"                                            \n\t"
	//"leaq   (%%r14,%%rsi,4), %%r14               \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".DDONE:                                     \n\t"
	"                                            \n\t"
	"                                            \n\t"

	: // output operands (none)
	: // input operands
	  "m" (k_iter), // 0
	  "m" (k_left), // 1
	  "m" (a),      // 2
	  "m" (b),      // 3
	  "m" (alpha),  // 4
	  "m" (beta),   // 5
	  "m" (c),      // 6
	  "m" (rs_c),   // 7
	  "m" (cs_c)/*,   // 8
	  "m" (b_next), // 9
	  "m" (a_next)*/  // 10
	: // register clobber list
	  "rax", "rbx", "rcx", "rdx", "rsi", "rdi", 
	  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
	  "xmm0", "xmm1", "xmm2", "xmm3",
	  "xmm4", "xmm5", "xmm6", "xmm7",
	  "xmm8", "xmm9", "xmm10", "xmm11",
	  "xmm12", "xmm13", "xmm14", "xmm15",
	  "memory"
	);
}




// assumes beta.r, beta.i have been broadcast into ymm1, ymm2.
// outputs to ymm0
#define CGEMM_INPUT_SCALE_GS_BETA_NZ \
	"vmovlpd    (%%rcx        ),  %%xmm0,  %%xmm0  \n\t" \
	"vmovhpd    (%%rcx,%%rsi,1),  %%xmm0,  %%xmm0  \n\t" \
	"vmovlpd    (%%rcx,%%rsi,2),  %%xmm3,  %%xmm3  \n\t" \
	"vmovhpd    (%%rcx,%%r13  ),  %%xmm3,  %%xmm3  \n\t" \
	"vinsertf128     $1, %%xmm3,  %%ymm0,  %%ymm0  \n\t" \
	"vpermilps    $0xb1, %%ymm0,  %%ymm3           \n\t" \
	"vmulps              %%ymm1,  %%ymm0,  %%ymm0  \n\t" \
	"vmulps              %%ymm2,  %%ymm3,  %%ymm3  \n\t" \
	"vaddsubps           %%ymm3,  %%ymm0,  %%ymm0  \n\t"

// assumes values to output are in ymm0
#define CGEMM_OUTPUT_GS \
	"vextractf128    $1, %%ymm0,  %%xmm3           \n\t" \
	"vmovlpd             %%xmm0,  (%%rcx        )  \n\t" \
	"vmovhpd             %%xmm0,  (%%rcx,%%rsi,1)  \n\t" \
	"vmovlpd             %%xmm3,  (%%rcx,%%rsi,2)  \n\t" \
	"vmovhpd             %%xmm3,  (%%rcx,%%r13  )  \n\t"

#define CGEMM_INPUT_SCALE_RS_BETA_NZ \
	"vmovups    (%%rcx),       %%ymm0            \n\t" \
	"vpermilps $0xb1, %%ymm0,  %%ymm3            \n\t" \
	"vmulps           %%ymm1,  %%ymm0,  %%ymm0   \n\t" \
	"vmulps           %%ymm2,  %%ymm3,  %%ymm3   \n\t" \
	"vaddsubps        %%ymm3,  %%ymm0,  %%ymm0   \n\t"
	
#define CGEMM_OUTPUT_RS \
	"vmovups           %%ymm0,  (%%rcx)          \n\t" \

void bli_cgemm_zen_asm_3x8
     (
       dim_t               k,
       scomplex*  restrict alpha,
       scomplex*  restrict a,
       scomplex*  restrict b,
       scomplex*  restrict beta,
       scomplex*  restrict c, inc_t rs_c, inc_t cs_c,
       auxinfo_t* restrict data,
       cntx_t*    restrict cntx
     )
{
	//void*   a_next = bli_auxinfo_next_a( data );
	//void*   b_next = bli_auxinfo_next_b( data );

	uint64_t   k_iter = k / 4;
	uint64_t   k_left = k % 4;

	__asm__ volatile
	(
	"                                            \n\t"
	"vzeroall                                    \n\t" // zero all xmm/ymm registers.
	"                                            \n\t"
	"                                            \n\t"
	"movq                %2, %%rax               \n\t" // load address of a.
	"movq                %3, %%rbx               \n\t" // load address of b.
	//"movq                %9, %%r15               \n\t" // load address of b_next.
	"                                            \n\t"
	"addq           $32 * 4, %%rbx               \n\t"
	"                                            \n\t" // initialize loop by pre-loading
	"vmovaps           -4 * 32(%%rbx), %%ymm0    \n\t"
	"vmovaps           -3 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"movq                %6, %%rcx               \n\t" // load address of c
	"movq                %7, %%rdi               \n\t" // load rs_c
	"leaq        (,%%rdi,8), %%rdi               \n\t" // rs_c *= sizeof(scomplex)
	"                                            \n\t"
	"leaq   (%%rcx,%%rdi,1), %%r11               \n\t" // r11 = c + 1*rs_c;
	"leaq   (%%rcx,%%rdi,2), %%r12               \n\t" // r12 = c + 2*rs_c;
	"                                            \n\t"
	"prefetcht0   7 * 8(%%rcx)                   \n\t" // prefetch c + 0*rs_c
	"prefetcht0   7 * 8(%%r11)                   \n\t" // prefetch c + 1*rs_c
	"prefetcht0   7 * 8(%%r12)                   \n\t" // prefetch c + 2*rs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"movq      %0, %%rsi                         \n\t" // i = k_iter;
	"testq  %%rsi, %%rsi                         \n\t" // check i via logical AND.
	"je     .CCONSIDKLEFT                        \n\t" // if i == 0, jump to code that
	"                                            \n\t" // contains the k_left loop.
	"                                            \n\t"
	"                                            \n\t"
	".CLOOPKITER:                                \n\t" // MAIN LOOP
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t" // iteration 0
	"prefetcht0   32 * 8(%%rax)                  \n\t"
	"                                            \n\t"
	"vbroadcastss       0 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss       1 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastss       2 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss       3 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastss       4 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss       5 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"vmovaps           -2 * 32(%%rbx), %%ymm0    \n\t"
	"vmovaps           -1 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t" // iteration 1
	"vbroadcastss       6 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss       7 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastss       8 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss       9 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastss      10 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss      11 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"vmovaps            0 * 32(%%rbx), %%ymm0    \n\t"
	"vmovaps            1 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t" // iteration 2
	"prefetcht0   38 * 8(%%rax)                  \n\t"
	"                                            \n\t"
	"vbroadcastss      12 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss      13 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastss      14 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss      15 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastss      16 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss      17 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"vmovaps            2 * 32(%%rbx), %%ymm0    \n\t"
	"vmovaps            3 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t" // iteration 3
	"vbroadcastss      18 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss      19 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastss      20 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss      21 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastss      22 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss      23 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"addq          $4 *  3 * 8, %%rax            \n\t" // a += 4*3  (unroll x mr)
	"addq          $4 *  8 * 8, %%rbx            \n\t" // b += 4*8  (unroll x nr)
	"                                            \n\t"
	"vmovaps           -4 * 32(%%rbx), %%ymm0    \n\t"
	"vmovaps           -3 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"decq   %%rsi                                \n\t" // i -= 1;
	"jne    .CLOOPKITER                          \n\t" // iterate again if i != 0.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".CCONSIDKLEFT:                              \n\t"
	"                                            \n\t"
	"movq      %1, %%rsi                         \n\t" // i = k_left;
	"testq  %%rsi, %%rsi                         \n\t" // check i via logical AND.
	"je     .CPOSTACCUM                          \n\t" // if i == 0, we're done; jump to end.
	"                                            \n\t" // else, we prepare to enter k_left loop.
	"                                            \n\t"
	"                                            \n\t"
	".CLOOPKLEFT:                                \n\t" // EDGE LOOP
	"                                            \n\t"
	"prefetcht0   32 * 8(%%rax)                  \n\t"
	"                                            \n\t"
	"vbroadcastss       0 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss       1 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastss       2 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss       3 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastss       4 *  4(%%rax), %%ymm2    \n\t"
	"vbroadcastss       5 *  4(%%rax), %%ymm3    \n\t"
	"vfmadd231ps       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231ps       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231ps       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"addq          $1 *  3 * 8, %%rax            \n\t" // a += 1*3  (unroll x mr)
	"addq          $1 *  8 * 8, %%rbx            \n\t" // b += 1*8  (unroll x nr)
	"                                            \n\t"
	"vmovaps           -4 * 32(%%rbx), %%ymm0    \n\t"
	"vmovaps           -3 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"decq   %%rsi                                \n\t" // i -= 1;
	"jne    .CLOOPKLEFT                          \n\t" // iterate again if i != 0.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".CPOSTACCUM:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t" // permute even and odd elements
	"                                            \n\t" // of ymm6/7, ymm10/11, ymm/14/15
	"vpermilps $0xb1, %%ymm6,  %%ymm6            \n\t"
	"vpermilps $0xb1, %%ymm7,  %%ymm7            \n\t"
	"vpermilps $0xb1, %%ymm10, %%ymm10           \n\t"
	"vpermilps $0xb1, %%ymm11, %%ymm11           \n\t"
	"vpermilps $0xb1, %%ymm14, %%ymm14           \n\t"
	"vpermilps $0xb1, %%ymm15, %%ymm15           \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t" // subtract/add even/odd elements
	"vaddsubps        %%ymm6,  %%ymm4,  %%ymm4   \n\t"
	"vaddsubps        %%ymm7,  %%ymm5,  %%ymm5   \n\t"
	"                                            \n\t"
	"vaddsubps        %%ymm10, %%ymm8,  %%ymm8   \n\t"
	"vaddsubps        %%ymm11, %%ymm9,  %%ymm9   \n\t"
	"                                            \n\t"
	"vaddsubps        %%ymm14, %%ymm12, %%ymm12  \n\t"
	"vaddsubps        %%ymm15, %%ymm13, %%ymm13  \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"movq         %4, %%rax                      \n\t" // load address of alpha
	"vbroadcastss    (%%rax), %%ymm0             \n\t" // load alpha_r and duplicate
	"vbroadcastss   4(%%rax), %%ymm1             \n\t" // load alpha_i and duplicate
	"                                            \n\t"
	"                                            \n\t"
	"vpermilps $0xb1, %%ymm4,  %%ymm3            \n\t"
	"vmulps           %%ymm0,  %%ymm4,  %%ymm4   \n\t"
	"vmulps           %%ymm1,  %%ymm3,  %%ymm3   \n\t"
	"vaddsubps        %%ymm3,  %%ymm4,  %%ymm4   \n\t"
	"                                            \n\t"
	"vpermilps $0xb1, %%ymm5,  %%ymm3            \n\t"
	"vmulps           %%ymm0,  %%ymm5,  %%ymm5   \n\t"
	"vmulps           %%ymm1,  %%ymm3,  %%ymm3   \n\t"
	"vaddsubps        %%ymm3,  %%ymm5,  %%ymm5   \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vpermilps $0xb1, %%ymm8,  %%ymm3            \n\t"
	"vmulps           %%ymm0,  %%ymm8,  %%ymm8   \n\t"
	"vmulps           %%ymm1,  %%ymm3,  %%ymm3   \n\t"
	"vaddsubps        %%ymm3,  %%ymm8,  %%ymm8   \n\t"
	"                                            \n\t"
	"vpermilps $0xb1, %%ymm9,  %%ymm3            \n\t"
	"vmulps           %%ymm0,  %%ymm9,  %%ymm9   \n\t"
	"vmulps           %%ymm1,  %%ymm3,  %%ymm3   \n\t"
	"vaddsubps        %%ymm3,  %%ymm9,  %%ymm9   \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vpermilps $0xb1, %%ymm12, %%ymm3            \n\t"
	"vmulps           %%ymm0,  %%ymm12, %%ymm12  \n\t"
	"vmulps           %%ymm1,  %%ymm3,  %%ymm3   \n\t"
	"vaddsubps        %%ymm3,  %%ymm12, %%ymm12  \n\t"
	"                                            \n\t"
	"vpermilps $0xb1, %%ymm13, %%ymm3            \n\t"
	"vmulps           %%ymm0,  %%ymm13, %%ymm13  \n\t"
	"vmulps           %%ymm1,  %%ymm3,  %%ymm3   \n\t"
	"vaddsubps        %%ymm3,  %%ymm13, %%ymm13  \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"movq         %5, %%rbx                      \n\t" // load address of beta 
	"vbroadcastss    (%%rbx), %%ymm1             \n\t" // load beta_r and duplicate
	"vbroadcastss   4(%%rbx), %%ymm2             \n\t" // load beta_i and duplicate
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"movq                %8, %%rsi               \n\t" // load cs_c
	"leaq        (,%%rsi,8), %%rsi               \n\t" // rsi = cs_c * sizeof(scomplex)
	"leaq        (,%%rsi,4), %%rdx               \n\t" // rdx = 4*cs_c;
	"leaq   (%%rsi,%%rsi,2), %%r13               \n\t" // r13 = 3*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t" // now avoid loading C if beta == 0
	"vxorps    %%ymm0,  %%ymm0,  %%ymm0          \n\t" // set ymm0 to zero.
	"vucomiss  %%xmm0,  %%xmm1                   \n\t" // set ZF if beta_r == 0.
	"sete       %%r8b                            \n\t" // r8b = ( ZF == 1 ? 1 : 0 );
	"vucomiss  %%xmm0,  %%xmm2                   \n\t" // set ZF if beta_i == 0.
	"sete       %%r9b                            \n\t" // r9b = ( ZF == 1 ? 1 : 0 );
	"andb       %%r8b, %%r9b                     \n\t" // set ZF if r8b & r9b == 1.
	"jne     .CBETAZERO                          \n\t" // if ZF = 1, jump to beta == 0 case
	"                                            \n\t"
	"                                            \n\t"
	"cmpq       $8, %%rsi                        \n\t" // set ZF if (8*cs_c) == 8.
	"jz      .CROWSTORED                         \n\t" // jump to row storage case
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".CGENSTORED:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	CGEMM_INPUT_SCALE_GS_BETA_NZ
	"vaddps           %%ymm4,  %%ymm0,  %%ymm0   \n\t"
	CGEMM_OUTPUT_GS
	"addq      %%rdx, %%rcx                      \n\t" // c += 4*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	CGEMM_INPUT_SCALE_GS_BETA_NZ
	"vaddps           %%ymm5,  %%ymm0,  %%ymm0   \n\t"
	CGEMM_OUTPUT_GS
	"movq      %%r11, %%rcx                      \n\t" // rcx = c + 1*rs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	CGEMM_INPUT_SCALE_GS_BETA_NZ
	"vaddps           %%ymm8,  %%ymm0,  %%ymm0   \n\t"
	CGEMM_OUTPUT_GS
	"addq      %%rdx, %%rcx                      \n\t" // c += 4*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	CGEMM_INPUT_SCALE_GS_BETA_NZ
	"vaddps           %%ymm9,  %%ymm0,  %%ymm0   \n\t"
	CGEMM_OUTPUT_GS
	"movq      %%r12, %%rcx                      \n\t" // rcx = c + 2*rs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	CGEMM_INPUT_SCALE_GS_BETA_NZ
	"vaddps           %%ymm12, %%ymm0,  %%ymm0   \n\t"
	CGEMM_OUTPUT_GS
	"addq      %%rdx, %%rcx                      \n\t" // c += 4*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	CGEMM_INPUT_SCALE_GS_BETA_NZ
	"vaddps           %%ymm13, %%ymm0,  %%ymm0   \n\t"
	CGEMM_OUTPUT_GS
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"jmp    .CDONE                               \n\t" // jump to end.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".CROWSTORED:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	CGEMM_INPUT_SCALE_RS_BETA_NZ
	"vaddps           %%ymm4,  %%ymm0,  %%ymm0   \n\t"
	CGEMM_OUTPUT_RS
	"addq      %%rdx, %%rcx                      \n\t" // c += 4*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	CGEMM_INPUT_SCALE_RS_BETA_NZ
	"vaddps           %%ymm5,  %%ymm0,  %%ymm0   \n\t"
	CGEMM_OUTPUT_RS
	"movq      %%r11, %%rcx                      \n\t" // rcx = c + 1*rs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	CGEMM_INPUT_SCALE_RS_BETA_NZ
	"vaddps           %%ymm8,  %%ymm0,  %%ymm0   \n\t"
	CGEMM_OUTPUT_RS
	"addq      %%rdx, %%rcx                      \n\t" // c += 4*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	CGEMM_INPUT_SCALE_RS_BETA_NZ
	"vaddps           %%ymm9,  %%ymm0,  %%ymm0   \n\t"
	CGEMM_OUTPUT_RS
	"movq      %%r12, %%rcx                      \n\t" // rcx = c + 2*rs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	CGEMM_INPUT_SCALE_RS_BETA_NZ
	"vaddps           %%ymm12, %%ymm0,  %%ymm0   \n\t"
	CGEMM_OUTPUT_RS
	"addq      %%rdx, %%rcx                      \n\t" // c += 4*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	CGEMM_INPUT_SCALE_RS_BETA_NZ
	"vaddps           %%ymm13, %%ymm0,  %%ymm0   \n\t"
	CGEMM_OUTPUT_RS
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"jmp    .CDONE                               \n\t" // jump to end.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".CBETAZERO:                                 \n\t"
	"                                            \n\t"
	"cmpq       $8, %%rsi                        \n\t" // set ZF if (8*cs_c) == 8.
	"jz      .CROWSTORBZ                         \n\t" // jump to row storage case
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".CGENSTORBZ:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps          %%ymm4,  %%ymm0            \n\t"
	CGEMM_OUTPUT_GS
	"addq      %%rdx, %%rcx                      \n\t" // c += 2*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps          %%ymm5,  %%ymm0            \n\t"
	CGEMM_OUTPUT_GS
	"movq      %%r11, %%rcx                      \n\t" // rcx = c + 1*rs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps          %%ymm8,  %%ymm0            \n\t"
	CGEMM_OUTPUT_GS
	"addq      %%rdx, %%rcx                      \n\t" // c += 2*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps          %%ymm9,  %%ymm0            \n\t"
	CGEMM_OUTPUT_GS
	"movq      %%r12, %%rcx                      \n\t" // rcx = c + 2*rs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps          %%ymm12, %%ymm0            \n\t"
	CGEMM_OUTPUT_GS
	"addq      %%rdx, %%rcx                      \n\t" // c += 2*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovaps          %%ymm13, %%ymm0            \n\t"
	CGEMM_OUTPUT_GS
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"jmp    .CDONE                               \n\t" // jump to end.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".CROWSTORBZ:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovups          %%ymm4,  (%%rcx)           \n\t"
	"vmovups          %%ymm5,  (%%rcx,%%rdx,1)   \n\t"
	"                                            \n\t"
	"vmovups          %%ymm8,  (%%r11)           \n\t"
	"vmovups          %%ymm9,  (%%r11,%%rdx,1)   \n\t"
	"                                            \n\t"
	"vmovups          %%ymm12, (%%r12)           \n\t"
	"vmovups          %%ymm13, (%%r12,%%rdx,1)   \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".CDONE:                                     \n\t"
	"                                            \n\t"
	"                                            \n\t"

	: // output operands (none)
	: // input operands
	  "m" (k_iter), // 0
	  "m" (k_left), // 1
	  "m" (a),      // 2
	  "m" (b),      // 3
	  "m" (alpha),  // 4
	  "m" (beta),   // 5
	  "m" (c),      // 6
	  "m" (rs_c),   // 7
	  "m" (cs_c)/*,   // 8
	  "m" (b_next), // 9
	  "m" (a_next)*/  // 10
	: // register clobber list
	  "rax", "rbx", "rcx", "rdx", "rsi", "rdi", 
	  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
	  "xmm0", "xmm1", "xmm2", "xmm3",
	  "xmm4", "xmm5", "xmm6", "xmm7",
	  "xmm8", "xmm9", "xmm10", "xmm11",
	  "xmm12", "xmm13", "xmm14", "xmm15",
	  "memory"
	);
}




// assumes beta.r, beta.i have been broadcast into ymm1, ymm2.
// outputs to ymm0
#define ZGEMM_INPUT_SCALE_GS_BETA_NZ \
	"vmovupd    (%%rcx),       %%xmm0            \n\t" \
	"vmovupd    (%%rcx,%%rsi), %%xmm3            \n\t" \
	"vinsertf128  $1, %%xmm3,  %%ymm0,  %%ymm0   \n\t" \
	"vpermilpd  $0x5, %%ymm0,  %%ymm3            \n\t" \
	"vmulpd           %%ymm1,  %%ymm0,  %%ymm0   \n\t" \
	"vmulpd           %%ymm2,  %%ymm3,  %%ymm3   \n\t" \
	"vaddsubpd        %%ymm3,  %%ymm0,  %%ymm0   \n\t"
	
// assumes values to output are in ymm0
#define ZGEMM_OUTPUT_GS \
	"vextractf128  $1, %%ymm0,  %%xmm3           \n\t" \
	"vmovupd           %%xmm0,  (%%rcx)          \n\t" \
	"vmovupd           %%xmm3,  (%%rcx,%%rsi  )  \n\t" \

#define ZGEMM_INPUT_SCALE_RS_BETA_NZ \
	"vmovupd    (%%rcx),       %%ymm0            \n\t" \
	"vpermilpd  $0x5, %%ymm0,  %%ymm3            \n\t" \
	"vmulpd           %%ymm1,  %%ymm0,  %%ymm0   \n\t" \
	"vmulpd           %%ymm2,  %%ymm3,  %%ymm3   \n\t" \
	"vaddsubpd        %%ymm3,  %%ymm0,  %%ymm0   \n\t"
	
#define ZGEMM_OUTPUT_RS \
	"vmovupd           %%ymm0,  (%%rcx)          \n\t" \

void bli_zgemm_zen_asm_3x4
     (
       dim_t               k,
       dcomplex*  restrict alpha,
       dcomplex*  restrict a,
       dcomplex*  restrict b,
       dcomplex*  restrict beta,
       dcomplex*  restrict c, inc_t rs_c, inc_t cs_c,
       auxinfo_t* restrict data,
       cntx_t*    restrict cntx
     )
{
	//void*   a_next = bli_auxinfo_next_a( data );
	//void*   b_next = bli_auxinfo_next_b( data );

    uint64_t   k_iter = k / 4;
    uint64_t   k_left = k % 4;

	//uint64_t   alpha_is_unit = bli_zeq1( *alpha );


	__asm__ volatile
	(
	"                                            \n\t"
	"vzeroall                                    \n\t" // zero all xmm/ymm registers.
	"                                            \n\t"
	"                                            \n\t"
	"movq                %2, %%rax               \n\t" // load address of a.
	"movq                %3, %%rbx               \n\t" // load address of b.
	//"movq                %9, %%r15               \n\t" // load address of b_next.
	"                                            \n\t"
	"addq           $32 * 4, %%rbx               \n\t"
	"                                            \n\t" // initialize loop by pre-loading
	"vmovapd           -4 * 32(%%rbx), %%ymm0    \n\t"
	"vmovapd           -3 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"movq                %6, %%rcx               \n\t" // load address of c
	"movq                %7, %%rdi               \n\t" // load rs_c
	"leaq        (,%%rdi,8), %%rdi               \n\t" // rs_c *= sizeof(dcomplex)
	"leaq        (,%%rdi,2), %%rdi               \n\t"
	"                                            \n\t"
	"leaq   (%%rcx,%%rdi,1), %%r11               \n\t" // r11 = c + 1*rs_c;
	"leaq   (%%rcx,%%rdi,2), %%r12               \n\t" // r12 = c + 2*rs_c;
	"                                            \n\t"
	"prefetcht0   7 * 8(%%rcx)                   \n\t" // prefetch c + 0*rs_c
	"prefetcht0   7 * 8(%%r11)                   \n\t" // prefetch c + 1*rs_c
	"prefetcht0   7 * 8(%%r12)                   \n\t" // prefetch c + 2*rs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"movq      %0, %%rsi                         \n\t" // i = k_iter;
	"testq  %%rsi, %%rsi                         \n\t" // check i via logical AND.
	"je     .ZCONSIDKLEFT                        \n\t" // if i == 0, jump to code that
	"                                            \n\t" // contains the k_left loop.
	"                                            \n\t"
	"                                            \n\t"
	".ZLOOPKITER:                                \n\t" // MAIN LOOP
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t" // iteration 0
	"prefetcht0  32 * 16(%%rax)                  \n\t"
	"                                            \n\t"
	"vbroadcastsd       0 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd       1 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastsd       2 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd       3 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastsd       4 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd       5 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"vmovapd           -2 * 32(%%rbx), %%ymm0    \n\t"
	"vmovapd           -1 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t" // iteration 1
	"vbroadcastsd       6 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd       7 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastsd       8 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd       9 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastsd      10 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd      11 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"vmovapd            0 * 32(%%rbx), %%ymm0    \n\t"
	"vmovapd            1 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t" // iteration 2
	"prefetcht0  38 * 16(%%rax)                  \n\t"
	"                                            \n\t"
	"vbroadcastsd      12 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd      13 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastsd      14 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd      15 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastsd      16 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd      17 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"vmovapd            2 * 32(%%rbx), %%ymm0    \n\t"
	"vmovapd            3 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t" // iteration 3
	"vbroadcastsd      18 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd      19 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastsd      20 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd      21 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastsd      22 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd      23 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"addq          $4 * 3 * 16, %%rax            \n\t" // a += 4*3 (unroll x mr)
	"addq          $4 * 4 * 16, %%rbx            \n\t" // b += 4*4 (unroll x nr)
	"                                            \n\t"
	"vmovapd           -4 * 32(%%rbx), %%ymm0    \n\t"
	"vmovapd           -3 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"decq   %%rsi                                \n\t" // i -= 1;
	"jne    .ZLOOPKITER                          \n\t" // iterate again if i != 0.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".ZCONSIDKLEFT:                              \n\t"
	"                                            \n\t"
	"movq      %1, %%rsi                         \n\t" // i = k_left;
	"testq  %%rsi, %%rsi                         \n\t" // check i via logical AND.
	"je     .ZPOSTACCUM                          \n\t" // if i == 0, we're done; jump to end.
	"                                            \n\t" // else, we prepare to enter k_left loop.
	"                                            \n\t"
	"                                            \n\t"
	".ZLOOPKLEFT:                                \n\t" // EDGE LOOP
	"                                            \n\t"
	"prefetcht0  32 * 16(%%rax)                  \n\t"
	"                                            \n\t"
	"vbroadcastsd       0 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd       1 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm4    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm5    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm6    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm7    \n\t"
	"                                            \n\t"
	"vbroadcastsd       2 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd       3 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm8    \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm9    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm10   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm11   \n\t"
	"                                            \n\t"
	"vbroadcastsd       4 *  8(%%rax), %%ymm2    \n\t"
	"vbroadcastsd       5 *  8(%%rax), %%ymm3    \n\t"
	"vfmadd231pd       %%ymm0, %%ymm2, %%ymm12   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm2, %%ymm13   \n\t"
	"vfmadd231pd       %%ymm0, %%ymm3, %%ymm14   \n\t"
	"vfmadd231pd       %%ymm1, %%ymm3, %%ymm15   \n\t"
	"                                            \n\t"
	"addq          $1 * 3 * 16, %%rax            \n\t" // a += 1*3 (unroll x mr)
	"addq          $1 * 4 * 16, %%rbx            \n\t" // b += 1*4 (unroll x nr)
	"                                            \n\t"
	"vmovapd           -4 * 32(%%rbx), %%ymm0    \n\t"
	"vmovapd           -3 * 32(%%rbx), %%ymm1    \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"decq   %%rsi                                \n\t" // i -= 1;
	"jne    .ZLOOPKLEFT                          \n\t" // iterate again if i != 0.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".ZPOSTACCUM:                                \n\t"
	"                                            \n\t"
	"                                            \n\t" // permute even and odd elements
	"                                            \n\t" // of ymm6/7, ymm10/11, ymm/14/15
	"vpermilpd  $0x5, %%ymm6,  %%ymm6            \n\t"
	"vpermilpd  $0x5, %%ymm7,  %%ymm7            \n\t"
	"vpermilpd  $0x5, %%ymm10, %%ymm10           \n\t"
	"vpermilpd  $0x5, %%ymm11, %%ymm11           \n\t"
	"vpermilpd  $0x5, %%ymm14, %%ymm14           \n\t"
	"vpermilpd  $0x5, %%ymm15, %%ymm15           \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t" // subtract/add even/odd elements
	"vaddsubpd        %%ymm6,  %%ymm4,  %%ymm4   \n\t"
	"vaddsubpd        %%ymm7,  %%ymm5,  %%ymm5   \n\t"
	"                                            \n\t"
	"vaddsubpd        %%ymm10, %%ymm8,  %%ymm8   \n\t"
	"vaddsubpd        %%ymm11, %%ymm9,  %%ymm9   \n\t"
	"                                            \n\t"
	"vaddsubpd        %%ymm14, %%ymm12, %%ymm12  \n\t"
	"vaddsubpd        %%ymm15, %%ymm13, %%ymm13  \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"movq         %4, %%rax                      \n\t" // load address of alpha
	"vbroadcastsd    (%%rax), %%ymm0             \n\t" // load alpha_r and duplicate
	"vbroadcastsd   8(%%rax), %%ymm1             \n\t" // load alpha_i and duplicate
	"                                            \n\t"
	"                                            \n\t"
	"vpermilpd  $0x5, %%ymm4,  %%ymm3            \n\t"
	"vmulpd           %%ymm0,  %%ymm4,  %%ymm4   \n\t"
	"vmulpd           %%ymm1,  %%ymm3,  %%ymm3   \n\t"
	"vaddsubpd        %%ymm3,  %%ymm4,  %%ymm4   \n\t"
	"                                            \n\t"
	"vpermilpd  $0x5, %%ymm5,  %%ymm3            \n\t"
	"vmulpd           %%ymm0,  %%ymm5,  %%ymm5   \n\t"
	"vmulpd           %%ymm1,  %%ymm3,  %%ymm3   \n\t"
	"vaddsubpd        %%ymm3,  %%ymm5,  %%ymm5   \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vpermilpd  $0x5, %%ymm8,  %%ymm3            \n\t"
	"vmulpd           %%ymm0,  %%ymm8,  %%ymm8   \n\t"
	"vmulpd           %%ymm1,  %%ymm3,  %%ymm3   \n\t"
	"vaddsubpd        %%ymm3,  %%ymm8,  %%ymm8   \n\t"
	"                                            \n\t"
	"vpermilpd  $0x5, %%ymm9,  %%ymm3            \n\t"
	"vmulpd           %%ymm0,  %%ymm9,  %%ymm9   \n\t"
	"vmulpd           %%ymm1,  %%ymm3,  %%ymm3   \n\t"
	"vaddsubpd        %%ymm3,  %%ymm9,  %%ymm9   \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vpermilpd  $0x5, %%ymm12, %%ymm3            \n\t"
	"vmulpd           %%ymm0,  %%ymm12, %%ymm12  \n\t"
	"vmulpd           %%ymm1,  %%ymm3,  %%ymm3   \n\t"
	"vaddsubpd        %%ymm3,  %%ymm12, %%ymm12  \n\t"
	"                                            \n\t"
	"vpermilpd  $0x5, %%ymm13, %%ymm3            \n\t"
	"vmulpd           %%ymm0,  %%ymm13, %%ymm13  \n\t"
	"vmulpd           %%ymm1,  %%ymm3,  %%ymm3   \n\t"
	"vaddsubpd        %%ymm3,  %%ymm13, %%ymm13  \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"movq         %5, %%rbx                      \n\t" // load address of beta 
	"vbroadcastsd    (%%rbx), %%ymm1             \n\t" // load beta_r and duplicate
	"vbroadcastsd   8(%%rbx), %%ymm2             \n\t" // load beta_i and duplicate
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"movq                %8, %%rsi               \n\t" // load cs_c
	"leaq        (,%%rsi,8), %%rsi               \n\t" // rsi = cs_c * sizeof(dcomplex)
	"leaq        (,%%rsi,2), %%rsi               \n\t"
	"leaq        (,%%rsi,2), %%rdx               \n\t" // rdx = 2*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t" // now avoid loading C if beta == 0
	"vxorpd    %%ymm0,  %%ymm0,  %%ymm0          \n\t" // set ymm0 to zero.
	"vucomisd  %%xmm0,  %%xmm1                   \n\t" // set ZF if beta_r == 0.
	"sete       %%r8b                            \n\t" // r8b = ( ZF == 1 ? 1 : 0 );
	"vucomisd  %%xmm0,  %%xmm2                   \n\t" // set ZF if beta_i == 0.
	"sete       %%r9b                            \n\t" // r9b = ( ZF == 1 ? 1 : 0 );
	"andb       %%r8b, %%r9b                     \n\t" // set ZF if r8b & r9b == 1.
	"jne     .ZBETAZERO                          \n\t" // if ZF = 1, jump to beta == 0 case
	"                                            \n\t"
	"                                            \n\t"
	"cmpq      $16, %%rsi                        \n\t" // set ZF if (16*cs_c) == 16.
	"jz      .ZROWSTORED                         \n\t" // jump to row storage case
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".ZGENSTORED:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	ZGEMM_INPUT_SCALE_GS_BETA_NZ
	"vaddpd           %%ymm4,  %%ymm0,  %%ymm0   \n\t"
	ZGEMM_OUTPUT_GS
	"addq      %%rdx, %%rcx                      \n\t" // c += 2*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	ZGEMM_INPUT_SCALE_GS_BETA_NZ
	"vaddpd           %%ymm5,  %%ymm0,  %%ymm0   \n\t"
	ZGEMM_OUTPUT_GS
	"movq      %%r11, %%rcx                      \n\t" // rcx = c + 1*rs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	ZGEMM_INPUT_SCALE_GS_BETA_NZ
	"vaddpd           %%ymm8,  %%ymm0,  %%ymm0   \n\t"
	ZGEMM_OUTPUT_GS
	"addq      %%rdx, %%rcx                      \n\t" // c += 2*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	ZGEMM_INPUT_SCALE_GS_BETA_NZ
	"vaddpd           %%ymm9,  %%ymm0,  %%ymm0   \n\t"
	ZGEMM_OUTPUT_GS
	"movq      %%r12, %%rcx                      \n\t" // rcx = c + 2*rs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	ZGEMM_INPUT_SCALE_GS_BETA_NZ
	"vaddpd           %%ymm12, %%ymm0,  %%ymm0   \n\t"
	ZGEMM_OUTPUT_GS
	"addq      %%rdx, %%rcx                      \n\t" // c += 2*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	ZGEMM_INPUT_SCALE_GS_BETA_NZ
	"vaddpd           %%ymm13, %%ymm0,  %%ymm0   \n\t"
	ZGEMM_OUTPUT_GS
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"jmp    .ZDONE                               \n\t" // jump to end.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".ZROWSTORED:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	ZGEMM_INPUT_SCALE_RS_BETA_NZ
	"vaddpd           %%ymm4,  %%ymm0,  %%ymm0   \n\t"
	ZGEMM_OUTPUT_RS
	"addq      %%rdx, %%rcx                      \n\t" // c += 2*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	ZGEMM_INPUT_SCALE_RS_BETA_NZ
	"vaddpd           %%ymm5,  %%ymm0,  %%ymm0   \n\t"
	ZGEMM_OUTPUT_RS
	"movq      %%r11, %%rcx                      \n\t" // rcx = c + 1*rs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	ZGEMM_INPUT_SCALE_RS_BETA_NZ
	"vaddpd           %%ymm8,  %%ymm0,  %%ymm0   \n\t"
	ZGEMM_OUTPUT_RS
	"addq      %%rdx, %%rcx                      \n\t" // c += 2*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	ZGEMM_INPUT_SCALE_RS_BETA_NZ
	"vaddpd           %%ymm9,  %%ymm0,  %%ymm0   \n\t"
	ZGEMM_OUTPUT_RS
	"movq      %%r12, %%rcx                      \n\t" // rcx = c + 2*rs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	ZGEMM_INPUT_SCALE_RS_BETA_NZ
	"vaddpd           %%ymm12, %%ymm0,  %%ymm0   \n\t"
	ZGEMM_OUTPUT_RS
	"addq      %%rdx, %%rcx                      \n\t" // c += 2*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	ZGEMM_INPUT_SCALE_RS_BETA_NZ
	"vaddpd           %%ymm13, %%ymm0,  %%ymm0   \n\t"
	ZGEMM_OUTPUT_RS
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"jmp    .ZDONE                               \n\t" // jump to end.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".ZBETAZERO:                                 \n\t"
	"                                            \n\t"
	"cmpq      $16, %%rsi                        \n\t" // set ZF if (16*cs_c) == 16.
	"jz      .ZROWSTORBZ                         \n\t" // jump to row storage case
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".ZGENSTORBZ:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd          %%ymm4,  %%ymm0            \n\t"
	ZGEMM_OUTPUT_GS
	"addq      %%rdx, %%rcx                      \n\t" // c += 2*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd          %%ymm5,  %%ymm0            \n\t"
	ZGEMM_OUTPUT_GS
	"movq      %%r11, %%rcx                      \n\t" // rcx = c + 1*rs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd          %%ymm8,  %%ymm0            \n\t"
	ZGEMM_OUTPUT_GS
	"addq      %%rdx, %%rcx                      \n\t" // c += 2*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd          %%ymm9,  %%ymm0            \n\t"
	ZGEMM_OUTPUT_GS
	"movq      %%r12, %%rcx                      \n\t" // rcx = c + 2*rs_c
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd          %%ymm12, %%ymm0            \n\t"
	ZGEMM_OUTPUT_GS
	"addq      %%rdx, %%rcx                      \n\t" // c += 2*cs_c;
	"                                            \n\t"
	"                                            \n\t"
	"vmovapd          %%ymm13, %%ymm0            \n\t"
	ZGEMM_OUTPUT_GS
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"jmp    .ZDONE                               \n\t" // jump to end.
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".ZROWSTORBZ:                                \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"vmovupd          %%ymm4,  (%%rcx)           \n\t"
	"vmovupd          %%ymm5,  (%%rcx,%%rdx,1)   \n\t"
	"                                            \n\t"
	"vmovupd          %%ymm8,  (%%r11)           \n\t"
	"vmovupd          %%ymm9,  (%%r11,%%rdx,1)   \n\t"
	"                                            \n\t"
	"vmovupd          %%ymm12, (%%r12)           \n\t"
	"vmovupd          %%ymm13, (%%r12,%%rdx,1)   \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	"                                            \n\t"
	".ZDONE:                                     \n\t"
	"                                            \n\t"
	"                                            \n\t"

	: // output operands (none)
	: // input operands
	  "m" (k_iter), // 0
	  "m" (k_left), // 1
	  "m" (a),      // 2
	  "m" (b),      // 3
	  "m" (alpha),  // 4
	  "m" (beta),   // 5
	  "m" (c),      // 6
	  "m" (rs_c),   // 7
	  "m" (cs_c)/*,   // 8
	  "m" (b_next), // 9
	  "m" (a_next)*/  // 10
	: // register clobber list
	  "rax", "rbx", "rcx", "rdx", "rsi", "rdi", 
	  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
	  "xmm0", "xmm1", "xmm2", "xmm3",
	  "xmm4", "xmm5", "xmm6", "xmm7",
	  "xmm8", "xmm9", "xmm10", "xmm11",
	  "xmm12", "xmm13", "xmm14", "xmm15",
	  "memory"
	);
}

