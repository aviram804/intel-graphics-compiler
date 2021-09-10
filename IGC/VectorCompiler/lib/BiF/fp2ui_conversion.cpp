/*========================== begin_copyright_notice ============================

Copyright (C) 2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#include <cm-cl/math.h>
#include <cm-cl/vector.h>
using namespace cm;

namespace details {

template <unsigned N>
static CM_NODEBUG CM_INLINE vector<uint64_t, N>
__impl_combineLoHi(vector<uint32_t, N> Lo, vector<uint32_t, N> Hi) {
  vector<uint32_t, 2 * N> Res;
  Res.template select<N, 2>(1) = Hi;
  Res.template select<N, 2>(0) = Lo;
  return Res.template format<uint64_t>();
}

template <unsigned N>
CM_NODEBUG CM_INLINE vector<float, N> __impl_ui2fp__(vector<uint64_t, N> a) {
  const vector<uint32_t, N> Zero(0);
  const vector<uint32_t, N> Ones(0xffffffff);
  const vector<uint32_t, N> One(1);

  vector<uint32_t, 2 *N> LoHi = a.template format<uint32_t>();
  vector<uint32_t, N> Lo = LoHi.template select<N, 2>(0);
  vector<uint32_t, N> Hi = LoHi.template select<N, 2>(1);
  vector<uint32_t, N> LZ = cm::math::count_leading_zeros(Hi);

  // we need to get that nice first set bit into bit position 23.
  // thus we shift our nice pair of values by 63 - 23 - clz,
  // some bits will be dropped by shift thus we'll add 1 bits as R bit.
  // uint8_t shift = 39 - lz;

  vector<uint32_t, N> DroppedBits = vector<uint32_t, N>(39) - LZ;
  // SI
  vector<uint32_t, N> Sha = DroppedBits & vector<uint32_t, N>(0x3f);
  vector<uint32_t, N> Vec32 = vector<int32_t, N>(32);
  vector<uint32_t, N> Sh32 = Vec32 - Sha;
  auto Flag_large_shift = (Sha >= Vec32);
  auto Flag_zero_shift = (Sha == Zero);
  vector<uint32_t, N> Mask1 = Ones;
  Mask1.merge(Zero, Flag_large_shift);
  vector<uint32_t, N> Mask0 = Ones;
  Mask0.merge(Zero, Flag_zero_shift);

  // partial shift
  vector<uint32_t, N> TmpH1 = ((Hi & Mask0) << Sh32) & Mask1;
  vector<uint32_t, N> TmpH2 = (Hi >> (Sha - Vec32)) & ~Mask1;
  vector<uint32_t, N> TmpL = (Lo >> Sha) & Mask1;
  vector<uint32_t, N> Mant = TmpL | TmpH1 | TmpH2;

  vector<uint32_t, N> TmpSha = One << (-Sh32);
  vector<uint32_t, N> TmpMask = TmpSha - One;
  vector<uint32_t, N> StickyH = Hi & ~Mask1;
  StickyH = StickyH & TmpMask;

  // calculate RS
  vector<uint32_t, N> L1 = Lo & ~Mask1;
  vector<uint32_t, N> L2 = Lo & (Mask1 >> Sh32);
  vector<uint32_t, N> StickyL = L1 | L2;
  vector<uint32_t, N> S1 = StickyH | StickyL;
  auto S = S1 == Zero;
  vector<uint32_t, N> NotS = Zero;
  NotS.merge(Ones, S);

  // R is set but no S, round to even.
  vector<uint32_t, N> R = Mant & One;
  Mant = (Mant + One) >> One;
  Mant &= ~(NotS & R);

  vector<uint32_t, N> Exp = vector<uint32_t, N>(0xbd) - LZ;
  vector<uint32_t, N> ResL = Exp << vector<uint32_t, N>(23);
  ResL += Mant;

  vector<float, N> ResultLarge = ResL.template format<float>();
  vector<float, N> ResultSmall = Lo;

  auto IsSmallPred = Hi == Zero;

  vector<float, N> Result = ResultLarge;
  Result.merge(ResultSmall, IsSmallPred);

  return Result;
}

template <unsigned N>
CM_NODEBUG CM_INLINE vector<float, N> __impl_si2fp__(vector<uint64_t, N> a) {
  const vector<uint32_t, N> Zero(0);

  // NOTE: SIToFP is special, since it does not do the convert by itself,
  // Instead it just creates a sequence of 64.bit operations which
  // are then expanded. As such some type convertion trickery is involved.
  vector<uint32_t, 2 *N> LoHi = a.template format<uint32_t>();
  vector<uint32_t, N> Lo = LoHi.template select<N, 2>(0);
  vector<uint32_t, N> Hi = LoHi.template select<N, 2>(1);
  vector<uint32_t, N> SB = Hi & vector<uint32_t, N>(1u << 31);
  auto IsSignZero = SB == Zero;
  vector<uint64_t, N> b = -a;
  b.merge(a, IsSignZero);
  vector<float, N> Res = __impl_ui2fp__<N>(b);
  Res.merge(-Res, ~IsSignZero);
  return Res;
}

template <unsigned N, bool isSigned>
CM_NODEBUG CM_INLINE vector<uint64_t, N> __impl_fp2ui__(vector<float, N> a) {
  // vector of floats -> vector of ints
  vector<uint32_t, N> Uifl = a.template format<uint32_t>();
  const vector<uint32_t, N> Exp_mask(0xff << 23);
  const vector<uint32_t, N> Mantissa_mask((1u << 23) - 1);
  const vector<uint32_t, N> Zero(0);
  const vector<uint32_t, N> Ones(0xffffffff);
  vector<uint32_t, N> Exp = (Uifl >> 23) & vector<uint32_t, N>(0xff);
  // mantissa without hidden bit
  vector<uint32_t, N> Pmantissa = Uifl & Mantissa_mask;
  // take hidden bit into account
  vector<uint32_t, N> Mantissa = Pmantissa | vector<uint32_t, N>(1 << 23);
  vector<uint32_t, N> Data_h = Mantissa << 8;
  vector<uint32_t, N> Data_l = Zero;

  // this block do Logical Shift Right
  vector<uint32_t, N> Shift = vector<uint32_t, N>(0xbe) - Exp;
  vector<uint32_t, N> Sha = Shift & vector<uint32_t, N>(0x3f);
  vector<uint32_t, N> Vec32 = vector<uint32_t, N>(32);
  vector<uint32_t, N> Sh32 = Vec32 - Sha;
  auto Flag_large_shift = (Sha >= Vec32);
  auto Flag_zero_shift = (Sha == Zero);
  vector<uint32_t, N> Mask1 = Ones;
  Mask1.merge(Zero, Flag_large_shift);
  vector<uint32_t, N> Mask0 = Ones;
  Mask0.merge(Zero, Flag_zero_shift);
  vector<uint32_t, N> TmpH1 = ((Data_h & Mask0) << Sh32) & Mask1;
  vector<uint32_t, N> TmpH2 = (Data_h >> (Sha - Vec32)) & ~Mask1;
  vector<uint32_t, N> TmpL = (Data_l >> Sha) & Mask1;
  vector<uint32_t, N> Lo = TmpL | TmpH1 | TmpH2;
  vector<uint32_t, N> Hi = (Data_h >> Sha) & Mask1;

  // Discard results if shift is greater than 63
  vector<uint32_t, N> Mask = Ones;
  auto Flag_discard = (Shift > vector<uint32_t, N>(63));
  Mask.merge(Zero, Flag_discard);
  Lo = Lo & Mask;
  Hi = Hi & Mask;
  vector<uint32_t, N> SignedBitMask(1u << 31);
  vector<uint32_t, N> SignedBit = Uifl & SignedBitMask;
  auto FlagSignSet = (SignedBit != Zero);
  auto FlagNoSignSet = (SignedBit == Zero);
  // check for Exponent overflow (when sign bit set)
  auto FlagExpO = (Exp > vector<uint32_t, N>(0xbe));
  auto FlagExpUO = FlagNoSignSet & FlagExpO;
  if constexpr (isSigned) {
    // calculate (NOT[Lo, Hi] + 1) (integer sign negation)
    vector<uint32_t, N> NegLo = ~Lo;
    vector<uint32_t, N> NegHi = ~Hi;
    // TODO Use Addc intrinsic
    vector<uint32_t, N> AddcResVal = NegLo + 1;
    auto AddcResCBMask = (NegLo == Ones);
    vector<uint32_t, N> AddcResCB = Zero;
    AddcResCB.merge(vector<uint32_t, N>(1), AddcResCBMask);

    NegHi = NegHi + AddcResCB;

    // if sign bit is set, alter the result with negated value
    // if (FlagSignSet)
    Lo.merge(AddcResVal, FlagSignSet);
    Hi.merge(NegHi, FlagSignSet);

    // Here we process overflows
    vector<uint32_t, N> LoOrHi = Lo | Hi;
    auto NZ = (LoOrHi != Zero);
    vector<uint32_t, N> HiHBit = Hi & SignedBitMask;
    auto NZ2 = SignedBit != HiHBit;
    auto Ovrfl1 = NZ2 & NZ;

    // In case of overflow, HW response is : 7fffffffffffffff
    // if (Ovrfl1)
    Lo.merge(Ones, Ovrfl1);
    Hi.merge(vector<uint32_t, N>((1u << 31) - 1), Ovrfl1);

    // if (FlagExpO)
    Lo.merge(Zero, FlagExpO);
    Hi.merge(vector<uint32_t, N>(1u << 31), FlagExpO);

    // if (FlagExpUO)
    Lo.merge(Ones, FlagExpUO);
    Hi.merge(vector<uint32_t, N>((1u << 31) - 1), FlagExpUO);

  } else {
    // if (FlagSignSet)
    Lo.merge(Zero, FlagSignSet);
    Hi.merge(Zero, FlagSignSet);
    // if (FlagExpUO)
    Lo.merge(Ones, FlagExpUO);
    Hi.merge(Ones, FlagExpUO);
  }
  return __impl_combineLoHi<N>(Lo, Hi);
}
} // namespace details

#define __FP2UI_VECTOR_IMPL(N)                                                 \
  CM_NODEBUG CM_NOINLINE extern "C" cl_vector<uint64_t, N>                     \
      __cm_intrinsic_impl_fp2ui_##N##_(cl_vector<float, N> a) {                \
    vector<uint64_t, N> b = details::__impl_fp2ui__<N, false>(a);              \
    return b.cl_vector();                                                      \
  };

#define __FP2SI_VECTOR_IMPL(N)                                                 \
  CM_NODEBUG CM_NOINLINE extern "C" cl_vector<int64_t, N>                      \
      __cm_intrinsic_impl_fp2si_##N##_(cl_vector<float, N> a) {                \
    vector<int64_t, N> b = details::__impl_fp2ui__<N, true>(a);                \
    return b.cl_vector();                                                      \
  };

#define __UI2FP_VECTOR_IMPL(N)                                                 \
  CM_NODEBUG CM_NOINLINE extern "C" cl_vector<float, N>                        \
      __cm_intrinsic_impl_ui2fp_##N##_(cl_vector<uint64_t, N> a) {             \
    vector<float, N> b = details::__impl_ui2fp__<N>(a);                        \
    return b.cl_vector();                                                      \
  };

#define __SI2FP_VECTOR_IMPL(N)                                                 \
  CM_NODEBUG CM_NOINLINE extern "C" cl_vector<float, N>                        \
      __cm_intrinsic_impl_si2fp_##N##_(cl_vector<uint64_t, N> a) {             \
    vector<float, N> b = details::__impl_si2fp__<N>(a);                        \
    return b.cl_vector();                                                      \
  };
// special case - input not a vector
CM_NODEBUG CM_NOINLINE extern "C" uint64_t
__cm_intrinsic_impl_fp2ui_1_base__(float a) {
  vector<uint64_t, 1> b =
      details::__impl_fp2ui__<1, false>(vector<float, 1>(a));
  return b[0];
}

// special case - input not a vector
CM_NODEBUG CM_NOINLINE extern "C" int64_t
__cm_intrinsic_impl_fp2si_1_base__(float a) {
  vector<int64_t, 1> b = details::__impl_fp2ui__<1, true>(vector<float, 1>(a));
  return b[0];
}

// special case - input not a vector
CM_NODEBUG CM_NOINLINE extern "C" float
__cm_intrinsic_impl_ui2fp_1_base__(uint64_t a) {
  vector<float, 1> b = details::__impl_ui2fp__<1>(vector<uint64_t, 1>(a));
  return b[0];
}

// special case - input not a vector
CM_NODEBUG CM_NOINLINE extern "C" float
__cm_intrinsic_impl_si2fp_1_base__(int64_t a) {
  vector<float, 1> b = details::__impl_ui2fp__<1>(vector<int64_t, 1>(a));
  return b[0];
}

#define __DEFINE_FP2UI_FUN(N)                                                  \
  __FP2UI_VECTOR_IMPL(N);                                                      \
  __FP2SI_VECTOR_IMPL(N);                                                      \
  __UI2FP_VECTOR_IMPL(N);                                                      \
  __SI2FP_VECTOR_IMPL(N);

__DEFINE_FP2UI_FUN(1);
__DEFINE_FP2UI_FUN(2);
__DEFINE_FP2UI_FUN(4);
__DEFINE_FP2UI_FUN(8);
__DEFINE_FP2UI_FUN(16);
__DEFINE_FP2UI_FUN(32);
