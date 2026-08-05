#include "pm4.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// --- guest memory ---------------------------------------------------------------
// Deliberately not ppc_context.h's PPC_LOAD/STORE macros, which need a `base` named
// exactly that in scope: keeping the accessors local is what lets this translation
// unit be reused by an offline replay harness without dragging in the recompiled
// image or SIMDe. The semantics are identical — guest memory is big-endian.
inline uint32_t GuestLoad32(const uint8_t* base, uint32_t va)
{
    uint32_t raw;
    memcpy(&raw, base + va, 4);
    return __builtin_bswap32(raw);
}

inline void GuestStore32(uint8_t* base, uint32_t va, uint32_t value)
{
    const uint32_t raw = __builtin_bswap32(value);
    memcpy(base + va, &raw, 4);
}

// Command-stream addresses are physical. Our physical arena is the cached view at
// 0xA0000000 (kernel/heap.cpp), and this is the exact inverse of what our
// MmGetPhysicalAddress does (`address & 0x1FFFFFFF`), which is what makes the round
// trip self-consistent.
//
// A caveat that cost real time to establish, and that matters the moment anyone
// compares our physical addresses against a capture's: **Xenia's physical addresses
// are NOT `virtual & 0x1FFFFFFF`.** A1 shows `MmGetPhysicalAddress(E3D71000)`
// answering `03D72000` — a constant +0x1000 skew — and the ring-buffer geometry only
// makes sense with it. Under the naive mask the ring appears to start 0x1000 inside
// its own allocation and overrun the end by the same amount; with the skew it starts
// exactly at the allocation base and fits exactly. Our convention is internally
// consistent and needs no skew, but a physical address in our log is 0x1000 below
// the same object's address in a capture.
constexpr uint32_t kPhysArenaBase = 0xA0000000u;
constexpr uint32_t kPhysArenaEnd = 0xBFFF0000u;

inline uint32_t PhysToVa(uint32_t addr)
{
    return kPhysArenaBase | (addr & 0x1FFFFFFFu);
}

// The GPU's per-address endian swizzle, carried in the low 2 bits of every address
// dword a packet holds: 0 = none, 1 = 8-in-16, 2 = 8-in-32, 3 = 16-in-32.
inline uint32_t GpuSwap(uint32_t v, uint32_t endian)
{
    switch (endian & 3)
    {
        default:
        case 0: return v;
        case 1: return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
        case 2: return __builtin_bswap32(v);
        case 3: return (v >> 16) | (v << 16);
    }
}

// ...and the swap that is left to APPLY, which is not the same thing, because
// GuestLoad32/GuestStore32 already perform an 8-in-32 of their own (that is what
// "guest memory is big-endian" means on a little-endian host). The code in the
// packet describes the whole conversion from the GPU's native little-endian dword to
// what the PowerPC will read; our accessors have already done the 8-in-32 half of
// it. So the residual swap is the code composed with 8-in-32 — which makes code 2,
// the code these fences use, the IDENTITY.
//
// Getting this wrong is silent and looks like something else entirely. Asura's Wrath
// stored its fences with a full GpuSwap on top of the accessor's own, and the
// symptom was not "wrong endianness": it was a retired-fence counter reading
// 0B000000 instead of 0000000B and three WAIT_REG_MEM packets missing references
// they were each exactly one byte-swap away from. Every one of those reads as a
// plausible protocol problem rather than a swap.
inline uint32_t GpuSwapResidual(uint32_t v, uint32_t endian)
{
    return GpuSwap(v, (endian & 3) ^ 2);
}

// --- state ------------------------------------------------------------------------
uint32_t g_ringBase = 0;   // guest virtual address of the ring
uint32_t g_ringDwords = 0; // ring size in dwords
uint32_t g_cursor = 0;     // our read pointer, ring-relative, in dwords

// CZ_PM4_STOP_ON_WAIT — see the WAIT_REG_MEM case. Read once; an env lookup per
// packet would be measurable at 1.27 M packets per 25 s.
const bool g_stopOnWait = getenv("CZ_PM4_STOP_ON_WAIT") != nullptr;

// CZ_PM4_ZERO_IS_NOP — the rejected reading of a zero header, kept as a measurable
// arm rather than deleted; see ExecutePacket.
const bool g_zeroIsNop = getenv("CZ_PM4_ZERO_IS_NOP") != nullptr;

// The register file, covering the real registers plus the SET_CONSTANT windows
// (ALU constants at 0x4000, fetch at 0x4800, bools at 0x4900, loops at 0x4908).
constexpr uint32_t kRegCount = 0x8000;
uint32_t g_regs[kRegCount];

// Registers with side effects (indices from Xenia's register_table.inc).
constexpr uint32_t kRegScratchUmsk = 0x01DC;     // writeback enable, one bit per scratch reg
constexpr uint32_t kRegScratchAddr = 0x01DD;     // writeback base, physical
constexpr uint32_t kRegScratch0 = 0x0578;
constexpr uint32_t kRegScratch7 = 0x057F;
constexpr uint32_t kRegCoherStatusHost = 0x0A31; // bit 31 = coherency action pending

// Predicated binning. SET_BIN_MASK_* tags the following packets with the EDRAM bins
// they touch and SET_BIN_SELECT_* picks the bin being rendered; the hardware ME runs
// a packet whose header bit 0 is set only when (mask & select) != 0. Both default to
// all-ones, i.e. no binning.
//
// Case Zero leans on this harder than the previous port did: SET_BIN_MASK_LO is its
// single most frequent type-3 opcode (2,353,460 of 8,283,322 in B1, over a quarter
// of all type-3 traffic). Getting the predication wrong here would silently drop or
// silently execute a large fraction of the stream.
uint64_t g_binMask = ~0ull;
uint64_t g_binSelect = ~0ull;

// --- census -----------------------------------------------------------------------
std::atomic<uint64_t> g_packets{ 0 };
std::atomic<uint64_t> g_types[4];
std::atomic<uint64_t> g_opcodes[128];
std::atomic<uint64_t> g_draws{ 0 };
std::atomic<uint64_t> g_frames{ 0 };
std::atomic<uint64_t> g_interrupts{ 0 };
void (*g_interruptSink)() = nullptr;

// The full Xenos type-3 table. A name here means "this command processor has such an
// opcode"; nullptr means it does not, which is load-bearing: the ring resync will
// only accept a named opcode as a plausible packet header, and an unnamed opcode
// appearing in our own stream is a reportable anomaly rather than something to skip.
//
// Written as explicit index assignments rather than a positional initialiser list on
// purpose: the positional form has 128 slots with 46 names scattered through it, and
// a single miscounted nullptr shifts every later opcode by one — which shows up not
// as a compile error but as a plausible-looking census with the wrong names on it.
// That is exactly the error that put SET_BIN_MASK at 0x39 and INTERRUPT at 0x40 in
// `tools/xtr_pm4_census.py`'s first draft.
struct OpcodeNames
{
    const char* name[128] = {};

    OpcodeNames()
    {
        auto set = [&](uint32_t op, const char* n) { name[op] = n; };
        set(0x10, "NOP");
        set(0x21, "REG_RMW");
        set(0x22, "DRAW_INDX");
        set(0x23, "VIZ_QUERY");
        set(0x25, "SET_STATE");
        set(0x26, "WAIT_FOR_IDLE");
        set(0x27, "IM_LOAD");
        set(0x2B, "IM_LOAD_IMMEDIATE");
        set(0x2C, "IM_STORE");
        set(0x2D, "SET_CONSTANT");
        set(0x2E, "LOAD_CONSTANT_CONTEXT");
        set(0x2F, "LOAD_ALU_CONSTANT");
        set(0x34, "DRAW_INDX_BIN");
        set(0x35, "DRAW_INDX_2_BIN");
        set(0x36, "DRAW_INDX_2");
        set(0x37, "INDIRECT_BUFFER_PFD");
        set(0x3B, "INVALIDATE_STATE");
        set(0x3C, "WAIT_REG_MEM");
        set(0x3D, "MEM_WRITE");
        set(0x3E, "REG_TO_MEM");
        set(0x3F, "INDIRECT_BUFFER");
        set(0x44, "COND_EXEC");
        set(0x45, "COND_WRITE");
        set(0x46, "EVENT_WRITE");
        set(0x48, "ME_INIT");
        set(0x4A, "SET_SHADER_BASES");
        set(0x4B, "SET_BIN_BASE_OFFSET");
        set(0x4F, "MEM_WRITE_CNTR");
        set(0x50, "SET_BIN_MASK");
        set(0x51, "SET_BIN_SELECT");
        set(0x52, "WAIT_REG_EQ");
        set(0x53, "WAIT_REG_GTE");
        set(0x54, "INTERRUPT");
        set(0x55, "SET_CONSTANT2");
        set(0x56, "SET_SHADER_CONSTANTS");
        set(0x58, "EVENT_WRITE_SHD");
        set(0x59, "EVENT_WRITE_CFL");
        set(0x5A, "EVENT_WRITE_EXT");
        set(0x5B, "EVENT_WRITE_ZPD");
        set(0x5C, "WAIT_UNTIL_READ");
        set(0x5D, "WAIT_IB_PFD_COMPLETE");
        set(0x5E, "CONTEXT_UPDATE");
        set(0x60, "SET_BIN_MASK_LO");
        set(0x61, "SET_BIN_MASK_HI");
        set(0x62, "SET_BIN_SELECT_LO");
        set(0x63, "SET_BIN_SELECT_HI");
        set(0x64, "XE_SWAP");
    }
};
const OpcodeNames kOpcodeNames;

// --- packet source ----------------------------------------------------------------
// One indirection covering both ways a packet stream reaches us: the ring (a wrapping
// window into guest memory) and an indirect buffer (a linear one). A struct rather
// than std::function because the walk calls it once per dword.
struct Source
{
    const uint8_t* base = nullptr;
    uint32_t va = 0;
    uint32_t wrapDwords = 0; // != 0 for the ring

    uint32_t operator()(uint32_t i) const
    {
        const uint32_t index = wrapDwords ? (i % wrapDwords) : i;
        return GuestLoad32(base, va + index * 4);
    }
};

void ExecuteLinear(uint8_t* base, uint32_t va, uint32_t sizeDwords, int depth);

// --- memory-writing helpers -------------------------------------------------------
// Refuse anything that does not land in the physical arena rather than storing
// through it: a malformed or half-written packet parsed as a fence would otherwise
// scribble an arbitrary guest address, and the resulting corruption surfaces nowhere
// near here.
bool StoreGpuRaw(uint8_t* base, uint32_t physAddr, uint32_t value)
{
    const uint32_t va = PhysToVa(physAddr & ~3u);
    if (va < kPhysArenaBase || va + 4 > kPhysArenaEnd)
    {
        static std::atomic<uint64_t> dropped{ 0 };
        if (dropped.fetch_add(1) < 8)
            fprintf(stderr,
                    "[pm4] GPU store outside the physical arena DROPPED "
                    "(phys=%08X -> va=%08X value=%08X)\n",
                    physAddr, va, value);
        return false;
    }
    GuestStore32(base, va, value);
    return true;
}

// A packet's memory write: `addrDword` is `physical | endian code`.
void StoreGpu(uint8_t* base, uint32_t addrDword, uint32_t value, uint32_t indexDwords = 0)
{
    StoreGpuRaw(base, (addrDword & ~3u) + indexDwords * 4, GpuSwapResidual(value, addrDword));
}

uint32_t LoadGpu(const uint8_t* base, uint32_t addrDword)
{
    const uint32_t va = PhysToVa(addrDword & ~3u);
    if (va < kPhysArenaBase || va + 4 > kPhysArenaEnd)
        return 0;
    return GpuSwapResidual(GuestLoad32(base, va), addrDword);
}

// --- register writes --------------------------------------------------------------
void WriteRegister(uint8_t* base, uint32_t index, uint32_t value)
{
    if (index >= kRegCount)
        return;
    g_regs[index] = value;

    // Scratch-register writeback: when SCRATCH_UMSK enables a scratch register, each
    // write to it is mirrored to SCRATCH_ADDR + reg*4. This is a real reporting
    // channel the guest reads, and it costs us nothing to honour because the values
    // come from the stream itself.
    //
    // StoreGpuRaw, NOT StoreGpu: SCRATCH_ADDR is a bare physical base with no endian
    // code in it, and running it through the packet-address path reads its low bits
    // as one. On the previous port that mistake byte-swapped every mirrored register,
    // and what it looked like was not an endian bug — it was WAIT_REG_MEM packets in
    // its own stream failing forever, because those packets poll this very block and
    // their reference values are the scratch registers' own contents.
    if (index >= kRegScratch0 && index <= kRegScratch7)
    {
        const uint32_t reg = index - kRegScratch0;
        if (g_regs[kRegScratchUmsk] & (1u << reg))
            StoreGpuRaw(base, g_regs[kRegScratchAddr] + reg * 4, value);
    }
}

bool EvalWaitCondition(uint32_t func, uint32_t value, uint32_t mask, uint32_t ref)
{
    const uint32_t v = value & mask;
    switch (func & 7)
    {
        case 0: return false; // never
        case 1: return v < ref;
        case 2: return v <= ref;
        case 3: return v == ref;
        case 4: return v != ref;
        case 5: return v >= ref;
        case 6: return v > ref;
        default: return true; // always
    }
}

// Execute one packet at `pos`. Returns the dwords consumed, or 0 when the packet
// claims more than `avail` — which at ring level means the driver has written the
// header but not yet the body, and the right answer is to come back next tick rather
// than to guess.
uint32_t ExecutePacket(uint8_t* base, const Source& fetch, uint32_t pos, uint32_t avail,
                       int depth)
{
    const uint32_t header = fetch(pos);
    const uint32_t type = header >> 30;
    g_packets.fetch_add(1, std::memory_order_relaxed);
    g_types[type].fetch_add(1, std::memory_order_relaxed);

    if (type == 2) // filler
        return 1;

    // An all-zero dword at ring level is memory the driver has not written yet.
    // Parsing it as a type-0 "write one register at index 0" would silently consume
    // two dwords of a packet still being written and desync the stream.
    if (header == 0 && depth == 0)
    {
        g_packets.fetch_sub(1, std::memory_order_relaxed);
        g_types[type].fetch_sub(1, std::memory_order_relaxed);
        return 0;
    }

    // Below the ring, a zero dword falls through to the type-0 path and is read as
    // "write one register at index 0", consuming the dword after it as data.
    //
    // That looks wrong, and finding 38 spent a while believing it was: it would mean
    // every odd-length run of padding shifts the walk by one dword, which is exactly
    // the symptom the indirect-buffer truncations show. It is not wrong. Xenia's B1
    // capture records the true length of all 24,527,474 packets it executed, and
    // `tools/pm4_packet_lengths.py` says our arithmetic matches every one of them —
    // including its single zero-header packet, which hardware consumed as TWO dwords,
    // not one. B1 containing exactly one such packet in 24.5 M is the other half of
    // the story: a correctly aligned walk of this title's streams essentially never
    // meets a zero header, so the zeros our truncated walks trip over are data being
    // read by a walk that is already lost.
    //
    // CZ_PM4_ZERO_IS_NOP=1 selects the rejected reading, so the question can be
    // re-measured without a rebuild (gotcha 50 — a rate needs two arms of the same
    // binary). Measured at 10 runs a side: no difference in the stall rate, which is
    // what the capture predicts for a rule that only fires after the walk is lost.
    if (header == 0 && g_zeroIsNop)
        return 1;

    if (type == 1) // two register writes, 11-bit indices
    {
        if (avail < 3)
            return 0;
        WriteRegister(base, header & 0x7FF, fetch(pos + 1));
        WriteRegister(base, (header >> 11) & 0x7FF, fetch(pos + 2));
        return 3;
    }

    const uint32_t bodyCount = ((header >> 16) & 0x3FFF) + 1;
    if (avail < bodyCount + 1)
        return 0;

    if (type == 0) // a run of register writes
    {
        const uint32_t reg = header & 0x7FFF;
        const bool oneReg = (header >> 15) & 1;
        for (uint32_t i = 0; i < bodyCount; i++)
            WriteRegister(base, oneReg ? reg : reg + i, fetch(pos + 1 + i));
        return bodyCount + 1;
    }

    // type 3
    const uint32_t opcode = (header >> 8) & 0x7F;
    g_opcodes[opcode].fetch_add(1, std::memory_order_relaxed);
    if (opcode == 0x22 || opcode == 0x36)
        g_draws.fetch_add(1, std::memory_order_relaxed);

    // An opcode this command processor does not have is reported, once each. B1's
    // census says there are exactly 21 and all are named, so any of these is either a
    // parser desync or a packet the captures never contained — both worth knowing
    // about, and neither worth guessing at.
    if (!kOpcodeNames.name[opcode])
    {
        static std::atomic<uint64_t> seen[128];
        if (seen[opcode].fetch_add(1) == 0)
            fprintf(stderr, "[pm4] UNKNOWN type-3 opcode 0x%02X (header %08X, %u body "
                            "dwords) at dword %u — B1's census says this title uses 21 "
                            "opcodes and this is not one of them\n",
                    opcode, header, bodyCount, pos);
    }

    // ME predication: header bit 0 marks a packet the hardware runs only when the
    // current bin mask and bin select overlap. Skipping the body wholesale is what
    // the ME does, for every type-3 opcode alike.
    if ((header & 1) && (g_binMask & g_binSelect) == 0)
        return bodyCount + 1;

    auto body = [&](uint32_t i) { return fetch(pos + 1 + i); };

    switch (opcode)
    {
        case 0x10: // NOP
        case 0x23: // VIZ_QUERY
        case 0x26: // WAIT_FOR_IDLE — instantly true; we execute synchronously
        case 0x3B: // INVALIDATE_STATE
        case 0x48: // ME_INIT
        case 0x5E: // CONTEXT_UPDATE
            break;

        // Draws are counted above and otherwise inert in this phase.
        case 0x22: // DRAW_INDX:   dword0 = viz query info, dword1 = VGT_DRAW_INITIATOR
        case 0x36: // DRAW_INDX_2: dword0 = VGT_DRAW_INITIATOR
            break;

        case 0x60: g_binMask = (g_binMask & 0xFFFFFFFF00000000ull) | body(0); break;
        case 0x61: g_binMask = (g_binMask & 0xFFFFFFFFull) | (uint64_t(body(0)) << 32); break;
        case 0x62: g_binSelect = (g_binSelect & 0xFFFFFFFF00000000ull) | body(0); break;
        case 0x63: g_binSelect = (g_binSelect & 0xFFFFFFFFull) | (uint64_t(body(0)) << 32); break;

        case 0x3D: // MEM_WRITE: addr(+endian), then the dwords to write
            for (uint32_t i = 1; i < bodyCount; i++)
                StoreGpu(base, body(0), body(i), i - 1);
            break;

        case 0x3E: // REG_TO_MEM: reg, addr(+endian)
            if (bodyCount >= 2)
            {
                const uint32_t reg = body(0) & 0x7FFF;
                StoreGpu(base, body(1), reg < kRegCount ? g_regs[reg] : 0);
            }
            break;

        case 0x21: // REG_RMW: rmw_info, and_mask, or_mask (info bits 31/30: masks are regs)
            if (bodyCount >= 3)
            {
                const uint32_t info = body(0), andM = body(1), orM = body(2);
                const uint32_t dst = info & 0x1FFF;
                uint32_t v = dst < kRegCount ? g_regs[dst] : 0;
                v &= ((info >> 31) & 1) ? g_regs[andM & 0x1FFF] : andM;
                v |= ((info >> 30) & 1) ? g_regs[orM & 0x1FFF] : orM;
                WriteRegister(base, dst, v);
            }
            break;

        // THE FENCES. This is the packet family the whole module turns on: the
        // driver's ring-progress waits poll words that nothing but these ever write,
        // and every value comes out of the stream rather than out of us. Case Zero
        // issues 2.35 million of them across B1 (EVENT_WRITE 1,203,473 +
        // EVENT_WRITE_EXT 1,141,008 + EVENT_WRITE_SHD 8,026).
        //
        // Fable 2 suppressed the bit-31 form of these. That is exactly the kind of
        // thing not to inherit: it was a workaround for its fences landing in an
        // o1heap-managed physical arena, which this runtime deliberately does not use
        // for physical memory (kernel/heap.h explains why).
        case 0x46: // EVENT_WRITE
        case 0x58: // EVENT_WRITE_SHD  (VS/PS-done fence)
        case 0x59: // EVENT_WRITE_CFL  (cache-flush-done fence)
        case 0x5A: // EVENT_WRITE_EXT
            WriteRegister(base, 0x21F9 /* VGT_EVENT_INITIATOR */, body(0) & 0x3F);
            if (bodyCount >= 3)
                StoreGpu(base, body(1), body(2));
            break;

        case 0x3C: // WAIT_REG_MEM: wait_info, addr/reg(+endian), ref, mask, [interval]
        {
            if (bodyCount < 4)
                break;
            const uint32_t info = body(0), poll = body(1), ref = body(2), mask = body(3);
            const bool isMemory = (info & 0x10) != 0;
            // Evaluated once, not spun on. Everything this executor can satisfy it
            // has already satisfied by the time it gets here, because we execute the
            // stream strictly in order and synchronously; and a wait that only a
            // guest thread can satisfy must NOT be blocked on, because that guest
            // thread is very likely the one waiting for us to make progress.
            uint32_t value;
            if (isMemory)
            {
                value = LoadGpu(base, poll);
            }
            else
            {
                const uint32_t reg = poll & 0x7FFF;
                // Coherency requests complete instantly on a GPU with no caches: the
                // driver sets bit 31 (pending) and waits for it to clear.
                if (reg == kRegCoherStatusHost)
                    g_regs[reg] &= ~0x80000000u;
                value = reg < kRegCount ? g_regs[reg] : 0;
            }
            if (!EvalWaitCondition(info, value, mask, ref))
            {
                static std::atomic<uint64_t> unmet{ 0 };
                const uint64_t n = unmet.fetch_add(1) + 1;
                if (n <= 8 || (n & 0xFFFF) == 0)
                    fprintf(stderr,
                            "[pm4] WAIT_REG_MEM #%llu not satisfied, %s: %s %08X "
                            "value=%08X mask=%08X ref=%08X func=%u\n",
                            (unsigned long long)n, g_stopOnWait ? "STOPPING" : "continuing",
                            isMemory ? "mem" : "reg", poll, value, mask, ref, info & 7);

                // CZ_PM4_STOP_ON_WAIT=1 — the more faithful behaviour, offered as an
                // experiment rather than the default because it can regress.
                //
                // Real hardware STALLS the command processor here until the condition
                // holds; we evaluate once and carry on, which is how our CP gets
                // ahead of the CPU and finds the scratch mirror poisoned at a later
                // INTERRUPT (see MirrorIsPoisoned in gpu/vd.cpp). Stopping the ring
                // walk at this packet — not spinning inside it, which would deadlock
                // against the very guest thread that has to satisfy the wait, but
                // returning and retrying next tick — is what the console does.
                //
                // The risk, and the reason it is not the default: if the condition is
                // only ever satisfied by work that appears LATER in the same stream,
                // the ring stalls permanently. That failure is at least loud —
                // Pm4_Execute reports a frozen cursor after 60 ticks — but it would
                // regress a gate that currently reaches 56 of 93. Measure both arms
                // before promoting it.
                if (g_stopOnWait && depth == 0)
                    return 0;
            }
            break;
        }

        case 0x45: // COND_WRITE: wait_info, poll addr/reg, ref, mask, write addr/reg, data
        {
            if (bodyCount < 6)
                break;
            const uint32_t info = body(0), poll = body(1), ref = body(2), mask = body(3);
            const uint32_t value =
                (info & 0x10) ? LoadGpu(base, poll)
                              : ((poll & 0x7FFF) < kRegCount ? g_regs[poll & 0x7FFF] : 0);
            if (EvalWaitCondition(info, value, mask, ref))
            {
                if (info & 0x100)
                    StoreGpu(base, body(4), body(5));
                else
                    WriteRegister(base, body(4) & 0x7FFF, body(5));
            }
            break;
        }

        case 0x54: // INTERRUPT: delivered HERE rather than after the walk — see pm4.h.
            g_interrupts.fetch_add(1, std::memory_order_relaxed);
            if (g_interruptSink)
                g_interruptSink();
            break;

        case 0x64: // XE_SWAP: the frame boundary, exactly one per frame in B1
                   // (1,089 swaps over 1,089 frames), and the runtime's only frame
                   // clock — VdSwap is kHighFrequency and appears in no kernel log.
            g_frames.fetch_add(1, std::memory_order_relaxed);
            break;

        case 0x2D: // SET_CONSTANT: offset_type, then data
        {
            uint32_t index = body(0) & 0x7FF;
            switch ((body(0) >> 16) & 0xFF)
            {
                case 0: index += 0x4000; break; // ALU constants
                case 1: index += 0x4800; break; // fetch constants
                case 2: index += 0x4900; break; // booleans
                case 3: index += 0x4908; break; // loops
                case 4: index += 0x2000; break; // registers
                default: index = kRegCount;     // unknown bank: drop
            }
            for (uint32_t i = 1; i < bodyCount; i++)
                WriteRegister(base, index + i - 1, body(i));
            break;
        }

        case 0x55: // SET_CONSTANT2 / SET_SHADER_CONSTANTS: absolute index in body(0)
        case 0x56:
            for (uint32_t i = 1; i < bodyCount; i++)
                WriteRegister(base, (body(0) & 0xFFFF) + i - 1, body(i));
            break;

        case 0x2F: // LOAD_ALU_CONSTANT: addr, offset_type, size — constants from memory
        {
            if (bodyCount < 3)
                break;
            uint32_t index = body(1) & 0x7FF;
            switch ((body(1) >> 16) & 0xFF)
            {
                case 0: index += 0x4000; break;
                case 1: index += 0x4800; break;
                case 2: index += 0x4900; break;
                case 3: index += 0x4908; break;
                case 4: index += 0x2000; break;
                default: index = kRegCount;
            }
            const uint32_t addr = PhysToVa(body(0) & 0x3FFFFFFC);
            const uint32_t size = body(2) & 0xFFF;
            for (uint32_t i = 0; i < size; i++)
                WriteRegister(base, index + i, GuestLoad32(base, addr + i * 4));
            break;
        }

        // Shader uploads. The renderer phase is where these become SPIR-V; here they
        // only have to not be mis-parsed — and IM_LOAD_IMMEDIATE in particular
        // carries its microcode inline, so getting its length wrong desyncs the whole
        // stream. Case Zero issues 836,994 IM_LOAD and 204,151 IM_LOAD_IMMEDIATE in
        // B1, so a length bug here would be immediate and total.
        case 0x27: // IM_LOAD: addr_type, start_size
        case 0x2B: // IM_LOAD_IMMEDIATE: type, start_size, then the code
            break;

        case 0x37: // INDIRECT_BUFFER_PFD
        case 0x3F: // INDIRECT_BUFFER: physical address, size in dwords
            if (bodyCount >= 2 && depth < 8)
            {
                const uint32_t addr = body(0);
                const uint32_t size = body(1) & 0xFFFFF;
                if (getenv("CZ_PM4_IB_TRACE"))
                {
                    static std::atomic<uint64_t> n{ 0 };
                    if (n.fetch_add(1) < 64)
                        fprintf(stderr,
                                "[pm4] INDIRECT_BUFFER header=%08X body0=%08X body1=%08X "
                                "-> addr=%08X size=%u (bodyCount=%u)\n",
                                header, body(0), body(1), addr, size, bodyCount);
                }
                if (size && (addr & 0x1FFFFFFFu) + uint64_t(size) * 4 <= 0x20000000ull)
                {
                    ExecuteLinear(base, PhysToVa(addr), size, depth + 1);
                }
                else
                {
                    // Same reasoning as the truncation report above: a buffer we
                    // decline to walk takes its trailing fence with it.
                    static std::atomic<uint64_t> skipped{ 0 };
                    const uint64_t n = skipped.fetch_add(1);
                    if (n < 16 || (n & 0xFFFu) == 0)
                        fprintf(stderr,
                                "[pm4] indirect buffer SKIPPED (addr=%08X size=%u, "
                                "occurrence %llu)\n",
                                addr, size, static_cast<unsigned long long>(n));
                }
            }
            break;

        default: // counted in the census above; the body is skipped safely
            break;
    }

    return bodyCount + 1;
}

void ExecuteLinear(uint8_t* base, uint32_t va, uint32_t sizeDwords, int depth)
{
    Source fetch{ base, va, 0 };
    uint32_t pos = 0;
    // The last few packets walked, so a truncation can name the packet whose LENGTH
    // was wrong rather than the innocent dword it eventually tripped over. A stopped
    // walk always reports the position where the arithmetic ran out, which is data by
    // then — one of the headers this caught was 3F800000, the float 1.0 — so without
    // the trail every report points at the wrong packet.
    struct Step
    {
        uint32_t pos, header, consumed;
    };
    Step trail[8] = {};
    uint32_t steps = 0;
    while (pos < sizeDwords)
    {
        const uint32_t header = fetch(pos);
        const uint32_t consumed = ExecutePacket(base, fetch, pos, sizeDwords - pos, depth);
        trail[steps % 8] = { pos, header, consumed };
        steps++;
        if (!consumed)
        {
            // A packet claiming more dwords than the buffer holds. Stopping is right —
            // guessing would desync — but stopping SILENTLY is not, and that is what
            // this did until finding 38.
            //
            // The driver's own progress fence is the LAST packet in these buffers, so
            // anything that cuts a buffer short drops the fence with it. The read
            // pointer the driver polls then stops advancing while our parser reports
            // itself fully caught up, and the next thread that waits on that fence
            // waits for the rest of the process's life. A truncation here is therefore
            // not a cosmetic parse issue; it is a hang, and it deserves to say so.
            static std::atomic<uint64_t> truncated{ 0 };
            const uint64_t n = truncated.fetch_add(1);
            if (n < 16 || (n & 0xFFFu) == 0)
            {
                fprintf(stderr,
                        "[pm4] indirect buffer TRUNCATED at dword %u of %u (va=%08X, "
                        "header=%08X, depth=%d, occurrence %llu) — every packet after "
                        "this one is lost, including any trailing fence\n",
                        pos, sizeDwords, va, fetch(pos), depth,
                        static_cast<unsigned long long>(n));
                const uint32_t shown = steps < 8 ? steps : 8;
                for (uint32_t i = shown; i-- > 0;)
                {
                    const Step& s = trail[(steps - 1 - i) % 8];
                    fprintf(stderr, "[pm4]   -%u: dword %u header %08X type %u op %02X "
                                    "count %u -> consumed %u\n",
                            i, s.pos, s.header, s.header >> 30, (s.header >> 8) & 0x7F,
                            ((s.header >> 16) & 0x3FFF) + 1, s.consumed);
                }
                // And the raw tail, because the question a truncation raises is what
                // the dwords we could not parse actually ARE. Capped: hardware's own
                // indirect buffers reach 65,522 dwords, so an early truncation in a
                // big one would otherwise dump 8,000 lines and bury the report it
                // belongs to. Use CZ_PM4_DUMP_TRUNCATED for the whole thing.
                const uint32_t tailEnd = std::min(sizeDwords, pos + 64);
                for (uint32_t i = pos; i < tailEnd; i++)
                    fprintf(stderr, "%s%08X", ((i - pos) % 8) ? " " : "\n[pm4]   tail ",
                            fetch(i));
                fprintf(stderr, "%s\n", tailEnd < sizeDwords ? " ..." : "");

                // CZ_PM4_DUMP_TRUNCATED=<path>: the whole buffer, once. The trail above
                // reaches a few packets back; the dword that was actually mis-sized can
                // be thousands of packets earlier, and only a full re-walk offline can
                // find it.
                if (const char* path = getenv("CZ_PM4_DUMP_TRUNCATED"))
                {
                    static std::atomic<uint32_t> dumps{ 0 };
                    const uint32_t index = dumps.fetch_add(1);
                    if (index < 6)
                    {
                        char name[512];
                        snprintf(name, sizeof(name), "%s.%u.%u.bin", path, index, sizeDwords);
                        if (FILE* f = fopen(name, "wb"))
                        {
                            for (uint32_t i = 0; i < sizeDwords; i++)
                            {
                                const uint32_t w = __builtin_bswap32(fetch(i));
                                fwrite(&w, 4, 1, f);
                            }
                            fclose(f);
                            fprintf(stderr, "[pm4] dumped %u dwords to %s\n", sizeDwords, name);
                        }
                    }
                }
            }
            break;
        }
        pos += consumed;
    }
}

} // namespace

// --- public interface ---------------------------------------------------------------

void Pm4_SetRingBuffer(uint32_t base, uint32_t sizeBytes)
{
    g_ringBase = base;
    g_ringDwords = sizeBytes / 4;
    g_cursor = 0;
}

bool Pm4_RingInitialized() { return g_ringBase != 0 && g_ringDwords != 0; }

uint32_t Pm4_Execute(uint8_t* base, uint32_t writePtr)
{
    if (!Pm4_RingInitialized())
        return writePtr;

    // The kicked write pointer counts linearly and never wraps; the cursor is
    // ring-relative. Both must be brought into the same space before anything is
    // compared, or the available-dword arithmetic starves the moment the ring
    // completes its first lap.
    const uint32_t target = writePtr % g_ringDwords;
    if (g_cursor == target)
        return g_cursor;

    Source fetch{ base, g_ringBase, g_ringDwords };

    uint32_t guard = g_ringDwords + 1; // never walk more than one lap per call
    while (g_cursor != target && guard--)
    {
        const uint32_t avail = (target + g_ringDwords - g_cursor) % g_ringDwords;
        const uint32_t consumed = ExecutePacket(base, fetch, g_cursor, avail, 0);
        if (!consumed)
            break; // tail packet not fully written yet — resume next tick
        g_cursor = (g_cursor + consumed) % g_ringDwords;
    }

    // A stall here is reported, not papered over. Skipping ahead to the write pointer
    // would make the boot proceed while telling the driver we consumed packets we
    // never parsed — after which it is entitled to recycle ring memory we are still
    // reading, and the resulting corruption looks nothing like its cause.
    // CZ_PM4_RESYNC=1 enables a forward scan to the next plausible packet header for
    // when that trade is worth making deliberately; it is off by default precisely so
    // a parser bug shows up as a stall rather than as mystery garbage.
    static uint32_t s_lastCursor = ~0u;
    static uint32_t s_staleTicks = 0;
    if (g_cursor == s_lastCursor && g_cursor != target)
    {
        if (++s_staleTicks >= 60)
        {
            static int dumps = 0;
            if (dumps++ < 4)
            {
                fprintf(stderr, "[pm4] STALLED at dword %u of %u (wptr %u); ring:", g_cursor,
                        g_ringDwords, target);
                for (uint32_t i = 0; i < 16; i++)
                    fprintf(stderr, "%s%08X", (i % 8) ? " " : "\n[pm4]   ",
                            fetch(g_cursor + i));
                fprintf(stderr, "\n");
            }
            if (getenv("CZ_PM4_RESYNC"))
            {
                const uint32_t avail = (target + g_ringDwords - g_cursor) % g_ringDwords;
                for (uint32_t k = 1; k < avail && k <= 64; k++)
                {
                    const uint32_t h = fetch(g_cursor + k);
                    const uint32_t cnt = ((h >> 16) & 0x3FFF) + 1;
                    // Type-3 with a known opcode only: a type-0 match is far too easy
                    // to fake — an ordinary body dword parses as one — and a wrong
                    // resync eats real packets.
                    if ((h >> 30) == 3 && kOpcodeNames.name[(h >> 8) & 0x7F] &&
                        cnt + 1 <= avail - k)
                    {
                        fprintf(stderr, "[pm4] resynced +%u dwords past the stall\n", k);
                        g_cursor = (g_cursor + k) % g_ringDwords;
                        break;
                    }
                }
            }
            s_staleTicks = 0;
        }
    }
    else
    {
        s_staleTicks = 0;
    }
    s_lastCursor = g_cursor;

    return g_cursor;
}

void Pm4_SetInterruptSink(void (*sink)()) { g_interruptSink = sink; }

uint32_t Pm4_Cursor() { return g_cursor; }
uint32_t Pm4_ScratchAddr() { return g_regs[kRegScratchAddr]; }
uint32_t Pm4_ScratchUmsk() { return g_regs[kRegScratchUmsk]; }

uint64_t Pm4_PacketCount() { return g_packets.load(); }
uint64_t Pm4_TypeCount(uint32_t type) { return type < 4 ? g_types[type].load() : 0; }
uint64_t Pm4_OpcodeCount(uint32_t opcode) { return opcode < 128 ? g_opcodes[opcode].load() : 0; }
uint64_t Pm4_DrawCount() { return g_draws.load(); }
uint64_t Pm4_FrameCount() { return g_frames.load(); }
uint64_t Pm4_InterruptCount() { return g_interrupts.load(); }
const char* Pm4_OpcodeName(uint32_t opcode)
{
    return opcode < 128 ? kOpcodeNames.name[opcode] : nullptr;
}
