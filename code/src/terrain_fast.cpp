// FastTerrain: the game's per-frame terrain level-of-detail code rewritten.
//
// The terrain is a tree of nodes (see fixes_terrain.cpp for the layout). Per
// frame the game walks every visible node five times (Setup, Eval, Mesh,
// sub-block culling, draw); at ViewDistance=10 that is 2000-4000 nodes, each
// visit a cache miss, plus x87 maths with slow float-to-int conversions.
// This file has exact rewrites of the hottest parts:
//
//   Detail  0x482550  the two detail numbers of a node (called from Setup's
//                     0x483440) and the distance used for its texture
//   Eval    0x4836e0  vertex activation, with 0x4839c0 (vertex test via
//                     0x482c30), 0x483c10 / 0x483d50 / 0x483e80 (dependency
//                     graph inside a node), 0x484540 / 0x484180 (across
//                     node edges and levels)
//   Draw    0x4818b0  the draw walk (with 0x481a40); the draw calls are the
//                     game's own (0x481c00 / 0x481f20)
//
// "Exact" means the same results bit for bit: the maths repeats the game's
// x87 sequence in double precision (the game runs the FPU at 53-bit
// precision, set by its C runtime at start-up, so every x87 result equals
// the double result), rounding to float wherever the game stores a float.
// FastTerrain is used only when the FPU is at 53-bit precision on entry.
// Walks prefetch the next nodes so their cache misses overlap.
//
// tools/terrain_harness.py runs the game's own code and these functions
// side by side in an x86 emulator on random terrain trees and compares
// every byte they write; TerrainCheck=1 does the same in the game.
//
// Built with SSE2 (the file is only entered when the CPU has it).
#include "terrain_fast.h"

#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace tf {

// ---- game data -------------------------------------------------------------

// Static tables in the exe (fixed values, checked in Init against the exe).
static const int kRow[17] = {0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255, 272};
static const int kKid[17] = {0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256};
static const int kSubKid[16] = {0, 4, 64, 68, 8, 12, 72, 76, 128, 132, 192, 196, 136, 140, 200, 204};  // 0x572288
static const int kSubMap[16] = {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};                // 0x5722c8

// Tables the game builds at start-up (read in place).
static inline const uint8_t* Band() { return reinterpret_cast<const uint8_t*>(0x66B3EC); }      // detail -> vertex band
static inline const uint16_t* Err() { return reinterpret_cast<const uint16_t*>(0x67B3EC); }     // band -> error
static inline const int32_t* Vt() { return reinterpret_cast<const int32_t*>(0x67B628); }        // per vertex 8 ints
static inline const uint32_t* SqrtTab() { return reinterpret_cast<const uint32_t*>(0x5E112C); }

// Game functions the rewrites call.
typedef int(__fastcall* DrawFn)(Node* n, int edx, int a1, uint8_t* verts, int vcount, uint8_t* idx, int icount,
                                int sub);
static const uintptr_t kDraw = 0x481C00;
static const uintptr_t kDrawAlt = 0x481F20;

// Byte access to game structures.
template <class T>
static inline T& At(void* base, int off) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off);
}

// Node fields (offsets as in the game).
struct Node {
  void* vtbl;          // 0x00
  Node** kids;         // 0x04 16x16 children or null
  Node* parent;        // 0x08
  uint8_t* vinfo;      // 0x0c per vertex 8 bytes (+1 band, +2 height); +0x908 band counts, +0xb08 order, +0xd4c base
  float s10, s14;      // 0x10, 0x14
  float y18, y1c;      // 0x18, 0x1c
  uint16_t lvl_l;      // 0x20
  uint16_t f22;        // 0x22
  uint16_t gx, gy;     // 0x24, 0x26
  uint8_t level;       // 0x28
  uint8_t shift;       // 0x29
  uint8_t idx;         // 0x2a position in the parent (x low nibble, y high)
  uint8_t f2b;
  uint32_t on_file;    // 0x2c
  uint32_t f30;        // 0x30
  uint8_t* T;          // 0x34 terrain
  uint8_t* D;          // 0x38 data
  uint8_t* tex;        // 0x3c
  uint32_t vis;        // 0x40
};
static_assert(offsetof(Node, kids) == 0x04, "node");
static_assert(offsetof(Node, vinfo) == 0x0c, "node");
static_assert(offsetof(Node, lvl_l) == 0x20, "node");
static_assert(offsetof(Node, gx) == 0x24, "node");
static_assert(offsetof(Node, level) == 0x28, "node");
static_assert(offsetof(Node, idx) == 0x2a, "node");
static_assert(offsetof(Node, T) == 0x34, "node");
static_assert(offsetof(Node, vis) == 0x40, "node");

// Terrain (node->T) fields.
static inline float Tf(const uint8_t* t, int off) { return *reinterpret_cast<const float*>(t + off); }
static inline int32_t& Ti(uint8_t* t, int off) { return *reinterpret_cast<int32_t*>(t + off); }
enum : int {
  kCamX = 0x60, kCamY = 0x64, kCamZ = 0x68, kTol = 0x6c, kK74 = 0x74, kK78 = 0x78,
  kCntTest = 0x90, kCntDetail = 0x98, kCntSet = 0x9c, kCntDep = 0xa0, kCntOn = 0xa8, kCntOff = 0xac,
  kCntUpdate = 0xb0, kFrame = 0xb4, kStamps = 0xb8, kStamp = 0x53c, kAltDraw = 0xcb0,
};
// Data (node->D) fields.
enum : int {
  kFlags = 0x121, kLo = 0x122, kHi = 0x124, kPrevLo = 0x126, kPrevHi = 0x128, kDStamp = 0x130,
  kVerts = 0x134, kIdx = 0x138, kVCount = 0x13c, kICount = 0x13e, kMinX = 0x164, kMinZ = 0x168, kMaxX = 0x16c,
  kMaxZ = 0x170, kFactor = 0x174, kSubMask = 0x178, kSubTab = 0x17c,
};
static inline float Df(const uint8_t* d, int off) { return *reinterpret_cast<const float*>(d + off); }
static inline uint16_t& Dw(uint8_t* d, int off) { return *reinterpret_cast<uint16_t*>(d + off); }

#if defined(_MSC_VER)
static inline void Prefetch(const void* p) { _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0); }
#else
static inline void Prefetch(const void* p) { __builtin_prefetch(p, 0, 3); }
#endif

// ---- options and counters --------------------------------------------------

Options g_opt;
Stats g_stats;

// ---- maths -----------------------------------------------------------------

// x87 "equal to zero" as the game tests it (fcomp 0 / test ah,40h): true for
// 0 and for NaN.
static inline bool EqZ(double v) { return !(v < 0.0 || v > 0.0); }

static inline uint32_t Bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
static inline float FromBits(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// 0x461ac0: the game's table square root (7 bits of mantissa).
static inline float GameSqrt(float f) {
  if (EqZ(f)) return 0.0f;
  const uint32_t u = Bits(f);
  uint32_t m = u & 0x7FFFFF;
  const uint32_t e = (u >> 23) - 0x7F;  // the sign bit stays in, as in the game
  if (e & 1) m |= 0x800000;
  int32_t e2 = static_cast<int16_t>(static_cast<uint16_t>(e)) >> 1;
  e2 += 0x7F;
  return FromBits(SqrtTab()[m >> 16] | (static_cast<uint32_t>(e2) << 23));
}

// 0x53a0f8 (truncating fistp qword), low 16 bits.
static inline uint16_t Ftol16(double v) {
  if (!(v < 9223372036854775808.0 && v >= -9223372036854775808.0)) return 0;  // integer indefinite
  if (v > -2147483648.0 && v < 2147483648.0) return static_cast<uint16_t>(static_cast<int32_t>(v));
  return static_cast<uint16_t>(static_cast<int64_t>(v));
}

// ---- Detail (0x482550) -----------------------------------------------------

// One axis of the box distance: far^2 ("top") and near^2 ("second").
static inline void AxisDist(double cam, float mn, float mx, double* top, double* second) {
  const double a = cam - static_cast<double>(mn);
  const double b = cam - static_cast<double>(mx);
  if (!(a >= 0.0)) {
    const double bf = static_cast<float>(b);
    *second = a * a;
    *top = bf * bf;
  } else if (b > 0.0) {
    const double bf = static_cast<float>(b);
    const double af = static_cast<float>(a);
    *top = af * af;
    *second = bf * bf;
  } else {
    const double two = cam + cam;
    const double s = static_cast<double>(mn) + static_cast<double>(mx);
    const double e = (s > two) ? static_cast<double>(mx) : static_cast<double>(mn);
    const double d = e - cam;
    *top = d * d;
    *second = 0.0;
  }
}

float Detail(Node* n, int flag) {
  uint8_t* T = n->T;
  uint8_t* D = n->D;
  const double camx = Tf(T, kCamX), camy = Tf(T, kCamY), camz = Tf(T, kCamZ);
  double xt, xs, zt, zs;
  AxisDist(camx, Df(D, kMinX), Df(D, kMaxX), &xt, &xs);
  AxisDist(camz, Df(D, kMinZ), Df(D, kMaxZ), &zt, &zs);
  const float s1 = static_cast<float>(zs + xs);  // nearest point, squared xz distance
  const float s0 = static_cast<float>(zt + xt);  // farthest
  const float dy0 = static_cast<float>(camy - static_cast<double>(n->y18));
  const float dy1 = static_cast<float>(camy - static_cast<double>(n->y1c));
  float ysq, dysel;
  if (!(static_cast<double>(dy0) >= 0.0)) {
    ysq = static_cast<float>(static_cast<double>(dy0) * dy0);
    dysel = dy0;
  } else if (static_cast<double>(dy1) > 0.0) {
    ysq = static_cast<float>(static_cast<double>(dy1) * dy1);
    dysel = dy1;
  } else {
    ysq = 0.0f;
    dysel = 0.0f;
  }
  const float r = GameSqrt(static_cast<float>(static_cast<double>(ysq) + s1));
  if (flag) return r;

  ++Ti(T, kCntDetail);
  Dw(D, kPrevLo) = Dw(D, kLo);
  Dw(D, kPrevHi) = Dw(D, kHi);
  const double fac = Df(D, kFactor);
  const double k74 = Tf(T, kK74);
  bool general = false;
  if (Ti(T, kTol) == 0) {
    Dw(D, kHi) = 0;
  } else if (EqZ(s1)) {
    Dw(D, kHi) = 0xFFFF;
  } else if (EqZ(s0)) {
    Dw(D, kHi) = 0xFFFF;
  } else {
    general = true;
  }
  if (!general) {
    double q;
    if (!(static_cast<double>(ysq) >= s1))
      q = GameSqrt(s1);
    else if (static_cast<double>(ysq) > s0)
      q = GameSqrt(s0);
    else
      q = dysel;
    const double v = ((static_cast<double>(ysq) / q + q) * fac) * k74;
    // 0x48280f, with TerrainLodFix's clamp.
    ++g_stats.lod_calls;
    uint16_t w;
    if (v > 65535.0) {
      ++g_stats.lod_over;
      w = g_opt.lod_fix ? 0xFFFF : Ftol16(v);
    } else {
      w = Ftol16(v);
    }
    Dw(D, kLo) = w;
    return r;
  }
  const float r1 = GameSqrt(s1);
  const float r0 = GameSqrt(s0);
  double q;
  if (!(static_cast<double>(ysq) >= s1))
    q = r1;
  else if (static_cast<double>(ysq) > s0)
    q = r0;
  else
    q = dysel;
  const double v = ((q + static_cast<double>(ysq) / q) * fac) * k74;
  Dw(D, kLo) = (v >= 65535.0) ? 0xFFFF : Ftol16(v);
  float a0 = dy0;
  if (!(static_cast<double>(a0) >= 0.0)) a0 = -a0;
  double a1 = dy1;
  if (!(a1 >= 0.0)) a1 = -a1;
  const double m = (static_cast<double>(a0) > a1) ? static_cast<double>(a0) : a1;
  const double m2 = m * m;
  const float t1 = static_cast<float>((m2 + s1) / r1);
  const double t0 = (m2 + s0) / r0;
  const double u = (static_cast<double>(t1) > t0) ? static_cast<double>(t1) : t0;
  const double v2 = u * (fac * k74);
  Dw(D, kHi) = (v2 >= 65535.0) ? 0xFFFF : Ftol16(v2);
  return r;
}

// ---- Eval (0x4836e0) and the vertex dependency graph -----------------------

static void Set(Node* n, int x, int y, int flag, int mask, int noprop);
static void FarSet(Node* n, int X, int Y, int flag, int mask, const uint8_t* vptr, int level);

// 0x482c30: 1 when the vertex's error is too small to show from the camera.
static inline int Test(Node* n, int x, int y) {
  const int v = kRow[y] + x;
  const uint8_t* vi = n->vinfo + v * 8;
  uint8_t* T = n->T;
  const double s10 = n->s10, s14 = n->s14;
  double e = static_cast<double>(Err()[vi[1]]) * s14;
  e = e * s10;
  const int sh = n->shift & 31;
  const int gxs = static_cast<int>((static_cast<uint32_t>(n->gx) + static_cast<uint32_t>(x)) << sh);
  const int gys = static_cast<int>((static_cast<uint32_t>(n->gy) + static_cast<uint32_t>(y)) << sh);
  const int base = *reinterpret_cast<const int32_t*>(n->vinfo + 0xD4C);
  const int hb = static_cast<int>(*reinterpret_cast<const uint16_t*>(vi + 2)) - base;
  const double dx = static_cast<double>(Tf(T, kCamX)) - static_cast<double>(gxs);
  const double dz = static_cast<double>(Tf(T, kCamZ)) - static_cast<double>(gys);
  const double hy = (static_cast<double>(hb) * s10 + static_cast<double>(base)) * s14;
  const double dy = static_cast<float>(static_cast<double>(Tf(T, kCamY)) - hy);
  ++Ti(T, kCntTest);
  const double dxz2 = dz * dz + dx * dx;
  const double d2 = dy * dy + dxz2;
  const double rhs = d2 * (d2 * static_cast<double>(Tf(T, kK78)));
  const double lhs = (dxz2 * e) * e;
  return (lhs > rhs) ? 0 : 1;
}

// 0x483c10: a vertex changed; update the vertices that depend on it.
static void Dep(Node* n, int x, int y, int flag) {
  ++Ti(n->T, kCntDep);
  if (x == 8 && y == 8) {
    Node* p = n->parent;
    if (!p) return;
    const int id = n->idx;
    const int px = id & 0xF, py = id >> 4;
    const int m = id & 0x11;
    if (m != 0x11 && m != 0) {
      Set(p, px, py, flag, 8, 0);
      Set(p, px + 1, py + 1, flag, 1, 0);
    } else {
      Set(p, px, py + 1, flag, 2, 0);
      Set(p, px + 1, py, flag, 4, 0);
    }
    return;
  }
  const int32_t* e = Vt() + (kRow[y] + x) * 8;
  if (e[2] == e[4] && e[3] == e[5]) return;
  Set(n, e[2], e[3], flag, e[6], 0);
  Set(n, e[4], e[5], flag, e[7], 0);
}

// 0x483e80: a vertex lost its last dependant; may it go? 1 = it went.
static int Release(Node* n, int x, int y) {
  const int v = kRow[y] + x;
  uint8_t* D = n->D;
  uint8_t* T = n->T;
  const uint8_t imp = n->vinfo[v * 8 + 1];
  if (n->vis != 0) {
    if (*reinterpret_cast<int32_t*>(D + kDStamp) == Ti(T, kFrame) ||
        Ti(T, kStamps + v * 4) == Ti(T, kStamp)) {
      if (Test(n, x, y)) {
        D[v] &= 0xBF;
        const uint8_t f = D[v];
        if (!(f & 0x80)) return 0;
        if (f & 0x10) return 0;
        D[v] = f & 0x7F;
        return 1;
      }
      D[v] |= 0xC0;
      return 0;
    }
  }
  uint8_t f = D[v];
  if (!(f & 0x40)) {
    if (imp > Band()[Dw(D, kHi)]) {
      D[v] = f | 0xC0;
      return 1;
    }
    if (f & 0x10) return 0;
    D[v] = f & 0x7F;
    return 1;
  }
  if (imp >= Band()[Dw(D, kLo)]) return 0;
  D[v] = f & 0xBF;
  f = D[v];
  if (f & 0x10) return 0;
  D[v] = f & 0x7F;
  return 1;
}

// 0x484540 (this = the parent p): an edge vertex of child c changed; pass it
// to the neighbouring child(ren), or through the root for the outer edge.
static void Neighbour(Node* p, Node* c, int x, int y, int flag, int mask, const uint8_t* vptr) {
  const int id = c->idx;
  const int lo = id & 0xF, hi = id & 0xF0;
  const int np = 1;  // Set's noprop argument: the game passes vptr (never 0)
  if (lo != 0 && lo != 0xF && hi != 0 && hi != 0xF0) {
    Node** k = p->kids;
    Node* s;
    if (x == 0) {
      if ((s = k[id - 1]) != nullptr) Set(s, 16, y, flag, mask, np);
      if (y == 0) {
        if ((s = k[id - 0x11]) != nullptr) Set(s, 16, 16, flag, mask, np);
        if ((s = p->kids[id - 0x10]) != nullptr) Set(s, 0, 16, flag, mask, np);
      } else if (y == 16) {
        if ((s = p->kids[id + 0xF]) != nullptr) Set(s, 16, 0, flag, mask, np);
        if ((s = p->kids[id + 0x10]) != nullptr) Set(s, 0, 0, flag, mask, np);
      }
      return;
    }
    if (x == 16) {
      if ((s = k[id + 1]) != nullptr) Set(s, 0, y, flag, mask, np);
      if (y == 0) {
        if ((s = p->kids[id - 0xF]) != nullptr) Set(s, 0, 16, flag, mask, np);
        if ((s = p->kids[id - 0x10]) != nullptr) Set(s, 16, 16, flag, mask, np);
      } else if (y == 16) {
        if ((s = p->kids[id + 0x11]) != nullptr) Set(s, 0, 0, flag, mask, np);
        if ((s = p->kids[id + 0x10]) != nullptr) Set(s, 16, 0, flag, mask, np);
      }
      return;
    }
    if (y == 0) {
      if ((s = k[id - 0x10]) != nullptr) Set(s, x, 16, flag, mask, np);
    } else if (y == 16) {
      if ((s = k[id + 0x10]) != nullptr) Set(s, x, 0, flag, mask, np);
    }
    return;
  }
  Node* root = p;
  while (root->parent) root = root->parent;
  int lv = static_cast<int>(p->level) - 1;
  const int sh = lv > 0 ? lv * 4 : 0;
  const int X = static_cast<int>((static_cast<uint32_t>(c->gx) + static_cast<uint32_t>(x)) << sh);
  const int Y = static_cast<int>((static_cast<uint32_t>(c->gy) + static_cast<uint32_t>(y)) << sh);
  FarSet(root, X, Y, flag, mask, vptr, c->level);
}

// Signed division by 16 rounding towards zero (cdq / and 0fh / add / sar 4).
static inline int Div16(int v) { return (v + ((v >> 31) & 0xF)) >> 4; }

// 0x484180: find the node(s) of level > "level" holding world vertex (X, Y)
// (in this node's units) and set the vertex there.
static void FarSet(Node* n, int X, int Y, int flag, int mask, const uint8_t* vptr, int level) {
  for (;;) {
    if (n->level == 0) {
      const uint8_t* vp = n->vinfo + (kRow[Y] + X) * 8;
      if (vp != vptr) Set(n, X, Y, flag, mask, vptr != nullptr);
      return;
    }
    const int sh = n->level * 4;
    const int S = 1 << (sh & 31);
    const int m = S - 1;
    const int rx = X & m;
    if (rx == 0 && (Y & m) == 0) {
      const int vx = X >> sh, vy = Y >> sh;
      const uint8_t* vp = n->vinfo + (kRow[vy] + vx) * 8;
      if (vp != vptr) Set(n, vx, vy, flag, mask, vptr != nullptr);
    }
    Node** k = n->kids;
    if (!k) return;
    if (static_cast<int>(n->level) <= level) return;
    int xa = X, ya = Y, xb = X - 1, yb = Y - 1;
    for (int i = n->level; i != 0; --i) {
      xa = Div16(xa);
      ya = Div16(ya);
      xb = Div16(xb);
      yb = Div16(yb);
    }
    Node* c;
    const int ym = Y & m;
    if (xb == xa) {
      if (xa >= 16) return;
      if (yb == ya) {
        if (ya >= 16) return;
        c = k[kKid[ya] + xa];
        if (!c) return;
        X = rx;
        Y = ym;
        n = c;
        continue;
      }
      if (ya < 16) {
        c = k[kKid[ya] + xa];
        if (c) FarSet(c, rx, ym, flag, mask, vptr, level);
      }
      if (yb >= 16) return;
      c = n->kids[kKid[yb] + xa];
      if (!c) return;
      X = rx;
      Y = ym + S;
      n = c;
      continue;
    }
    if (yb == ya) {
      if (xa < 16 && ya < 16) {
        c = k[kKid[ya] + xa];
        if (c) FarSet(c, rx, ym, flag, mask, vptr, level);
      }
      if (xb >= 16 || ya >= 16) return;
      c = n->kids[kKid[ya] + xb];
      if (!c) return;
      Y = ym;
      X = rx + S;
      n = c;
      continue;
    }
    if (xa < 16) {
      if (ya < 16) {
        c = k[kKid[ya] + xa];
        if (c) FarSet(c, rx, ym, flag, mask, vptr, level);
      }
      if (yb < 16) {
        c = n->kids[kKid[yb] + xa];
        if (c) FarSet(c, rx, Y & (m + S), flag, mask, vptr, level);
      }
    }
    if (xb >= 16) return;
    if (ya < 16) {
      c = n->kids[kKid[ya] + xb];
      if (c) FarSet(c, rx + S, ym, flag, mask, vptr, level);
    }
    if (yb >= 16) return;
    c = n->kids[kKid[yb] + xb];
    if (!c) return;
    X = rx + S;
    Y = ym + S;
    n = c;
  }
}

// 0x483d50: add (flag != 0) or remove a dependant (mask bit) of vertex (x, y).
static void Set(Node* n, int x, int y, int flag, int mask, int noprop) {
  if (x < 0 || y < 0 || x > 16 || y > 16) return;
  ++Ti(n->T, kCntSet);
  const int v = kRow[y] + x;
  uint8_t* D = n->D;
  const uint8_t* vptr = n->vinfo + v * 8;
  const uint8_t mk = static_cast<uint8_t>(mask);
  int changed = 0;
  bool dep;
  if (flag) {
    uint8_t f = D[v];
    if (!(mk & f)) {
      D[v] = f | mk;
      changed = 1;
    }
    f = D[v];
    if (f & 0x80) {
      if (!changed) return;
      dep = false;
    } else {
      D[v] = f | 0x80;
      dep = true;
    }
  } else {
    D[v] &= static_cast<uint8_t>(~mk);
    dep = !(D[v] & 0xF) && Release(n, x, y);
  }
  if (dep) {
    D[kFlags] |= 2;
    Dep(n, x, y, D[v] & 0x80);
  }
  Node* p = n->parent;
  if (!p || noprop) return;
  if (x == 0 || x == 16 || y == 0 || y == 16) Neighbour(p, n, x, y, flag, mask, vptr);
}

// 0x4839c0: test a vertex in the uncertain band and switch it on or off.
static inline void Update(Node* n, int x, int y) {
  ++Ti(n->T, kCntUpdate);
  const int v = kRow[y] + x;
  uint8_t* D = n->D;
  if (Test(n, x, y) == 0) {
    const uint8_t f = D[v] | 0x40;
    D[v] = f;
    if (f & 0x80) return;
    D[v] = f | 0x80;
  } else {
    D[v] &= 0xBF;
    const uint8_t f = D[v];
    if (f & 0x10) return;
    if (!(f & 0x80)) return;
    D[v] = f & 0x7F;
  }
  D[kFlags] |= 2;
  Dep(n, x, y, D[v] & 0x80);
}

static void EvalNode(Node* n);

static void EvalKids(Node* n) {
  Node** k = n->kids;
  // Prefetch: node lines a few children ahead, their data lines a bit later.
  for (int i = 0; i < 6; ++i)
    if (k[i]) Prefetch(&k[i]->vis);
  for (int i = 0; i < 256; ++i) {
    Node* c = n->kids[i];
    if (i + 6 < 256) {
      Node* a = k[i + 6];
      if (a) Prefetch(&a->vis);
    }
    if (i + 3 < 256) {
      Node* b = k[i + 3];
      if (b && b->vis) {
        Prefetch(b->D + kFlags);
        Prefetch(b->D);
      }
    }
    if (!c) continue;
    const uint32_t pv = n->vis;
    if (pv == 0x200000) {
      c->vis = 0x3FFFFF;
    } else if (pv == 0x3FFFFF && g_opt.hole_fix_hooked) {
      // TerrainHoleFix (see fixes_terrain.cpp), same counts as its stub.
      ++g_stats.hole_parents;
      if (c->vis != 0x3FFFFF) {
        ++g_stats.hole_fixed;
        if (g_opt.hole_fix) c->vis = 0x3FFFFF;
      }
    }
    EvalNode(c);
  }
}

static void EvalNode(Node* n) {
  if (n->vis == 0) return;
  if (n->kids && !(n->D[kFlags] & 1)) EvalKids(n);
  uint8_t* T = n->T;
  ++Ti(T, kStamp);
  uint8_t* D = n->D;
  const uint8_t* band = Band();
  const int a = band[Dw(D, kLo)];
  const int c = band[Dw(D, kPrevLo)];
  int b = band[Dw(D, kHi)];
  if (b != 0xFF) ++b;
  int d = band[Dw(D, kPrevHi)];
  if (d != 0xFF) ++d;
  const int16_t* counts = reinterpret_cast<const int16_t*>(n->vinfo + 0x908);
  const int16_t* order = reinterpret_cast<const int16_t*>(n->vinfo + 0xB08);
  const int32_t* vt = Vt();
  ++g_stats.eval_nodes;
  // Vertices that left the always-on band.
  for (int i = counts[c]; i < 0x121 && i < counts[a]; ++i) {
    const int v = order[i];
    D[v] &= 0xBF;
    if (D[v] & 0xF) continue;
    ++Ti(T, kCntOff);
    const uint8_t f = D[v];
    if ((f & 0x10) || !(f & 0x80)) continue;
    D[v] = f & 0x7F;
    D[kFlags] |= 2;
    Dep(n, vt[v * 8], vt[v * 8 + 1], 0);
  }
  // Vertices that entered it.
  if (d > b) {
    for (int i = counts[b]; i < 0x121 && i <= counts[d]; ++i) {
      const int v = order[i];
      D[v] |= 0x40;
      if (D[v] & 0xF) continue;
      ++Ti(T, kCntOn);
      const uint8_t f = D[v];
      if (f & 0x80) continue;
      D[v] = f | 0x80;
      D[kFlags] |= 2;
      Dep(n, vt[v * 8], vt[v * 8 + 1], 0x80);
    }
  }
  // The uncertain band: test each vertex against the camera.
  for (int i = counts[a]; i < 0x121 && i < counts[b]; ++i) {
    const int v = order[i];
    ++g_stats.eval_tests;
    if (D[v] & 0xF)
      Ti(T, kStamps + v * 4) = Ti(T, kStamp);
    else
      Update(n, vt[v * 8], vt[v * 8 + 1]);
  }
  *reinterpret_cast<int32_t*>(D + kDStamp) = Ti(T, kFrame);
}

void Eval(Node* n) { EvalNode(n); }

// ---- Draw walk (0x4818b0 / 0x481a40) ---------------------------------------

static void DrawBlocks(Node* n, int a1, int x, int y, int q, int a6);

static inline DrawFn PickDraw(const Node* n) {
  return reinterpret_cast<DrawFn>(At<int32_t>(n->T, kAltDraw) ? kDrawAlt : kDraw);
}

int DrawWalk(Node* n, int a1, int x, int y, int size, int q, int a6) {
  const uint8_t f = n->D[kFlags];
  if ((f & 1) && !(f & 8)) return 1;
  const int half = size >> 1;
  if (half == 8) {
    if (n->vis & 2) DrawWalk(n, a1, x, y, 8, 0, a6);
    if (n->vis & 4) DrawWalk(n, a1, x + 8, y, 8, 4, a6);
    if (n->vis & 8) DrawWalk(n, a1, x, y + 8, 8, 8, a6);
    if (n->vis & 0x10) DrawWalk(n, a1, x + 8, y + 8, 8, 0xC, a6);
    return 1;
  }
  if (half == 4) {
    const int sh = q & 31;
    if (n->vis & (0x20u << sh)) DrawBlocks(n, a1, x, y, q, a6);
    if (n->vis & (0x40u << sh)) DrawBlocks(n, a1, x + 4, y, q + 1, a6);
    if (n->vis & (0x80u << sh)) DrawBlocks(n, a1, x, y + 4, q + 2, a6);
    if (n->vis & (0x100u << sh)) DrawBlocks(n, a1, x + 4, y + 4, q + 3, a6);
  }
  return 1;
}

// 0x481a40: one 4x4 sub-block: its part of the node's own mesh if the node
// draws it, then the children under it (each drawn whole if collapsed).
static void DrawBlocks(Node* n, int a1, int /*x*/, int /*y*/, int q, int a6) {
  uint8_t* D = n->D;
  ++g_stats.draw_blocks;
  const uint8_t* tab = *reinterpret_cast<uint8_t* const*>(D + kSubTab);
  if (tab) {
    const int s = kSubMap[q & 15];
    int v0 = 0, i0 = 0, vc, ic;
    const int16_t* e = reinterpret_cast<const int16_t*>(tab + s * 12);
    if (s == 0) {
      vc = e[0];
      ic = e[1];
    } else {
      const int16_t* pe = reinterpret_cast<const int16_t*>(tab + (s - 1) * 12);
      v0 = pe[0];
      i0 = pe[1];
      vc = e[0] - v0;
      ic = e[1] - i0;
    }
    if (vc != 0 && (static_cast<int>(*reinterpret_cast<const int16_t*>(D + kSubMask)) & (1 << s))) {
      uint8_t* verts = *reinterpret_cast<uint8_t**>(D + kVerts) + v0 * 32;
      uint8_t* idx = *reinterpret_cast<uint8_t**>(D + kIdx) + i0 * 2;
      ++g_stats.draw_calls;
      PickDraw(n)(n, 0, a6, verts, vc, idx, ic, s);
    }
  }
  if (!n->kids || (n->D[kFlags] & 1)) return;
  Node** k = n->kids;
  const int base = kSubKid[q & 15];
  Node* c[16];
  for (int r = 0, j = 0; r < 4; ++r)
    for (int i = 0; i < 4; ++i, ++j) {
      c[j] = k[base + r * 16 + i];
      if (c[j]) Prefetch(&c[j]->vis);
    }
  for (int j = 0; j < 16; ++j)
    if (c[j] && c[j]->vis) {
      Prefetch(c[j]->D + kFlags);
      Prefetch(c[j]->D + kVCount);
    }
  for (int j = 0; j < 16; ++j) {
    // The game reloads the child pointer; drawing cannot change the tree.
    Node* ch = n->kids[base + (j >> 2) * 16 + (j & 3)];
    if (!ch || !ch->vis) continue;
    uint8_t* cd = ch->D;
    const int16_t vc = *reinterpret_cast<const int16_t*>(cd + kVCount);
    if (vc && !(cd[kFlags] & 8)) {
      ++g_stats.draw_calls;
      PickDraw(n)(ch, 0, a6, *reinterpret_cast<uint8_t**>(cd + kVerts), vc, *reinterpret_cast<uint8_t**>(cd + kIdx),
                  *reinterpret_cast<const int16_t*>(cd + kICount), -1);
    }
    DrawWalk(ch, a1, 0, 0, 0x10, 0, a6);
  }
}

// ---- FPU check ---------------------------------------------------------------

bool FpuIs53() {
  uint16_t cw = 0;
#if defined(_MSC_VER)
  __asm fnstcw cw
#else
  __asm__ __volatile__("fnstcw %0" : "=m"(cw));
#endif
  return (cw & 0x300) == 0x200;
}

}  // namespace tf
