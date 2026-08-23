// Offline gate for the DRAW-ORDER GATE's hash (part 72, CZ_VK_ORDER_GATE).
//
// WHY THIS EXISTS
// ---------------
// The order gate guards the largest and riskiest item in the performance plan: parallel
// command recording. Draw order on this title is SEMANTIC, so a recorder that emits the
// same draws in a different order produces a wrong picture that no frame time, counter or
// era median would flag. The gate hashes the draw identities sequentially and compares.
//
// ITS ONE FATAL FAILURE MODE IS A COMMUTATIVE HASH. If mixing were XOR-only, or addition,
// then transposing two draws would leave the hash UNCHANGED and the gate would pass a
// misordered frame forever while looking like it was working. That is not a hypothetical
// preference between hash functions — it is the difference between a gate and a placebo,
// and "FNV is non-commutative" is exactly the kind of thing this project has a rule about
// asserting rather than measuring.
//
// So this checks the property directly, on the arithmetic the runtime actually uses:
// every adjacent transposition in a realistic sequence must change the hash, and so must
// a reversal, a rotation and a single substitution. It also checks the NEGATIVE case —
// an unchanged sequence must hash identically — because a hash that changed every time
// would "detect" everything and mean nothing.
//
//   clang++ -O1 -o /tmp/order_gate tools/order_gate_test.cpp && /tmp/order_gate
//
#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>

static inline uint64_t mixq(uint64_t h, uint64_t v)
{
    h ^= v;
    return h * 0x100000001B3ull;      // the runtime's mix, verbatim
}

static uint64_t seq_hash(const std::vector<uint64_t>& v)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint64_t x : v)
        h = mixq(h, x);
    return h;
}

// A draw identity built the way DoDraw builds it: ordinal, pipeline, prim/count,
// index range, then the two shader hashes.
static uint64_t ident(uint64_t ord, uint64_t pipe, uint64_t prim, uint64_t idx,
                      uint64_t vs, uint64_t ps)
{
    uint64_t h = 0xCBF29CE484222325ull;
    h = mixq(h, ord); h = mixq(h, pipe); h = mixq(h, prim);
    h = mixq(h, idx); h = mixq(h, vs);   h = mixq(h, ps);
    return h;
}

int fails = 0;
static void Check(const char* what, bool ok)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

int main()
{
    // A frame's worth of draws, with deliberate near-duplicates: many draws sharing one
    // pipeline and one shader pair, differing only in index range. Those are the pairs a
    // parallel recorder is most likely to transpose and the hardest for a hash to tell
    // apart, so the test population is made of them on purpose.
    std::vector<uint64_t> f;
    for (uint64_t i = 0; i < 4000; ++i)
        f.push_back(ident(i, 0x7000 + (i % 7), (4ull << 32) | (30 + i % 3),
                          (i * 96) << 32 | (i * 3), 0xAABBCCDDull, 0x11223344ull));
    const uint64_t base = seq_hash(f);

    Check("an unchanged sequence hashes identically", seq_hash(f) == base);

    // EVERY adjacent transposition must be detected — not a sample, all 3,999 of them.
    size_t missed = 0;
    for (size_t i = 0; i + 1 < f.size(); ++i)
    {
        std::vector<uint64_t> g = f;
        std::swap(g[i], g[i + 1]);
        if (seq_hash(g) == base) ++missed;
    }
    printf("  adjacent transpositions missed: %zu of %zu\n", missed, f.size() - 1);
    Check("every adjacent transposition changes the hash", missed == 0);

    // Distant transpositions, a rotation, a reversal, a single substitution, and a drop.
    {
        std::vector<uint64_t> g = f; std::swap(g[3], g[3500]);
        Check("a distant transposition changes the hash", seq_hash(g) != base);
    }
    {
        std::vector<uint64_t> g = f; std::rotate(g.begin(), g.begin() + 1, g.end());
        Check("a rotation by one changes the hash", seq_hash(g) != base);
    }
    {
        std::vector<uint64_t> g = f; std::reverse(g.begin(), g.end());
        Check("a reversal changes the hash", seq_hash(g) != base);
    }
    {
        std::vector<uint64_t> g = f; g[1234] ^= 1ull;
        Check("a one-bit change in one draw changes the hash", seq_hash(g) != base);
    }
    {
        std::vector<uint64_t> g = f; g.erase(g.begin() + 2000);
        Check("a DROPPED draw changes the hash", seq_hash(g) != base);
    }
    {
        std::vector<uint64_t> g = f; g.push_back(g.back());
        Check("a DUPLICATED draw changes the hash", seq_hash(g) != base);
    }

    // THE NEGATIVE CONTROL FOR THE TEST ITSELF: a commutative mix must FAIL the
    // transposition case. If this passes, the test above is not testing what it claims —
    // it would pass for any hash at all.
    {
        auto comm = [](const std::vector<uint64_t>& v) {
            uint64_t h = 0xCBF29CE484222325ull;
            for (uint64_t x : v) h ^= x;          // XOR: order-blind on purpose
            return h;
        };
        std::vector<uint64_t> g = f; std::swap(g[10], g[11]);
        Check("(control) a COMMUTATIVE mix is blind to a transposition",
              comm(g) == comm(f));
    }

    printf("%s (%d failure(s))\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
