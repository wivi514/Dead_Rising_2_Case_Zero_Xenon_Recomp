// Native keyboard/mouse through the title's OWN input layer (part 92, executing
// docs/native-kbm-plan.md phases B+C; every address below is derived in
// docs/native-kbm-phaseA.md and none is guessed).
//
// THE DESIGN, second iteration. The first build connected the title's own dormant
// KEYBOARD CONTROLLER CLASS on engine port 2 — and the whole chain worked (the
// title's parser resolved all 133 of our bindings for that port, its context
// pass accepted them, and a synthetic ENTER fired COMMAND_FRONTEND_A_BUTTON into
// the title's own engagement scan) — but the ENGAGE path for a class-0 controller
// then crashed on a null in profile machinery the 360 build never expected to run
// for a keyboard (its keyboard support is compiled out one level further up than
// the connect). The evidence from that build redirects the design rather than
// killing it:
//
//   * The command QUERY histogram (CZ_KBM_TRACE=1 hooks below) shows the game
//     reads commands for EVERY port — and port 0's command path, including
//     engagement, is exercised by every pad press of every boot. Port 0 is the
//     fully-supported road.
//   * Every controller object carries ALL 95 source records — the pad's key
//     sources (cat 0) simply have no feeder, and the pad's own state conversion
//     (sub_828070E0) never touches them.
//
// So the native path now targets THE PLAYER'S OWN PORT 0:
//
//   1. SPLICE. Our key bindings (DR2 PC's keymap defaults translated into this
//      image's vocabulary — tools/gen_kbm_map.py, player-editable override at
//      <root>/kbmap.txt) are woven directly into port 0's binding records after
//      the title parses its own padmap: a command with a free second source gets
//      the key as src2 with OR — the exact two-source shape the title's own
//      padmap uses — and an unbound command gets the whole line. The record
//      layout (24 bytes: flag/src1/mode1/src2/mode2/comb) and the mode enum
//      (HELD=0 PRESSED=1 RELEASED=2 REPEAT=3 ACCELREPEAT=4 TAP1=5 TAP2=6
//      QUICKTIMEDRELEASE=7 NONE=8; comb NONE=0 AND=1 NOT=2 OR=3) were both
//      verified against the pad's own parsed records. Lines whose sources are
//      all BUTTON_* (DR2's mousemap lines) are NOT spliced: the mouse reaches
//      those commands at the source level below, through the pad's own
//      face-button bindings. If the title re-parses padmap (a pad reconnect),
//      the splice notices its sentinel gone and re-applies.
//
//   2. KEY SOURCES. SDL key events (host/window.cpp translates scancode -> the
//      Windows VK codes the image's own token table carries) feed the pad-0
//      controller's key source records via the title's own setter
//      (sub_828049D8) — the same calls the dormant keystroke handler makes,
//      aimed at the port the player actually uses. VK -> source index comes
//      from the guest's own token table, read at verify time.
//
//   3. STICK/BUTTON OVERRIDES, post-conversion. A strong hook on the pad state
//      conversion (sub_828070E0) runs the title's own conversion first, then
//      for the port-0 controller:
//        - WASD held  -> LEFT_THUMBSTICK X/Y/DIR/MAG + the four half sources,
//          full deflection, normalized diagonals — no deadzone rescale, no
//          response curve: the A/S/D crispness the operator asked for.
//        - mouse      -> RIGHT_THUMBSTICK X/Y = the pad's own converted value
//          PLUS raw pixel deltas x sensitivity, deliberately unclamped — the
//          on-demand evaluator passes source values straight through, which is
//          what frees the camera from the stick's turn-rate ceiling (DR2 PC's
//          MOUSE_RAW wiring, phaseA A.3).
//        - mouse buttons -> the face-button/trigger sources (left=BUTTON_3/X,
//          right=BUTTON_R2 trigger, middle=BUTTON_4/Y — semantically DR2 PC's
//          mousemap, reached through the pad's own two-source lines).
//      The conversion rewrites every source every tick, so every override is
//      self-healing: stop writing and the pad's own values are back next tick.
//
//   4. KEYSTROKES for DlgKeyboard: XamInputGetKeystrokeEx still serves the
//      queued key events for any-user polls (the on-screen keyboard is its one
//      caller now), and stays honest-empty otherwise.
//
// The wheel arrives as synthetic KEY_1/KEY_3 taps (DR2 PC's own mousemap pairs
// every wheel binding with those keys).
//
// CZ_NO_NATIVE_KBM=1 is the whole-feature control arm: no splice, no hooks
// firing, the keystroke import empty as before, and window.cpp's part-91 v1
// keyboard->pad-0 merge carries the input exactly as shipped. While the host
// settings panel is up, the v1 merge also comes back so panel navigation keeps
// working (window.cpp's gate).
//
// CAPTURE-THEN-VERIFY (pc_options.cpp's discipline): before the first guest
// call, the token table must read "KEY_A", the command table must read
// "COMMAND_USER_CAM_LEFTRIGHT" at index 216, and every guest function used must
// be a known function start. Any mismatch declines the whole feature loudly.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "../kernel/heap.h"
#include "../kernel/klog.h"
#include "../kernel/memory.h"
#include "../host/host_paths.h"
#include "../host/settings.h"
#include "kbm_default_map.h"
#include "native_kbm.h"
#include "ppc_recomp_shared.h"

extern "C" PPC_FUNC(__imp__sub_828053C8);
extern "C" PPC_FUNC(__imp__sub_82805510);
extern "C" PPC_FUNC(__imp__sub_828070E0);

namespace
{

// ---- guest addresses (docs/native-kbm-phaseA.md; verified before use) --------
constexpr uint32_t kPortMap      = 0x82AD65E8; // 16 x u32 controller-per-port
constexpr uint32_t kTokenNames   = 0x829F3930; // 95 name ptrs (entry 0 = NONE)
constexpr uint32_t kTokenValues  = 0x829F3AB0; // parallel values (VK for keys)
constexpr uint32_t kTokenCats    = 0x829F3C30; // parallel categories (0 = key)
constexpr uint32_t kCmdTable     = 0x829DC810; // 305 command-name pointers
constexpr uint32_t kFnSetSource  = 0x828049D8; // SetSource(this, idx, f1, f2)
constexpr uint32_t kBindRecords  = 0x82AD6CF8; // 16 x { .., array, count, fn }
constexpr uint32_t kTokenCount   = 95;
constexpr uint32_t kCmdCount     = 305;

// Source indices = token-table indices (entry 0 is NONE).
constexpr uint32_t kSrcLtX = 67, kSrcLtY = 68, kSrcLtDir = 69, kSrcLtMag = 70;
constexpr uint32_t kSrcRtX = 71, kSrcRtY = 72;
constexpr uint32_t kSrcBtn3 = 77, kSrcBtn4 = 78, kSrcBtnR2 = 85;
constexpr uint32_t kSrcLtUp = 87, kSrcLtRight = 88, kSrcLtDown = 89, kSrcLtLeft = 90;
constexpr uint32_t kSrcLShift = 52, kSrcRShift = 53, kSrcLCtrl = 54, kSrcRCtrl = 55;
constexpr uint32_t kSrcLAlt = 56, kSrcRAlt = 57;

// The mode/combiner enums, verified against the pad's own parsed records
// (FRONTEND_A_BUTTON read {src1=75, mode1=1(PRESSED), mode2=8(NONE), comb=0}).
enum Mode { M_HELD = 0, M_PRESSED, M_RELEASED, M_REPEAT, M_ACCELREPEAT,
            M_TAP1, M_TAP2, M_QTR, M_NONE };
enum Comb { C_NONE = 0, C_AND, C_NOT, C_OR };

constexpr uint32_t ERROR_EMPTY_ = 0x490;

enum class Phase : int { Unstarted, Waiting, Active, Declined };
std::atomic<Phase> g_phase{ Phase::Unstarted };

// Built at verify time from the guest's own tables.
uint16_t g_vkToSrc[256];                       // VK -> source index (0 = none)
struct Binding
{
    uint16_t cmd;
    uint8_t src1, mode1, src2, mode2, comb;
};
std::vector<Binding> g_splice;                 // resolved key bindings
uint16_t g_sentinelCmd = 0;                    // first spliced cmd, for re-splice
uint8_t g_sentinelSrc = 0;

// ---- window-thread state ----------------------------------------------------
struct Keystroke
{
    uint16_t vk, unicode, flags;
};
std::mutex g_queueMutex;
std::deque<Keystroke> g_dlgQueue;              // for XamInputGetKeystrokeEx
std::deque<Keystroke> g_srcQueue;              // for the key-source feed
std::atomic<uint32_t> g_wasd{ 0 };
std::atomic<int> g_mouseDX{ 0 }, g_mouseDY{ 0 };
std::atomic<uint32_t> g_mouseButtons{ 0 };

bool TraceOn()
{
    static const bool on = getenv("CZ_KBM_TRACE") != nullptr;
    return on;
}

uint32_t LoadU32(uint8_t* base, uint32_t addr)
{
    uint32_t v;
    memcpy(&v, base + addr, 4);
    return __builtin_bswap32(v);
}

void StoreU32(uint8_t* base, uint32_t addr, uint32_t v)
{
    v = __builtin_bswap32(v);
    memcpy(base + addr, &v, 4);
}

float LoadF32(uint8_t* base, uint32_t addr)
{
    const uint32_t v = LoadU32(base, addr);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

const char* GuestStr(uint8_t* base, uint32_t addr)
{
    return reinterpret_cast<const char*>(base + addr);
}

bool GuestCall(PPCContext& ctx, uint8_t* base, uint32_t fnAddr, const char* what)
{
    PPCFunc* fn = g_memory.FindFunction(fnAddr);
    if (!fn)
    {
        fprintf(stderr, "[kbm] %s: %08X is not a known function start — REFUSED\n",
                what, fnAddr);
        return false;
    }
    fn(ctx, base);
    return true;
}

// SetSource(this, idx, value, dt) — the title's own setter; it maintains the
// record's edge/held state from value transitions, so a level write per tick is
// exactly what its own callers do.
void SetSource(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t idx,
               float value, float dt)
{
    ctx.r3.u64 = obj;
    ctx.r4.u64 = idx;
    ctx.f1.f64 = double(value);
    ctx.f2.f64 = double(dt);
    GuestCall(ctx, base, kFnSetSource, "set-source");
}

float SourceValue(uint8_t* base, uint32_t obj, uint32_t idx)
{
    return LoadF32(base, obj + 0x11D8 + idx * 0x30 + 8);
}

// ---- the map: our own reader of the DR2-PC line format -----------------------
// The title's parser exists and works (the port-2 build proved it end to end),
// but splicing into port 0's LIVE records needs per-line merge decisions its
// whole-table parser cannot make, so the lines are read here and resolved
// against the guest's own name tables — the same authorities its parser uses.

int LookupName(uint8_t* base, uint32_t tableBase, uint32_t count,
               const std::string& name)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t p = LoadU32(base, tableBase + 4 * i);
        if (p && name == GuestStr(base, p))
            return int(i);
    }
    return -1;
}

int LookupMode(const std::string& w)
{
    static const char* names[] = { "held", "pressed", "released", "repeat",
                                   "accelrepeat", "tap1", "tap2",
                                   "quicktimedrelease", "none" };
    std::string lower;
    for (char c : w)
        lower.push_back(char(tolower(uint8_t(c))));
    for (int i = 0; i < 9; ++i)
        if (lower == names[i])
            return i;
    return -1;
}

int LookupComb(const std::string& w)
{
    static const char* names[] = { "none", "and", "not", "or" };
    std::string lower;
    for (char c : w)
        lower.push_back(char(tolower(uint8_t(c))));
    for (int i = 0; i < 4; ++i)
        if (lower == names[i])
            return i;
    return -1;
}

std::string LoadMapText()
{
    const auto override_ = HostPaths::Root() / "kbmap.txt";
    std::ifstream f(override_, std::ios::binary);
    if (f)
    {
        std::string text((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (!text.empty())
        {
            fprintf(stderr, "[kbm] bindings: %s (%zu bytes, player override)\n",
                    override_.string().c_str(), text.size());
            return text;
        }
    }
    fprintf(stderr, "[kbm] bindings: built-in DR2-PC defaults "
                    "(create <root>/kbmap.txt to override)\n");
    return kKbmDefaultMap;
}

bool IsKeyToken(uint8_t* base, int idx)
{
    return idx > 0 && LoadU32(base, kTokenCats + 4 * uint32_t(idx)) == 0;
}

// Parse the map text into the splice list. Non-KEY-only lines (DR2's mousemap
// entries) are skipped by design — the mouse reaches those commands through the
// pad's own face-button sources. Unknown names are counted and named, never
// silently dropped in bulk.
void BuildSplice(uint8_t* base)
{
    g_splice.clear();
    const std::string text = LoadMapText();
    int lineNo = 0, skippedMouse = 0, bad = 0;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        ++lineNo;
        if (const size_t c = line.find("//"); c != std::string::npos)
            line.resize(c);
        // tokenize on ( , ) and whitespace
        std::vector<std::string> tok;
        std::string cur;
        for (char ch : line)
        {
            if (ch == '(' || ch == ')' || ch == ',' || ch == ' ' || ch == '\t' ||
                ch == '\r')
            {
                if (!cur.empty())
                    tok.push_back(cur), cur.clear();
            }
            else
                cur.push_back(ch);
        }
        if (!cur.empty())
            tok.push_back(cur);
        if (tok.empty())
            continue;
        if (tok.size() != 6)
        {
            fprintf(stderr, "[kbm] map line %d malformed (%zu tokens) — skipped\n",
                    lineNo, tok.size());
            ++bad;
            continue;
        }
        const int cmd = LookupName(base, kCmdTable, kCmdCount, tok[0]);
        const int s1 = LookupName(base, kTokenNames, kTokenCount, tok[1]);
        const int m1 = LookupMode(tok[2]);
        const int s2 = LookupName(base, kTokenNames, kTokenCount, tok[3]);
        const int m2 = LookupMode(tok[4]);
        const int cb = LookupComb(tok[5]);
        if (cmd < 0 || s1 < 0 || m1 < 0 || s2 < 0 || m2 < 0 || cb < 0)
        {
            fprintf(stderr, "[kbm] map line %d (%s): unresolved name — skipped\n",
                    lineNo, tok[0].c_str());
            ++bad;
            continue;
        }
        if (!IsKeyToken(base, s1) && !IsKeyToken(base, s2))
        {
            ++skippedMouse;   // BUTTON_*/stick-only line: source-level mouse merge
            continue;
        }
        g_splice.push_back({ uint16_t(cmd), uint8_t(s1), uint8_t(m1),
                             uint8_t(s2), uint8_t(m2), uint8_t(cb) });
    }
    fprintf(stderr, "[kbm] map: %zu key bindings resolved, %d mouse-side lines "
                    "folded into the source merge, %d bad\n",
            g_splice.size(), skippedMouse, bad);
}

// Weave the key bindings into port 0's live binding records. Policy, in order:
// whole line into an unbound record; else the line's first KEY source into a
// free src2 with OR; else skip and say so. Returns how many landed.
uint32_t ApplySplice(uint8_t* base)
{
    const uint32_t array = LoadU32(base, kBindRecords + 4);
    if (!array)
        return 0;
    uint32_t applied = 0, skipped = 0;
    g_sentinelCmd = 0;
    for (const Binding& b : g_splice)
    {
        const uint32_t rec = array + uint32_t(b.cmd) * 24;
        const uint32_t src1 = LoadU32(base, rec + 4);
        if (src1 == 0)
        {
            base[rec] = 1;
            StoreU32(base, rec + 4, b.src1);
            StoreU32(base, rec + 8, b.mode1);
            StoreU32(base, rec + 0xC, b.src2);
            StoreU32(base, rec + 0x10, b.mode2);
            StoreU32(base, rec + 0x14, b.comb);
        }
        else if (LoadU32(base, rec + 0xC) == 0)
        {
            // the record's own src2 slot is free: key rides along with OR —
            // the exact shape the title's own two-source padmap lines use.
            const bool s1Key = IsKeyToken(base, b.src1);
            const uint8_t src = s1Key ? b.src1 : b.src2;
            const uint8_t mode = s1Key ? b.mode1 : b.mode2;
            StoreU32(base, rec + 0xC, src);
            StoreU32(base, rec + 0x10, mode);
            StoreU32(base, rec + 0x14, C_OR);
        }
        else
        {
            ++skipped;
            if (TraceOn())
                fprintf(stderr, "[kbm]   no free slot for cmd %u (%s)\n", b.cmd,
                        GuestStr(base, LoadU32(base, kCmdTable + 4 * b.cmd)));
            continue;
        }
        if (!g_sentinelCmd)
        {
            g_sentinelCmd = b.cmd;
            g_sentinelSrc = IsKeyToken(base, b.src1) ? b.src1 : b.src2;
        }
        ++applied;
    }
    fprintf(stderr, "[kbm] splice: %u of %zu key bindings applied to port 0 "
                    "(%u had no free slot)\n",
            applied, g_splice.size(), skipped);
    return applied;
}

bool SpliceIntact(uint8_t* base)
{
    if (!g_sentinelCmd)
        return true;
    const uint32_t array = LoadU32(base, kBindRecords + 4);
    if (!array)
        return false;
    const uint32_t rec = array + uint32_t(g_sentinelCmd) * 24;
    return LoadU32(base, rec + 4) == g_sentinelSrc ||
           LoadU32(base, rec + 0xC) == g_sentinelSrc;
}

bool BytesAre(uint8_t* base, uint32_t addr, const char* s)
{
    return memcmp(base + addr, s, strlen(s) + 1) == 0;
}

// The one-time structural check, plus the VK -> source map read from the
// guest's own token table.
bool VerifyImage(uint8_t* base)
{
    const uint32_t keyA = LoadU32(base, kTokenNames + 4);   // entry 1
    if (!keyA || !BytesAre(base, keyA, "KEY_A"))
    {
        fprintf(stderr, "[kbm] verify FAILED: token table at %08X does not read "
                        "KEY_A — native KB/M DISABLED (v1 merge stays)\n",
                kTokenNames);
        return false;
    }
    const uint32_t cam = LoadU32(base, kCmdTable + 216 * 4);
    if (!cam || !BytesAre(base, cam, "COMMAND_USER_CAM_LEFTRIGHT"))
    {
        fprintf(stderr, "[kbm] verify FAILED: command 216 is not "
                        "COMMAND_USER_CAM_LEFTRIGHT — native KB/M DISABLED\n");
        return false;
    }
    if (!g_memory.FindFunction(kFnSetSource))
    {
        fprintf(stderr, "[kbm] verify FAILED: %08X is not a function start — "
                        "native KB/M DISABLED\n", kFnSetSource);
        return false;
    }
    memset(g_vkToSrc, 0, sizeof g_vkToSrc);
    for (uint32_t i = 1; i < kTokenCount; ++i)
        if (LoadU32(base, kTokenCats + 4 * i) == 0)
        {
            const uint32_t vk = LoadU32(base, kTokenValues + 4 * i);
            if (vk < 256)
                g_vkToSrc[vk] = uint16_t(i);
        }
    return true;
}

// Port 0's own padmap has been parsed once the title's records carry sources —
// the gate that also proves the command registry is up.
bool Port0Parsed(uint8_t* base)
{
    const uint32_t count = LoadU32(base, kBindRecords + 8);
    const uint32_t array = LoadU32(base, kBindRecords + 4);
    if (!count || !array)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        if (LoadU32(base, array + i * 24 + 4) != 0)
            return true;
    return false;
}

// ---- the per-tick feed, run AFTER the pad's own state conversion -------------
void PostConversionFeed(PPCContext& ctx, uint8_t* base, uint32_t obj)
{
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    if (dt <= 0.0f || dt > 0.25f)
        dt = 1.0f / 60.0f;

    const bool live = !Settings_OverlayVisible();

    // Key sources from the event queue — the same SetSource calls the title's
    // own (dormant) keystroke handler makes, including the modifier pairs.
    std::deque<Keystroke> events;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        events.swap(g_srcQueue);
    }
    for (const Keystroke& ks : events)
    {
        if (ks.flags & 0x0004)
            continue;                              // repeat: level unchanged
        const bool down = (ks.flags & 0x0001) != 0;
        const uint16_t src = ks.vk < 256 ? g_vkToSrc[ks.vk] : 0;
        if (src && live)
            SetSource(ctx, base, obj, src, down ? 1.0f : 0.0f, 0.0f);
        SetSource(ctx, base, obj, kSrcLShift, (ks.flags & 0x8) ? 1.0f : 0.0f, 0.0f);
        SetSource(ctx, base, obj, kSrcRShift, (ks.flags & 0x8) ? 1.0f : 0.0f, 0.0f);
        SetSource(ctx, base, obj, kSrcLCtrl, (ks.flags & 0x10) ? 1.0f : 0.0f, 0.0f);
        SetSource(ctx, base, obj, kSrcRCtrl, (ks.flags & 0x10) ? 1.0f : 0.0f, 0.0f);
        SetSource(ctx, base, obj, kSrcLAlt, (ks.flags & 0x20) ? 1.0f : 0.0f, 0.0f);
        SetSource(ctx, base, obj, kSrcRAlt, (ks.flags & 0x20) ? 1.0f : 0.0f, 0.0f);
    }

    // WASD -> left stick, full deflection, normalized diagonals, overriding the
    // pad's converted values only while keys are held (the conversion rewrites
    // every tick, so releasing the keys hands the stick straight back). Signs:
    // the conversion negates Y between XInput and the source, and DIR is
    // atan2(x, yForward) with MAG deadzone-rescaled — all transcribed from
    // 0x828074A0..0x828074F0.
    const uint32_t wasd = live ? g_wasd.load(std::memory_order_acquire) : 0;
    if (wasd)
    {
        float x = float((wasd >> 3) & 1) - float((wasd >> 2) & 1);   // D - A
        float yFwd = float(wasd & 1) - float((wasd >> 1) & 1);       // W - S
        const float len = std::sqrt(x * x + yFwd * yFwd);
        if (len > 0.0f)
        {
            x /= len;
            yFwd /= len;
        }
        SetSource(ctx, base, obj, kSrcLtX, x, dt);
        SetSource(ctx, base, obj, kSrcLtY, -yFwd, dt);
        SetSource(ctx, base, obj, kSrcLtDir,
                  len > 0.0f ? std::atan2(x, yFwd) : 0.0f, dt);
        SetSource(ctx, base, obj, kSrcLtMag, len > 0.0f ? 1.0f : 0.0f, dt);
        SetSource(ctx, base, obj, kSrcLtUp, yFwd > 0.5f ? 1.0f : 0.0f, 0.0f);
        SetSource(ctx, base, obj, kSrcLtRight, x > 0.5f ? 1.0f : 0.0f, 0.0f);
        SetSource(ctx, base, obj, kSrcLtDown, yFwd < -0.5f ? 1.0f : 0.0f, 0.0f);
        SetSource(ctx, base, obj, kSrcLtLeft, x < -0.5f ? 1.0f : 0.0f, 0.0f);
    }

    // Mouse -> right stick sources, ADDITIVE with the pad's converted value and
    // deliberately unclamped (raw pixels x sensitivity — the DR2 PC camera).
    // Screen-down-positive matches the engine's source convention (the pad path
    // negates XInput's Y).
    if (live && Settings_MouseCam())
    {
        const int dx = g_mouseDX.exchange(0, std::memory_order_relaxed);
        const int dy = g_mouseDY.exchange(0, std::memory_order_relaxed);
        const float sens = float(Settings_MouseSens());
        const float s = sens * sens * 0.0045f;
        if (dx || dy)
        {
            SetSource(ctx, base, obj, kSrcRtX,
                      SourceValue(base, obj, kSrcRtX) + float(dx) * s, dt);
            SetSource(ctx, base, obj, kSrcRtY,
                      SourceValue(base, obj, kSrcRtY) + float(dy) * s, dt);
        }
    }

    // Mouse buttons onto the pad's own face-button/trigger sources: left =
    // quick attack (BUTTON_3/X), right = aim (the BUTTON_R2 trigger source),
    // middle = BUTTON_4/Y — DR2 PC's mousemap semantics through the pad's own
    // bindings. OR with the pad: only override while pressed.
    const uint32_t mb = live ? g_mouseButtons.load(std::memory_order_acquire) : 0;
    if (mb & 1)
        SetSource(ctx, base, obj, kSrcBtn3, 1.0f, 0.0f);
    if (mb & 2)
        SetSource(ctx, base, obj, kSrcBtnR2, 1.0f, 0.0f);
    if (mb & 4)
        SetSource(ctx, base, obj, kSrcBtn4, 1.0f, 0.0f);

    // CZ_KBM_TEST_KEYS=ms:vkhex[,...] — synthetic taps through the same queue
    // real SDL events use; the headless proof of the whole chain. Manufactures
    // progress: never a gate configuration for anything but itself.
    {
        static const char* seq = getenv("CZ_KBM_TEST_KEYS");
        if (seq && *seq)
        {
            static const auto t0 = std::chrono::steady_clock::now();
            static size_t cursor = 0;
            static std::vector<std::pair<long, uint16_t>> taps = [] {
                std::vector<std::pair<long, uint16_t>> v;
                const char* p = getenv("CZ_KBM_TEST_KEYS");
                while (p && *p)
                {
                    long ms = strtol(p, const_cast<char**>(&p), 10);
                    if (*p == ':')
                    {
                        const uint16_t vk =
                            uint16_t(strtol(p + 1, const_cast<char**>(&p), 16));
                        v.emplace_back(ms, vk);
                    }
                    while (*p == ',')
                        ++p;
                }
                return v;
            }();
            const long up = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            if (cursor < taps.size() && up >= taps[cursor].first)
            {
                fprintf(stderr, "[kbm] TEST tap vk=%02X at %ld ms (synthetic — "
                                "not a gate run)\n", taps[cursor].second, up);
                NativeKbm_PushKey(taps[cursor].second, 0, true, false, 0);
                NativeKbm_PushKey(taps[cursor].second, 0, false, false, 0);
                ++cursor;
            }
        }
    }
}

// ---- CZ_KBM_TRACE=1: who queries which PORT's commands ----------------------
std::atomic<uint64_t> g_queryByPort[2][16];

void DumpQueryHistogram()
{
    static auto lastDump = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - lastDump).count() < 10)
        return;
    lastDump = now;
    char line[256];
    int n = snprintf(line, sizeof line, "[kbm] cmd queries by port (bool/float):");
    for (int p = 0; p < 16; ++p)
    {
        const uint64_t b = g_queryByPort[0][p].load(std::memory_order_relaxed);
        const uint64_t f = g_queryByPort[1][p].load(std::memory_order_relaxed);
        if (b || f)
            n += snprintf(line + n, sizeof line - n, " p%d:%llu/%llu", p,
                          (unsigned long long)b, (unsigned long long)f);
    }
    fprintf(stderr, "%s\n", line);
}

} // namespace

// The command-value queries, hooked for the port histogram (trace-only cost).
PPC_FUNC(sub_828053C8)
{
    if (TraceOn())
    {
        if (ctx.r4.u32 < 16)
            g_queryByPort[0][ctx.r4.u32].fetch_add(1, std::memory_order_relaxed);
        DumpQueryHistogram();
    }
    __imp__sub_828053C8(ctx, base);
}

PPC_FUNC(sub_82805510)
{
    if (TraceOn())
    {
        if (ctx.r4.u32 < 16)
            g_queryByPort[1][ctx.r4.u32].fetch_add(1, std::memory_order_relaxed);
    }
    __imp__sub_82805510(ctx, base);
}

// The pad state-to-source conversion: the title's own conversion first, then
// the native overrides for the port-0 controller (see the module comment).
PPC_FUNC(sub_828070E0)
{
    const uint32_t self = ctx.r3.u32;
    __imp__sub_828070E0(ctx, base);
    if (!NativeKbm_Active())
        return;
    if (self && self == LoadU32(base, kPortMap))
    {
        if (!SpliceIntact(base))
        {
            fprintf(stderr, "[kbm] splice sentinel lost (padmap re-parsed?) — "
                            "re-applying\n");
            ApplySplice(base);
        }
        PostConversionFeed(ctx, base, self);
    }
}

bool NativeKbm_Enabled()
{
    static const bool off = getenv("CZ_NO_NATIVE_KBM") != nullptr;
    return !off;
}

bool NativeKbm_Active()
{
    return g_phase.load(std::memory_order_acquire) == Phase::Active;
}

void NativeKbm_Pump(PPCContext& ctx, uint8_t* base)
{
    (void)ctx;
    if (!NativeKbm_Enabled())
        return;
    Phase p = g_phase.load(std::memory_order_acquire);
    if (p == Phase::Active || p == Phase::Declined)
        return;
    if (p == Phase::Unstarted)
    {
        g_phase.store(VerifyImage(base) ? Phase::Waiting : Phase::Declined,
                      std::memory_order_release);
        if (g_phase.load(std::memory_order_relaxed) == Phase::Waiting)
            KLOG("[kbm] image verify OK — waiting for the title's own padmap "
                 "parse\n");
        return;
    }
    if (!Port0Parsed(base))
        return;
    BuildSplice(base);
    const uint32_t applied = ApplySplice(base);
    g_phase.store(applied ? Phase::Active : Phase::Declined,
                  std::memory_order_release);
    if (!applied)
        fprintf(stderr, "[kbm] nothing spliced — declining native KB/M "
                        "(v1 merge stays)\n");
}

void NativeKbm_HandleKeystroke(PPCContext& ctx, uint8_t* base)
{
    const uint32_t userPtr = ctx.r3.u32;
    const uint32_t out = ctx.r5.u32;

    if (out)
        memset(base + out, 0, 8);
    if (!NativeKbm_Active() || !out)
    {
        ctx.r3.u64 = ERROR_EMPTY_;
        return;
    }
    // Serves DlgKeyboard-style polls (typically user 0xFF with the any-user
    // flag). The key-source feed does not run here — it lives in the pad
    // conversion hook, which is per-tick regardless of who polls keystrokes.
    Keystroke ks;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (g_dlgQueue.empty())
        {
            ctx.r3.u64 = ERROR_EMPTY_;
            return;
        }
        ks = g_dlgQueue.front();
        g_dlgQueue.pop_front();
    }
    base[out + 0] = uint8_t(ks.vk >> 8);
    base[out + 1] = uint8_t(ks.vk);
    base[out + 2] = uint8_t(ks.unicode >> 8);
    base[out + 3] = uint8_t(ks.unicode);
    base[out + 4] = uint8_t(ks.flags >> 8);
    base[out + 5] = uint8_t(ks.flags);
    base[out + 6] = 0;
    base[out + 7] = 0;
    if (userPtr)
        StoreU32(base, userPtr, 0);
    ctx.r3.u64 = 0;
}

// ---- window-thread side ------------------------------------------------------

void NativeKbm_PushKey(uint16_t vk, uint16_t unicode, bool down, bool repeat,
                       uint16_t mods)
{
    if (!NativeKbm_Enabled())
        return;
    uint16_t flags = down ? 0x0001 : 0x0002;      // KEYDOWN / KEYUP
    if (repeat)
        flags |= 0x0004;                          // REPEAT
    flags |= mods;                                // 0x8 shift, 0x10 ctrl, 0x20 alt
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_srcQueue.size() >= 64)
        g_srcQueue.pop_front();
    g_srcQueue.push_back({ vk, unicode, flags });
    if (g_dlgQueue.size() >= 64)
        g_dlgQueue.pop_front();
    g_dlgQueue.push_back({ vk, unicode, flags });
}

void NativeKbm_MouseDelta(int dx, int dy)
{
    g_mouseDX.fetch_add(dx, std::memory_order_relaxed);
    g_mouseDY.fetch_add(dy, std::memory_order_relaxed);
}

void NativeKbm_MouseButtons(uint32_t mask)
{
    g_mouseButtons.store(mask, std::memory_order_release);
}

void NativeKbm_MouseWheel(int steps)
{
    // DR2 PC's mousemap pairs every wheel binding with KEY_1/KEY_3 alternates;
    // the map binds those keys, so a wheel step is a key tap.
    const uint16_t vk = steps > 0 ? 0x33 : 0x31;   // '3' up / '1' down
    for (int i = std::abs(steps); i > 0; --i)
    {
        NativeKbm_PushKey(vk, 0, true, false, 0);
        NativeKbm_PushKey(vk, 0, false, false, 0);
    }
}

void NativeKbm_MoveKeys(uint32_t wasdMask)
{
    g_wasd.store(wasdMask, std::memory_order_release);
}
