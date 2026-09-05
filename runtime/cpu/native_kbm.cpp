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
//        - mouse buttons -> the pad's own sources (left=BUTTON_3/X = attack,
//          right=BUTTON_R2 = aim, middle=BUTTON_R3 = heavy attack/cam reset —
//          DR2 PC's mousemap semantics through the pad's own bindings).
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
#include <set>
#include <string>
#include <thread>
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
extern "C" PPC_FUNC(__imp__sub_82804AF8);

namespace
{

// memmem is a GNU extension the Windows CRT lacks, and the scan below is the one
// caller in the runtime. Same code on both platforms on purpose (an #ifdef'd
// glibc memmem would make the two legs scan differently): skip to the needle's
// first byte with memchr — for texel data the first byte is selective — then one
// memcmp. Called on device changes and rescans, never per frame.
const uint8_t* FindBytes(const uint8_t* hay, size_t hayLen,
                         const uint8_t* needle, size_t needleLen)
{
    if (needleLen == 0 || hayLen < needleLen)
        return nullptr;
    const uint8_t* p = hay;
    const uint8_t* end = hay + hayLen - needleLen + 1;
    while (p < end)
    {
        p = static_cast<const uint8_t*>(std::memchr(p, needle[0], size_t(end - p)));
        if (!p)
            return nullptr;
        if (std::memcmp(p, needle, needleLen) == 0)
            return p;
        ++p;
    }
    return nullptr;
}

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
constexpr uint32_t kSrcBtn3 = 77, kSrcBtnR2 = 85, kSrcBtnR3 = 86;
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
std::atomic<uint32_t> g_panelButtons{ 0 };   // XI bits mirrored from key levels
// camera surplus in stick units, XInput sign; bit-cast into int32 atomics
std::atomic<int32_t> g_camSurX{ 0 }, g_camSurY{ 0 };

void AtomicAddFloat(std::atomic<int32_t>& a, float v)
{
    int32_t oldBits = a.load(std::memory_order_relaxed);
    for (;;)
    {
        float f;
        memcpy(&f, &oldBits, 4);
        f += v;
        int32_t newBits;
        memcpy(&newBits, &f, 4);
        if (a.compare_exchange_weak(oldBits, newBits, std::memory_order_relaxed))
            return;
    }
}

float AtomicTakeFloat(std::atomic<int32_t>& a)
{
    const int32_t bits = a.exchange(0, std::memory_order_relaxed);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

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

void StoreF32(uint8_t* base, uint32_t addr, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    StoreU32(base, addr, v);
}

// Raw mouse-look accumulator for the DIRECT camera-look path (imported from Case
// West, part 93). window.cpp feeds it the same per-poll deltas it hands the stick;
// the camera hook (sub_824676C0) drains it once a frame and adds it straight onto
// the camera's yaw/pitch angles, past the engine's radial turn-rate clamp.
std::atomic<int> g_lookDX{ 0 }, g_lookDY{ 0 };
void TakeMouseLook(int& dx, int& dy)
{
    dx = g_lookDX.exchange(0, std::memory_order_relaxed);
    dy = g_lookDY.exchange(0, std::memory_order_relaxed);
}

// True while the KB/mouse (not a pad) is the live device — defined down by the
// device-follow state; forward-declared here for the direct-camera hook's gate.
bool MouseDeviceActive();

const char* GuestStr(uint8_t* base, uint32_t addr)
{
    return reinterpret_cast<const char*>(base + addr);
}

// DIRECT CAMERA LOOK — imported from Case West (part 93; docs/mouse-camera-uncap.md).
// The mouse fed the game's right-stick command, but the manual third-person camera
// turns that into a turn rate through a per-axis CLAMP, so any input-side gain (the
// part-91 CameraSurplus / DR2-PC source-unclamp already in this file) is clamped
// straight back — the "turn-rate ceiling at MOUSE SENS 10" the operator felt. Case
// West fixed it by writing the camera ANGLE directly, past the clamp.
//
// FINDING THE CAMERA, AND WHY THE ENTRY-HOOK APPROACH FAILED. The live camera update
// is the function at 0x8246F9A8 (found empirically: it is the sole consumer of camera
// command 216 COMMAND_USER_CAM_LEFTRIGHT / 217 UPDOWN, queried via sub_82805510 from
// lr=0x8246FA6C/0x8246FA84, during real gameplay). But the recompiler executes that
// code through a path that BYPASSES its entry symbol — a strong PPC_FUNC(sub_8246F9A8)
// override fires 0 times even across 784k-resolve gameplay (0x8246F9A4 just before it
// is a null word; both entries are dead). Two earlier shape-matched candidates
// (sub_82471EA0, sub_8246F9A8-as-entry, sub_824676C0) were all dead for this reason.
//
// THE WORKING BYPASS. The camera keeps its object in r31 (non-volatile) and calls the
// command query sub_82805510 FROM INSIDE ITSELF — and sub_82805510 DOES hook and fire.
// At the query hook's entry, before the query's own prologue runs, ctx.r31 still holds
// the CALLER's r31 = the camera object. So we CAPTURE it there (g_camObj), and add our
// uncapped mouse delta onto camera+0x40 (YAW) / +0x44 (PITCH) from a per-frame input
// hook (sub_82804AF8) — bypassing both the entry-hook problem and the radial clamp.
// Adding to the persistent angle integral is stable: the camera re-reads +0x40/+0x44
// as the base for its own clamped stick delta each frame, so our addition persists.
// Offsets +0x40/+0x44 and r31=camera are read off 0x8246F9A8's own stfs stores.
std::atomic<uint32_t> g_camObj{ 0 };


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

uint32_t g_lastApplied = 0;   // what the last ApplySplice landed (see SpliceIntact)

// Weave the key bindings into port 0's live binding records. Policy, in order:
// whole line into an unbound record; else the line's first KEY source into a
// free src2 with OR; else skip and say so. Returns how many landed.
uint32_t ApplySplice(uint8_t* base)
{
    const uint32_t array = LoadU32(base, kBindRecords + 4);
    if (!array)
        return 0;
    uint32_t applied = 0, skipped = 0;
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
        ++applied;
    }
    fprintf(stderr, "[kbm] splice: %u of %zu key bindings applied to port 0 "
                    "(%u had no free slot)\n",
            applied, g_splice.size(), skipped);
    g_lastApplied = applied;
    return applied;
}

// Is the splice still standing? The single-sentinel version of this check
// failed the operator in one boot: the first splice fired MID-PARSE (the
// title's own padmap parse was still filling the table, so 91 of 93 lines
// found free slots), the finishing parse overwrote almost everything, and the
// one sentinel record happened to survive — E and ESC died with no re-apply.
// Now the whole applied set is the check: count the records that carry any
// KEY-category source and compare against what the last apply landed. ~600
// byte reads per controller tick — noise.
uint32_t KeySpliceCount(uint8_t* base)
{
    const uint32_t array = LoadU32(base, kBindRecords + 4);
    const uint32_t count = LoadU32(base, kBindRecords + 8);
    if (!array)
        return 0;
    uint32_t keyed = 0;
    for (uint32_t i = 0; i < count && i < kCmdCount; ++i)
    {
        const uint32_t rec = array + i * 24;
        const uint32_t s1 = LoadU32(base, rec + 4);
        const uint32_t s2 = LoadU32(base, rec + 0xC);
        if ((s1 && s1 < kTokenCount && IsKeyToken(base, int(s1))) ||
            (s2 && s2 < kTokenCount && IsKeyToken(base, int(s2))))
            ++keyed;
    }
    return keyed;
}

bool SpliceIntact(uint8_t* base)
{
    if (!g_lastApplied)
        return true;
    return KeySpliceCount(base) * 4 >= g_lastApplied * 3;
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
// How many of port 0's records the title's parse has filled (src1 != 0) —
// the stability gate in the pump watches this settle before splicing.
uint32_t ParsedCount(uint8_t* base, uint32_t /*port*/, uint32_t* outCount)
{
    const uint32_t count = LoadU32(base, kBindRecords + 8);
    const uint32_t array = LoadU32(base, kBindRecords + 4);
    if (outCount)
        *outCount = count;
    uint32_t parsed = 0;
    if (array)
        for (uint32_t i = 0; i < count && i < kCmdCount; ++i)
            parsed += LoadU32(base, array + i * 24 + 4) != 0 ? 1 : 0;
    return parsed;
}

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

    // SECOND ITERATION: the stick/button/camera writes that used to live here
    // raced the title's own conversion (two writers into the RAW source array,
    // whichever landed last at the per-frame publish won — aim on a HELD
    // binding died outright). Everything the conversion owns now arrives
    // through the XInput state (window.cpp's reduced merge: WASD sticks, mouse
    // buttons, the clamped camera), and the camera's unclamped remainder is
    // added AFTER the title's publish (the sub_82804AF8 hook below). Only the
    // KEY sources are fed here, because nothing else writes their cells.

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
    // 256 bytes was NOT enough for 16 ports of growing counters, and
    // snprintf's return value is the WOULD-HAVE-BEEN length: once n passed the
    // buffer, `sizeof line - n` underflowed and the next call smashed the host
    // stack with histogram digits — a guest thread then "crashed" at an ASCII
    // fault address made of this very string (part 92 round 3; the crash that
    // was attributed to the title's keyboard-engagement path bears the same
    // fingerprint, so that attribution is retracted as unproven in
    // docs/native-kbm-phaseA.md).
    char line[640];
    size_t n = snprintf(line, sizeof line, "[kbm] cmd queries by port (bool/float):");
    for (int p = 0; p < 16 && n < sizeof line; ++p)
    {
        const uint64_t b = g_queryByPort[0][p].load(std::memory_order_relaxed);
        const uint64_t f = g_queryByPort[1][p].load(std::memory_order_relaxed);
        if (b || f)
        {
            const int w = snprintf(line + n, sizeof line - n, " p%d:%llu/%llu", p,
                                   (unsigned long long)b, (unsigned long long)f);
            if (w < 0 || size_t(w) >= sizeof line - n)
                break;
            n += size_t(w);
        }
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
    // CAPTURE THE CAMERA OBJECT for the direct-look fix (part 93; see g_camObj). The
    // live camera update (0x8246F9A8, un-hookable at its entry) queries cmd 216
    // (COMMAND_USER_CAM_LEFTRIGHT) and 217 (UPDOWN) from lr 0x8246FA6C / 0x8246FA84.
    // At this hook's entry — before sub_82805510's own prologue — ctx.r31 still holds
    // the caller's r31 = the camera object (the same r31 the camera writes +0x40/+0x44
    // through). Gated to the camera's OWN call sites so the other cmd-216/217 reader
    // (the deadzone checker at 0x821592F0, not an angle-writer) does not overwrite it.
    if ((ctx.r3.u32 == 216 || ctx.r3.u32 == 217) &&
        (uint32_t(ctx.lr) == 0x8246FA6C || uint32_t(ctx.lr) == 0x8246FA84))
    {
        g_camObj.store(ctx.r31.u32, std::memory_order_relaxed);
        if (getenv("CZ_KBM_CAM_TRACE"))
        {
            static uint32_t last = 0;
            if (ctx.r31.u32 != last)
            {
                last = ctx.r31.u32;
                fprintf(stderr, "[camlook] captured camera object %08X (from lr=%08X)\n",
                        ctx.r31.u32, uint32_t(ctx.lr));
            }
        }
    }
    __imp__sub_82805510(ctx, base);
}

// The per-frame source PUBLISH (memcpy of the raw array into the EFFECTIVE
// array the queries read, at this+8). After the title's own copy, the camera's
// unclamped remainder is added straight into the effective right-stick cells —
// past the publish there is no other writer until the next frame, which is
// what makes this race-free where the post-conversion write was not. Sign: the
// effective cells carry the engine's convention (Y negated vs XInput).
namespace
{
// CZ_KBM_TRACE movement-latency probes: log every crossing of |0.5| on the
// left-stick X value at three layers — the ring entry the title consumes, the
// raw source cell, the published (effective) cell — so a held A/D press
// timestamps its way through the pipeline and the lag names its layer.
void ProbeCrossing(const char* what, float v, float* last)
{
    const bool was = *last > 0.5f || *last < -0.5f;
    const bool is = v > 0.5f || v < -0.5f;
    if (was != is)
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        fprintf(stderr, "[kbm] probe %s %s %.2f @%lld ms\n", what,
                is ? "ENGAGED" : "released", v,
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    }
    *last = v;
}
} // namespace

PPC_FUNC(sub_82804AF8)
{
    const uint32_t self = ctx.r3.u32;
    __imp__sub_82804AF8(ctx, base);
    if (!NativeKbm_Active() || !self || self != LoadU32(base, kPortMap))
        return;

    // DIRECT CAMERA-ANGLE LOOK (part 93; the working import of Case West's fix). Add
    // the accumulated raw mouse delta straight onto the captured camera object's yaw
    // (+0x40) / pitch (+0x44), past the engine's radial turn-rate clamp. g_camObj was
    // captured from the camera's own command query (see sub_82805510); this per-frame
    // publish is a reliable point to apply it. Adding to the persistent angle integral
    // is stable — the camera re-reads +0x40/+0x44 as the base for its own clamped stick
    // delta each frame, so the addition persists and is not itself clamped.
    if (MouseDeviceActive() && Settings_MouseCam())
    {
        const uint32_t cam = g_camObj.load(std::memory_order_relaxed);
        if (cam >= 0x10000 && cam < 0xF0000000)
        {
            int dx = 0, dy = 0;
            TakeMouseLook(dx, dy);
            if (dx || dy)
            {
                float scale = 0.00027f;
                if (const char* e = getenv("CZ_KBM_LOOK_SCALE"))
                {
                    const float vv = float(atof(e));
                    if (vv > 0.0f)
                        scale = vv;
                }
                const float k = float(Settings_MouseSens()) * scale;
                float sgx = -1.0f, sgy = 1.0f;
                if (getenv("CZ_KBM_INVERT_X")) sgx = -sgx;
                if (getenv("CZ_KBM_INVERT_Y")) sgy = -sgy;
                const float yaw = LoadF32(base, cam + 0x40);
                const float pit = LoadF32(base, cam + 0x44);
                StoreF32(base, cam + 0x40, yaw + float(dx) * k * sgx);
                StoreF32(base, cam + 0x44, pit + float(dy) * k * sgy);
                if (getenv("CZ_KBM_CAM_TRACE"))
                {
                    static uint64_t n = 0;
                    if ((n++ % 20) == 0)
                        fprintf(stderr, "[camlook] APPLY cam=%08X dx=%d dy=%d k=%.5f "
                                        "yaw %.4f->%.4f\n", cam, dx, dy, k, yaw,
                                yaw + float(dx) * k * sgx);
                }
            }
        }
    }

    if (TraceOn())
    {
        static float lastA = 0.0f;
        ProbeCrossing("A(effective)", LoadF32(base, self + 8 + kSrcLtX * 0x30),
                      &lastA);
    }
    const float sx = AtomicTakeFloat(g_camSurX);
    const float sy = AtomicTakeFloat(g_camSurY);
    if (sx == 0.0f && sy == 0.0f)
        return;
    const uint32_t cellX = self + 8 + kSrcRtX * 0x30;
    const uint32_t cellY = self + 8 + kSrcRtY * 0x30;
    uint32_t bits;
    float v;
    v = LoadF32(base, cellX) + sx;
    memcpy(&bits, &v, 4);
    StoreU32(base, cellX, bits);
    v = LoadF32(base, cellY) - sy;          // engine negates Y vs XInput
    memcpy(&bits, &v, 4);
    StoreU32(base, cellY, bits);
}

// The pad state-to-source conversion: the title's own conversion first, then
// the native overrides for the port-0 controller (see the module comment).
PPC_FUNC(sub_828070E0)
{
    const uint32_t self = ctx.r3.u32;
    const uint32_t ringIdx = ctx.r4.u32;
    __imp__sub_828070E0(ctx, base);
    if (!NativeKbm_Active())
        return;
    if (self && self == LoadU32(base, kPortMap))
    {
        if (TraceOn())
        {
            // the ring entry the title just converted: state at this+0x2498+idx*16,
            // thumbLX at +4 (BE s16)
            static float lastRing = 0.0f, lastB = 0.0f;
            const uint32_t st = self + 0x2498 + (ringIdx & 31) * 16;
            int16_t lx = int16_t((base[st + 4] << 8) | base[st + 5]);
            ProbeCrossing("ring(consumed)", float(lx) / 32767.0f, &lastRing);
            ProbeCrossing("B(raw)", LoadF32(base, self + 0x11D8 + kSrcLtX * 0x30),
                          &lastB);
        }
        if (!SpliceIntact(base))
        {
            fprintf(stderr, "[kbm] splice sentinel lost (padmap re-parsed?) — "
                            "re-applying\n");
            ApplySplice(base);
        }
        PostConversionFeed(ctx, base, self);
    }
}

// ---- DEVICE-FOLLOW PROMPT ART (part 92, the operator's commission) -----------
// The glyph bank is a boot-time choice (the VFS serves the key-chip fecmn.tex
// while the keyboard is the input path), but prompts should follow the LIVE
// device: touch the pad -> Xbox art, touch a key or the mouse -> key chips.
// Mechanism: tools/gen_kbm_icons.py emits glyph_swap.bin carrying BOTH texel
// sets plus each decoded record's 16-byte header (unique per glyph — bytes
// 12..15 differ). A host worker finds every in-memory copy of each record by
// that fingerprint (one scan of the guest arenas, ~hundreds of ms, off the
// guest threads) and on a device change overwrites the texels in place — the
// renderer's per-draw content guard (the part-24 HUD machinery) sees changed
// bytes and re-uploads. Idempotent, self-checking (the fingerprint is
// re-verified before every write; a stale address is dropped for rescan).
namespace
{
enum { DEV_KB = 0, DEV_PAD = 1 };
std::atomic<int> g_wantedDevice{ DEV_KB };

// The direct-camera gate (forward-declared up by the helpers): scale the camera only
// while the KB/mouse is the live device, never a controller.
bool MouseDeviceActive()
{
    return g_wantedDevice.load(std::memory_order_acquire) == DEV_KB;
}
std::atomic<bool> g_deviceWorkerUp{ false };

struct SwapGlyph
{
    std::string name;
    uint8_t fp[16];
    std::vector<uint8_t> padTex, kbTex;
    std::vector<uint32_t> addrs;
};
std::vector<SwapGlyph> g_swapGlyphs;

bool LoadGlyphSwap()
{
    const auto path = HostPaths::Root() / "assets/game_kbm/glyph_swap.bin";
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    char magic[4];
    uint32_t count = 0;
    f.read(magic, 4);
    f.read(reinterpret_cast<char*>(&count), 4);
    if (memcmp(magic, "KBSW", 4) != 0 || count == 0 || count > 64)
        return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t nameLen = 0, texLen = 0;
        f.read(reinterpret_cast<char*>(&nameLen), 4);
        f.read(reinterpret_cast<char*>(&texLen), 4);
        if (!f || nameLen > 64 || texLen > 0x20000)
            return false;
        SwapGlyph g;
        g.name.resize(nameLen);
        f.read(g.name.data(), nameLen);
        f.read(reinterpret_cast<char*>(g.fp), 16);
        g.padTex.resize(texLen);
        f.read(reinterpret_cast<char*>(g.padTex.data()), texLen);
        g.kbTex.resize(texLen);
        f.read(reinterpret_cast<char*>(g.kbTex.data()), texLen);
        if (!f)
            return false;
        g_swapGlyphs.push_back(std::move(g));
    }
    return true;
}

// ---- DEVICE-FOLLOW PROMPT STRINGS (the MASH-on-pad fix) ----------------------
// The kbm overlay's str_en.bcs carries FOUR keyboard wordings (PRESS ENTER twice,
// "A / D KEYS " in the grapple tutorial, and id 4049's MASH struggle label), and
// the bank is a boot-time choice exactly like the glyph bank — so a pad player
// got stick GLYPHS (the texel swap above) under a keyboard LABEL: the operator's
// report was MASH showing on the Xbox prompt. The strings must follow the live
// device the same way the art does.
//
// Mechanism: the loaded bank is the file's own bytes in guest memory (the .bcs
// offset-table format requires the whole file in one buffer), so it is located
// once by its 64-byte prefix (the id-table header — unique, and identical
// across the kbm/patched variants so the locator cannot be wrong-footed by
// which one loaded), confirmed by a 4 KB compare, and each string is swapped in
// place at its file offset. Every write re-verifies the region is one of the
// two known variants first — anything else means the model is wrong and the
// write is refused (the glyph swap's own discipline). The one unequal pair,
// MASH(5) vs "LS \0"(4), swaps inside a 5-byte region whose last byte is NUL in
// both variants, so neither direction can touch the next string.
//
// Honest limitation, unlike the texel swap: a widget copies its TEXT when it is
// built, so a flip updates the NEXT build of a screen/prompt (the next grapple,
// the next title-screen entry), not one already showing. Prompts are built per
// use, so in practice the label matches the device that triggered it.
struct SwapString
{
    const char* what;
    uint32_t off;                    // offset of the region inside the bank file
    std::vector<uint8_t> kb, pad;    // both variants, same region length
};
std::vector<SwapString> g_swapStrings;
std::vector<uint8_t> g_bankFp;       // first 64 bytes: the in-memory locator
std::vector<uint8_t> g_bankHdr;      // first 4096: hit confirmation / stale check
std::vector<uint32_t> g_bankAddrs;

bool LoadStrSwap()
{
    const auto path = HostPaths::Root() / "assets/game_kbm/data/frontend/str_en.bcs";
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return false;
    const std::streamsize sz = f.tellg();
    if (sz < 4096)
        return false;
    std::vector<uint8_t> bank(static_cast<size_t>(sz), 0);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bank.data()), sz);
    if (!f)
        return false;

    // The three same-length wordings, located by searching for the KB bytes —
    // each must occur exactly once (the generator gates the same uniqueness on
    // the original wording; a count != 1 here means the bank moved under us and
    // that string just does not follow, said out loud).
    struct Pair { const char* what; const char* kb; const char* pad; size_t len; };
    const Pair pairs[] = {
        { "title PRESS/ENTER pair", "PRESS\0ENTER\0", "PRESS\0START\0", 12 },
        { "PRESS ENTER",            "PRESS ENTER\0",  "PRESS START\0",  12 },
        { "grapple tutorial",       "A / D KEYS ",    "LEFT STICK ",    11 },
    };
    for (const Pair& p : pairs)
    {
        size_t count = 0;
        const uint8_t* hit = nullptr;
        const uint8_t* from = bank.data();
        size_t left = bank.size();
        while (const uint8_t* h = FindBytes(from, left,
                   reinterpret_cast<const uint8_t*>(p.kb), p.len))
        {
            ++count;
            hit = h;
            left -= size_t(h - from) + 1;
            from = h + 1;
        }
        if (count != 1)
        {
            fprintf(stderr, "[kbm] string-follow: %s occurs %zu times in the "
                            "bank (expected 1) — that string stays as booted\n",
                    p.what, count);
            continue;
        }
        SwapString s;
        s.what = p.what;
        s.off = uint32_t(hit - bank.data());
        s.kb.assign(p.kb, p.kb + p.len);
        s.pad.assign(p.pad, p.pad + p.len);
        g_swapStrings.push_back(std::move(s));
    }

    // Id 4049 (the struggle label) through the bank's own table — exact, not a
    // search. Region is 5 bytes: "MASH\0" <-> "LS \0\0".
    const uint32_t n = *reinterpret_cast<const uint32_t*>(bank.data());
    if (bank.size() >= 4 + 8ull * n)
    {
        const uint32_t* ids = reinterpret_cast<const uint32_t*>(bank.data() + 4);
        const uint32_t* offs = reinterpret_cast<const uint32_t*>(bank.data() + 4 + 4ull * n);
        for (uint32_t k = 0; k < n; ++k)
            if (ids[k] == 4049)
            {
                if (offs[k] + 5 <= bank.size() &&
                    memcmp(bank.data() + offs[k], "MASH\0", 5) == 0)
                {
                    SwapString s;
                    s.what = "struggle label MASH/LS";
                    s.off = offs[k];
                    s.kb.assign({'M', 'A', 'S', 'H', 0});
                    s.pad.assign({'L', 'S', ' ', 0, 0});
                    g_swapStrings.push_back(std::move(s));
                }
                else
                    fprintf(stderr, "[kbm] string-follow: id 4049 is not MASH in "
                                    "the bank — the label stays as booted\n");
                break;
            }
    }
    if (g_swapStrings.empty())
        return false;
    g_bankFp.assign(bank.begin(), bank.begin() + 64);
    g_bankHdr.assign(bank.begin(), bank.begin() + 4096);
    return true;
}

// Locate every in-memory copy of each glyph's TEXELS. Measured live (part 92
// round 4, process_vm_readv over a gameplay run): the decoded glyph textures
// sit PAGE-ALIGNED and HEADERLESS in the physical arena, byte-identical to the
// stored (tiled, swapped) texel payloads — the 05 01 01 E6 records seen
// elsewhere are not them. A glyph is found by a DISCRIMINATING 64-byte slice
// (the first aligned offset where the two art sets differ — corner blocks are
// transparent and identical across glyphs, which burned the first probe), and
// each hit is confirmed by a FULL compare against either art set.
struct Range { uint32_t lo, hi; };
static const Range kScanRanges[] = {
    { 0xA0000000u, 0xBFFF0000u },   // physical arena: textures live here
    { 0x40000000u, 0x7FE00000u },   // large-page virtual
    { 0x00010000u, 0x40000000u },   // small-page virtual
};

// The string bank, unlike the textures, is a file read into a HEAP allocation,
// so it is scanned in every range (the physOnly economy is a texture fact). One
// 64-byte needle over the arenas, each hit confirmed by the 4 KB header.
void ScanForStrBank(uint8_t* base)
{
    if (g_swapStrings.empty())
        return;
    g_bankAddrs.clear();
    for (const Range& r : kScanRanges)
    {
        const uint8_t* p = base + r.lo;
        size_t left = r.hi - r.lo;
        while (left >= g_bankHdr.size())
        {
            const uint8_t* hit = FindBytes(p, left, g_bankFp.data(), g_bankFp.size());
            if (!hit)
                break;
            const size_t remain = size_t(base + r.hi - hit);
            if (remain >= g_bankHdr.size() &&
                memcmp(hit, g_bankHdr.data(), g_bankHdr.size()) == 0)
            {
                g_bankAddrs.push_back(uint32_t(hit - base));
                fprintf(stderr, "[kbm] device-follow scan: string bank at "
                                "%08X\n", uint32_t(hit - base));
            }
            left = remain - 1;
            p = hit + 1;
        }
    }
    if (g_bankAddrs.empty())
        fprintf(stderr, "[kbm] device-follow scan: string bank NOT found — "
                        "prompt wording stays as booted this round\n");
}

void ScanForGlyphs(uint8_t* base, bool physOnly)
{
    size_t found = 0;
    for (SwapGlyph& g : g_swapGlyphs)
    {
        if (!g.addrs.empty())
            continue;
        // the discriminating slice: first 64-aligned offset where the sets differ
        size_t po = 0;
        while (po + 64 <= g.kbTex.size() &&
               memcmp(g.kbTex.data() + po, g.padTex.data() + po, 64) == 0)
            po += 64;
        if (po + 64 > g.kbTex.size())
            continue;                      // sets identical?! nothing to swap
        for (const Range& r : kScanRanges)
        {
            if (physOnly && r.lo != 0xA0000000u)
                continue;      // rescans sweep only where the copies really live
            const uint8_t* lo = base + r.lo;
            const size_t len = r.hi - r.lo;
            for (const uint8_t* probe : { g.kbTex.data() + po, g.padTex.data() + po })
            {
                const uint8_t* p = lo;
                size_t left = len;
                while (left >= 64)
                {
                    const uint8_t* hit = FindBytes(p, left, probe, 64);
                    if (!hit)
                        break;
                    if (size_t(hit - base) >= po)
                    {
                        const uint8_t* texBase = hit - po;
                        if (memcmp(texBase, g.kbTex.data(), g.kbTex.size()) == 0 ||
                            memcmp(texBase, g.padTex.data(), g.padTex.size()) == 0)
                        {
                            const uint32_t addr = uint32_t(texBase - base);
                            bool known = false;
                            for (uint32_t a2 : g.addrs)
                                known |= a2 == addr;
                            if (!known)
                            {
                                g.addrs.push_back(addr);
                                ++found;
                            }
                        }
                    }
                    const size_t adv = size_t(hit - p) + 64;
                    p += adv;
                    left -= adv;
                }
            }
            if (!g.addrs.empty())
                break;                     // physical-arena hit: done for this glyph
        }
    }
    size_t located = 0;
    for (const SwapGlyph& g : g_swapGlyphs)
        located += !g.addrs.empty();
    fprintf(stderr, "[kbm] device-follow scan: %zu of %zu glyphs located "
                    "(%zu copies)\n", located, g_swapGlyphs.size(), found);
}

void DeviceWorker(uint8_t* base)
{
    int applied = -1;                 // force the first apply
    for (;;)
    {
        const int want = g_wantedDevice.load(std::memory_order_acquire);
        if (want == applied)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            continue;
        }
        auto swapAll = [&](size_t& wrote, size_t& strWrote, size_t& stale) {
            for (SwapGlyph& g : g_swapGlyphs)
            {
                auto it = g.addrs.begin();
                while (it != g.addrs.end())
                {
                    // still one of the two art sets? anything else means the
                    // allocation was reused — drop it for rescan.
                    if (memcmp(base + *it, g.kbTex.data(), g.kbTex.size()) != 0 &&
                        memcmp(base + *it, g.padTex.data(), g.padTex.size()) != 0)
                    {
                        it = g.addrs.erase(it);
                        ++stale;
                        continue;
                    }
                    const auto& tex = want == DEV_PAD ? g.padTex : g.kbTex;
                    memcpy(base + *it, tex.data(), tex.size());
                    ++wrote;
                    ++it;
                }
            }
            // The prompt STRINGS follow the same flip. Stale check is the 4 KB
            // header (an allocation reuse cannot keep it), and every region is
            // verified to be one of the two known variants before the write.
            auto ba = g_bankAddrs.begin();
            while (ba != g_bankAddrs.end())
            {
                if (memcmp(base + *ba, g_bankHdr.data(), g_bankHdr.size()) != 0)
                {
                    ba = g_bankAddrs.erase(ba);
                    ++stale;
                    continue;
                }
                for (const SwapString& s : g_swapStrings)
                {
                    uint8_t* region = base + *ba + s.off;
                    if (memcmp(region, s.kb.data(), s.kb.size()) != 0 &&
                        memcmp(region, s.pad.data(), s.pad.size()) != 0)
                        continue;      // not our model — refuse the write
                    const auto& text = want == DEV_PAD ? s.pad : s.kb;
                    memcpy(region, text.data(), text.size());
                    ++strWrote;
                }
                ++ba;
            }
        };
        // Swap the KNOWN copies first — the flip must be instant. Rescans for
        // unlocated glyphs run AFTER, rate-limited to one per 20 s and to the
        // physical arena past the first pass: the first version swept multiple
        // gigabytes on EVERY flip whenever one glyph stayed unlocated, and
        // alternating devices in play turned that into a constant memory storm
        // — the operator's sub-30-fps report.
        size_t wrote = 0, strWrote = 0, stale = 0;
        swapAll(wrote, strWrote, stale);
        bool anyMissing = !g_swapStrings.empty() && g_bankAddrs.empty();
        for (const SwapGlyph& g : g_swapGlyphs)
            if (g.addrs.empty())
            {
                anyMissing = true;
                break;
            }
        static auto lastScan = std::chrono::steady_clock::time_point{};
        static bool scannedOnce = false;
        const auto now = std::chrono::steady_clock::now();
        if ((anyMissing || stale) &&
            (lastScan == std::chrono::steady_clock::time_point{} ||
             now - lastScan > std::chrono::seconds(20)))
        {
            ScanForGlyphs(base, scannedOnce);   // full sweep once, then physical-only
            if (g_bankAddrs.empty())
                ScanForStrBank(base);           // the bank is heap-resident: all ranges
            scannedOnce = true;
            lastScan = std::chrono::steady_clock::now();
            swapAll(wrote, strWrote, stale);    // newly-found copies get the art now
        }
        applied = want;
        fprintf(stderr, "[kbm] prompt art -> %s (%zu texel copies, %zu strings "
                        "swapped%s)\n",
                want == DEV_PAD ? "PAD" : "KEYBOARD", wrote, strWrote,
                stale ? ", stale addresses dropped" : "");
    }
}
} // namespace

void NativeKbm_NoteDeviceInput(bool pad)
{
    if (!NativeKbm_Active())
        return;
    static const bool promptsOff = getenv("CZ_NO_KB_PROMPTS") != nullptr;
    if (promptsOff)
        return;
    const int want = pad ? DEV_PAD : DEV_KB;
    if (g_wantedDevice.exchange(want, std::memory_order_release) != want ||
        !g_deviceWorkerUp.load(std::memory_order_acquire))
    {
        if (!g_deviceWorkerUp.exchange(true, std::memory_order_acq_rel))
        {
            if (!LoadGlyphSwap())
            {
                fprintf(stderr, "[kbm] glyph_swap.bin missing/bad — prompt art "
                                "stays as booted\n");
                return;
            }
            if (!LoadStrSwap())
                fprintf(stderr, "[kbm] string-follow: kbm str_en.bcs missing/bad "
                                "— prompt WORDING stays as booted (art still "
                                "follows)\n");
            std::thread(DeviceWorker, g_memory.base).detach();
        }
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
    // The title's parse must be FINISHED, not merely started: require a
    // substantially-filled table whose parsed count has been STABLE across
    // ~120 pump calls (about two seconds) — the mid-parse splice above is what
    // this prevents.
    {
        static uint32_t lastCount = 0, stablePolls = 0;
        uint32_t cnt = 0;
        const uint32_t parsed = ParsedCount(base, 0, &cnt);
        if (parsed < 150 || parsed != lastCount)
        {
            lastCount = parsed;
            stablePolls = 0;
            return;
        }
        if (++stablePolls < 120)
            return;
    }
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

void NativeKbm_PanelKeyLevel(uint16_t vk, bool down)
{
    // Pad-button LEVELS for the guest Visuals panel pump, tracked for EVERY
    // key event regardless of the overlay/focus gates — the first version
    // lived inside PushKey, whose gating suppressed the release of the very
    // ENTER that opened the panel: the A bit stuck on and the selected row
    // stepped by itself (the operator's report).
    uint32_t bit = 0;
    switch (vk)
    {
        case 0x26: bit = 0x0001; break;   // up    -> XI_DPAD_UP
        case 0x28: bit = 0x0002; break;   // down  -> XI_DPAD_DOWN
        case 0x25: bit = 0x0004; break;   // left  -> XI_DPAD_LEFT
        case 0x27: bit = 0x0008; break;   // right -> XI_DPAD_RIGHT
        case 0x0D: bit = 0x1000; break;   // enter -> XI_A
        case 0x1B: bit = 0x2000; break;   // esc   -> XI_B
        case 0x58: bit = 0x4000; break;   // X     -> XI_X (the panel's save+apply)
        default: return;
    }
    if (down)
        g_panelButtons.fetch_or(bit, std::memory_order_relaxed);
    else
        g_panelButtons.fetch_and(~bit, std::memory_order_relaxed);
}

void NativeKbm_PushKey(uint16_t vk, uint16_t unicode, bool down, bool repeat,
                       uint16_t mods)
{
    if (!NativeKbm_Enabled())
        return;
    if (down)
        NativeKbm_NoteDeviceInput(false);      // device-follow: a key names KB
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

uint32_t NativeKbm_PanelButtons()
{
    return NativeKbm_Active() ? g_panelButtons.load(std::memory_order_relaxed) : 0;
}

void NativeKbm_CameraSurplus(float sx, float sy)
{
    if (sx != 0.0f)
        AtomicAddFloat(g_camSurX, sx);
    if (sy != 0.0f)
        AtomicAddFloat(g_camSurY, sy);
}

// Raw per-poll mouse deltas for the DIRECT camera-look path (sub_824676C0 drains
// them once a frame and adds them straight onto the camera's yaw/pitch angles,
// bypassing the engine's radial turn-rate clamp — see that hook). Imported from
// Case West, part 93.
void NativeKbm_AddMouseLook(int dx, int dy)
{
    if (dx)
        g_lookDX.fetch_add(dx, std::memory_order_relaxed);
    if (dy)
        g_lookDY.fetch_add(dy, std::memory_order_relaxed);
}
