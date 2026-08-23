// Offline gate for the vertical-waste predicate (part 72, CZ_VK_VCULL_CENSUS).
//
// WHY THIS EXISTS
// ---------------
// The census in runtime/gpu/vk_renderer.cpp answers the one question that decides the
// live performance plan's biggest item: how many world draws does the 21:9 culling
// over-widen submit that land entirely off-screen? Its whole content is one geometric
// predicate — project a box's eight corners by the final projection and ask whether all
// of them are outside the clip volume in Y — and a SIGN error in that predicate does not
// crash, does not look wrong, and reports a plausible number that an operator session
// cannot audit. That is the defect class this file exists to catch.
//
// Every case is a box whose classification is known from the projection's own geometry
// rather than from the code: centred, far above, far below, off each side, straddling an
// edge, behind the eye, straddling the near plane, and one box sitting inside the
// over-widen band (between the screen's vertical extent t and the widened frustum's k*t)
// — which is precisely the population the item proposes to recover. It also exercises
// both directions of the census's MECHANICAL control (CZ_VK_VCULL_SCALE): a tiny clip
// bound must turn an on-screen box into a wasted one, a huge one must do the reverse.
//
// The arithmetic below is copied VERBATIM from VerticalWasteCensus. That duplication is
// deliberate and it is the cost of having a gate at all: the predicate is fifteen lines
// inside a 22,000-line translation unit with no seam to link against. If you change the
// predicate there, change it here and re-run.
//
// Confirmed capable of failing (gotcha 30): flipping one comparison's sign
// (`cy <= by` -> `cy <= -by`) takes it from 0 failures to 6.
//
//   clang++ -O1 -o /tmp/vcull_predicate tools/vcull_predicate_test.cpp -lm && /tmp/vcull_predicate
//
#include <cstdio>
#include <cmath>
#include <cstring>
struct R3 { float mn[3], mx[3]; };
// Classify: 0 = on screen, 1 = wasted vertically, 2 = wasted horizontally, 3 = both,
// -1 = near-plane straddle (counted as on-screen by the census).
int Classify(const float* m, const R3& rec, float s)
{
    bool allAbove = true, allBelow = true, allLeft = true, allRight = true;
    for (int c = 0; c < 8; ++c)
    {
        const float x = (c & 1) ? rec.mx[0] : rec.mn[0];
        const float y = (c & 2) ? rec.mx[1] : rec.mn[1];
        const float z = (c & 4) ? rec.mx[2] : rec.mn[2];
        const float cw = m[12]*x + m[13]*y + m[14]*z + m[15];
        if (!(cw > 0.0f)) return -1;
        const float cx = m[0]*x + m[1]*y + m[2]*z + m[3];
        const float cy = m[4]*x + m[5]*y + m[6]*z + m[7];
        const float by = s*cw, bx = s*cw;
        if (cy <= by) allAbove = false;
        if (cy >= -by) allBelow = false;
        if (cx >= -bx) allLeft = false;
        if (cx <= bx) allRight = false;
    }
    return (allAbove||allBelow ? 1 : 0) | (allLeft||allRight ? 2 : 0);
}
int fails = 0;
void Check(const char* what, int got, int want)
{
    printf("  %-46s got %2d want %2d  %s\n", what, got, want, got==want?"ok":"FAIL");
    if (got != want) ++fails;
}
int main()
{
    // A 16:9 perspective in the renderer's convention: row-major, clip = M*(x,y,z,1),
    // row3 = (0,0,1,0) so w_clip = z_view. m11 = cot(vfov/2), m00 = m11*9/16.
    const float t = tanf(0.5f * 45.0f * 3.14159265f/180.0f);   // 45 deg vertical
    const float B = 1.0f/t, A = B*9.0f/16.0f;
    float m[16] = { A,0,0,0,  0,B,0,0,  0,0,1,0,  0,0,1,0 };
    // At z=100 the screen half-extents are: y = t*100, x = (16/9)*t*100.
    const float hy = t*100.0f, hx = hy*16.0f/9.0f;
    auto box=[&](float cx,float cy,float cz,float r){ R3 b; b.mn[0]=cx-r;b.mn[1]=cy-r;b.mn[2]=cz-r;
                                                      b.mx[0]=cx+r;b.mx[1]=cy+r;b.mx[2]=cz+r; return b; };
    Check("centred at z=100",              Classify(m, box(0,0,100,5), 1.0f), 0);
    Check("far ABOVE the screen",          Classify(m, box(0, hy*3, 100, 5), 1.0f), 1);
    Check("far BELOW the screen",          Classify(m, box(0,-hy*3, 100, 5), 1.0f), 1);
    Check("far LEFT of the screen",        Classify(m, box(-hx*3, 0, 100, 5), 1.0f), 2);
    Check("far RIGHT of the screen",       Classify(m, box( hx*3, 0, 100, 5), 1.0f), 2);
    Check("above AND right",               Classify(m, box( hx*3, hy*3, 100, 5), 1.0f), 3);
    Check("straddling the top edge",       Classify(m, box(0, hy, 100, 20), 1.0f), 0);
    Check("behind the eye",                Classify(m, box(0,0,-100,5), 1.0f), -1);
    Check("straddling the near plane",     Classify(m, box(0,0,0,50), 1.0f), -1);
    // The MECHANICAL control: a small clip scale must turn an on-screen off-centre box
    // into a 'wasted' one, and a large scale must turn a wasted one back on-screen.
    Check("off-centre box, scale 1.0",     Classify(m, box(0, hy*0.5f, 100, 5), 1.0f), 0);
    Check("off-centre box, scale 0.02",    Classify(m, box(0, hy*0.5f, 100, 5), 0.02f), 1);   // x straddles 0
    Check("far-above box, scale 50",       Classify(m, box(0, hy*3, 100, 5), 50.0f), 0);
    // THE SEMANTIC CASE, which is the whole item. The over-widen makes the game admit
    // geometry out to k*t vertically; the screen still ends at t. A box sitting in that
    // band is exactly what a horizontal-only cull fix would remove.
    const float k = 1.34375f;
    Check("in the over-widen band (t..k*t)", Classify(m, box(0, hy*1.17f, 100, 2), 1.0f), 1);
    printf("%s (%d failure(s))\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
