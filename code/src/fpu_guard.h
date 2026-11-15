// FpuGuard: keeps the x87 control word and the SSE control register as they
// were across a call into code that may change them (Windows imaging
// components, drivers). The game's own maths (physics included) runs at the
// x87 precision its C runtime set at start-up (53-bit); a library that leaves
// it changed on the game thread would change the game's results from then on.
#pragma once
#include <cstdint>

#include "simd.h"

class FpuGuard {
 public:
  FpuGuard() { Save(&cw_, &csr_); }
  ~FpuGuard() {
    uint16_t cw;
    uint32_t csr;
    Save(&cw, &csr);
    if (cw != cw_ || csr != csr_) {
      ++Changed();
      Restore(cw_, csr_);
    }
  }
  FpuGuard(const FpuGuard&) = delete;
  FpuGuard& operator=(const FpuGuard&) = delete;
  // How many guarded calls changed the state (and were put back).
  static unsigned& Changed() {
    static unsigned n = 0;
    return n;
  }
  static void Save(uint16_t* cw, uint32_t* csr) {
#if defined(_MSC_VER)
    uint16_t c;
    __asm fnstcw c
    *cw = c;
    *csr = simd::CpuHasSse2() ? _mm_getcsr() : 0;
#else
    __asm__ __volatile__("fnstcw %0" : "=m"(*cw));
    *csr = 0;
    if (simd::CpuHasSse2()) __asm__ __volatile__("stmxcsr %0" : "=m"(*csr));
#endif
  }
  static void Restore(uint16_t cw, uint32_t csr) {
#if defined(_MSC_VER)
    __asm fldcw cw
    if (simd::CpuHasSse2()) _mm_setcsr(csr);
#else
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
    if (simd::CpuHasSse2()) __asm__ __volatile__("ldmxcsr %0" : : "m"(csr));
#endif
  }

 private:
  uint16_t cw_;
  uint32_t csr_;
};
