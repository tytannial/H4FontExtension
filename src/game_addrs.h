// Absolute virtual addresses inside heroes4.exe (2003 Complete, image base
// 0x400000). Every value here was read out of that exe's IDA database
// (heroes4.exe.i64) and is only valid for that build; docs/PROGRESS.md
// sections 2-7 record how each one was calibrated.
#ifndef H4CN_GAME_ADDRS_H_
#define H4CN_GAME_ADDRS_H_

#include <array>
#include <cstdint>

namespace game {

// An inline hook target.
//
// `len` is the number of prologue bytes that get overwritten and MUST end on an
// instruction boundary, because the trampoline re-executes a verbatim copy of
// those bytes and then jumps to addr + len. Copying a partial instruction makes
// the trampoline decode the jump that follows it as operand bytes.
//
// `prologue` holds the bytes verified in the IDB. The installer compares them
// before patching, so a different exe revision degrades to "no Chinese text"
// instead of corrupting code at a wrong address.
struct HookTarget {
  uint32_t addr;
  int len;
  std::array<uint8_t, 8> prologue;
};

// --- font.obj entry points --------------------------------------------------
// Lengths below are the first instruction-boundary at or after 5 bytes:
//   0x71BD50  51 53 55 | 8B 6C 24 1C          1+1+1+4 -> 7
//   0x71BDE0  53 56 | 8B 74 24 0C              1+1+4   -> 6
//   0x71BE40  51 53 55 56 | 8B 74 24 14        1*4+4   -> 8
//   0x71C100  6A FF | 68 A0 60 94 00           2+5     -> 7 (SEH)
//   0x71C5E0  8B 54 24 08 | 53                 4+1     -> 5
//   0x71C7F0  6A FF | 68 B8 60 94 00           2+5     -> 7 (SEH)
//   0x71C900  6A FF | 68 D8 60 94 00           2+5     -> 7 (SEH)

// t_font::draw_to(bitmap&, screen_point, const char*, u16, bool, u16)
inline constexpr HookTarget kFontDrawToPt{
    0x71BD50, 7, {0x51, 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x1C}};

// t_font::get_width(const char*)
inline constexpr HookTarget kFontGetWidth{
    0x71BDE0, 6, {0x53, 0x56, 0x8B, 0x74, 0x24, 0x0C}};

// t_font::get_column(const char*, int x)
inline constexpr HookTarget kFontGetColumn{
    0x71BE40, 8, {0x51, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x14}};

// t_font::wrap_text(t_string_vector&, const char*, int width)
inline constexpr HookTarget kFontWrapText{
    0x71C100, 7, {0x6A, 0xFF, 0x68, 0xA0, 0x60, 0x94, 0x00}};

// t_font::wrap_text(t_string_vector&, const char*, int min, int max)
inline constexpr HookTarget kFontWrapTextMm{
    0x71C5E0, 5, {0x8B, 0x54, 0x24, 0x08, 0x53}};

// t_font::draw_to(bitmap&, const screen_rect&, const char*, u16, bool, u16)
inline constexpr HookTarget kFontDrawToRect{
    0x71C7F0, 7, {0x6A, 0xFF, 0x68, 0xB8, 0x60, 0x94, 0x00}};

// t_font::get_wrapped_height(const char*, int width)
inline constexpr HookTarget kFontGetWrappedHeight{
    0x71C900, 7, {0x6A, 0xFF, 0x68, 0xD8, 0x60, 0x94, 0x00}};

// --- VC6 STL helpers instantiated inside the exe ----------------------------
// Used to rebuild a t_string_vector the way the game itself does, instead of
// hand-rolling the refcounted-string ABI.

inline constexpr uint32_t kAddrStrVecClear = 0x5ABC70;   // vector::erase(b, e)
inline constexpr uint32_t kAddrStrVecInsert = 0x440F50;  // vector::insert(p, v)
inline constexpr uint32_t kAddrStrAssign = 0x401D30;     // string::assign(p, n)
inline constexpr uint32_t kAddrStrTidy = 0x401CE0;       // string::_Tidy(bool)

// --- RGB565 blend masks -----------------------------------------------------
// Initialised by the game before the first draw. Read at blit time so the
// plugin follows the game rather than assuming a resolution/pixel format.
// Calibrated values: 0x000F81F0, 0x0000F81F, 0x00007E00, 0x000007E0.

inline constexpr uint32_t kAddrBlendMaskM0 = 0xAAF0D8;
inline constexpr uint32_t kAddrBlendMaskM1 = 0xAAF0DC;
inline constexpr uint32_t kAddrBlendMaskM2 = 0xAAF0F4;
inline constexpr uint32_t kAddrBlendMaskM3 = 0xAAF104;

inline constexpr uint32_t kFallbackBlendMaskM0 = 0x000F81F0;
inline constexpr uint32_t kFallbackBlendMaskM1 = 0x0000F81F;
inline constexpr uint32_t kFallbackBlendMaskM2 = 0x00007E00;
inline constexpr uint32_t kFallbackBlendMaskM3 = 0x000007E0;

}  // namespace game

#endif  // H4CN_GAME_ADDRS_H_
