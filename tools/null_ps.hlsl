// THE NULL PIXEL SHADER: a fragment stage that does nothing.
//
// WHY THIS EXISTS (part 106). The GPU split (CZ_VK_GPU_PASSES) says WHERE the device's
// frame goes by region — 77-82% of it in the title's own >=256-draw passes at 1080p —
// and nothing in this port has ever said what those passes are made of: vertex work,
// pixel work, or the fixed per-draw cost of rasterising 9,000 draws. A translation
// layer can only act on a share it has named. Bound in place of every translated pixel
// shader (CZ_VK_NULL_PS=1), this module leaves the vertex shader, the vertex input, the
// depth test and write, the MSAA state and the raster all exactly as the draw would
// normally have them, and removes ONLY the pixel shading. The device frame with it is
// "everything but pixel shading"; the difference from the base run is what the pixel
// shaders cost. CZ_VK_SCISSOR_1PX=1 is the complementary arm (everything but the
// pixels themselves — vertex + per-draw cost), and the pair brackets the decomposition.
//
// It writes no colour, so the picture is garbage BY DESIGN — this is a measurement arm
// and can never be a mode. Compiled by the same DXC and flags as the shader cache
// (tools/gen_null_ps_shader.sh), checked in as runtime/gpu/null_ps_spv.h.
void main()
{
}
