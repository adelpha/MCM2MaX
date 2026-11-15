// Test DLL for terrain_harness.py: the FastTerrain rewrites with plain cdecl
// entry points, no imports, loaded at a fixed base inside an x86 emulator
// next to Mcm2.exe.
#include "../../src/terrain_fast.h"

extern "C" {
__declspec(dllexport) float H_Detail(tf::Node* n, int flag) { return tf::Detail(n, flag); }
__declspec(dllexport) void H_Eval(tf::Node* n) { tf::Eval(n); }
__declspec(dllexport) int H_Draw(tf::Node* n, int a1, int x, int y, int size, int q, int a6) {
  return tf::DrawWalk(n, a1, x, y, size, q, a6);
}
__declspec(dllexport) void H_SetOptions(int lod_fix, int hole_hooked, int hole_fix) {
  tf::g_opt.lod_fix = lod_fix != 0;
  tf::g_opt.hole_fix_hooked = hole_hooked != 0;
  tf::g_opt.hole_fix = hole_fix != 0;
}
__declspec(dllexport) tf::Stats* H_Stats() { return &tf::g_stats; }
int __stdcall HEntry(void*, unsigned, void*) { return 1; }
}
