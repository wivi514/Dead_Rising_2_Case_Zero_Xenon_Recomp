// Phase 5: the renderer. Translates the PM4 draw stream onto a host Vulkan device
// using the XenosRecomp-translated shaders in assets/shader_spv.
//
// WHERE THIS SITS
// ---------------
// The command processor (gpu/pm4.cpp) already walks the guest's real command stream,
// keeps a register file and knows which microcode is bound. This module is the sink it
// hands draws to. Nothing here parses packets and nothing here talks to SDL: the draw
// arrives decoded, and the finished frame leaves as RGBA8 through the present seam
// phase 3 built.
//
// That split is deliberate and it is what findings 38-39 bought. A renderer that
// reached back into the ring would be able to render a frame the parser never reached,
// and the frame counter in the window title would stop being the same number the ring
// trace prints. Everything here happens at a stream position the parser actually got
// to.
//
// EDRAM SEMANTICS, AND WHY THE TARGET IS NOT CLEARED PER FRAME
// -----------------------------------------------------------
// The Xbox 360 renders into a 10 MB on-die EDRAM and then RESOLVES a region of it into
// guest memory. The title clears through the copy block's own clear bits, not with a
// full-screen draw, so a host renderer that clears its colour target at the top of
// every frame is inventing a clear the title did not ask for — and one that discards
// content later passes go on to sample. The target here is persistent, and it is
// cleared exactly when the guest's resolve says to clear it.
//
// OFF BY DEFAULT UNTIL IT IS GOOD ENOUGH TO BE ON: CZ_VKDRAW=1 enables it. That is not
// timidity, it is the same-binary control arm every claim in this project needs — with
// it off the runtime is byte-for-byte the phase 3 binary, so "did the renderer change
// the kernel gates / the frame rate / the stall rate" is one environment variable
// rather than a rebuild (gotcha 86).
#pragma once

#include <cstdint>

struct Pm4Draw;

// Bring up the device, load the shader cache, allocate the render targets. Returns
// false and stays false when CZ_VKDRAW is unset, when there is no usable Vulkan
// device, or when the shader cache is missing — each of which is reported once, by
// name, because a renderer that silently declined to start looks exactly like a
// renderer that started and drew nothing.
bool VkRenderer_Init();

// True once Init has succeeded. Cheap; makes no Vulkan calls.
bool VkRenderer_Active();

// One draw, from inside the PM4 walk, with the register file and the bound shaders
// current. Resolves arrive here too — they are draws with RB_MODECONTROL's edram_mode
// set to kCopy, not a packet of their own — and are routed internally.
void VkRenderer_Draw(uint8_t* base, const Pm4Draw& draw);

// The XE_SWAP packet: submit the frame, read the resolved surface back and publish it
// to the window. Called from the same walk, at the swap's own position in the stream.
void VkRenderer_OnSwap(uint8_t* base, uint32_t frontBuffer, uint32_t width,
                       uint32_t height);

// The periodic counter block (CZ_VK_STATS=1, and once at exit). Every "we could not
// draw this" path increments a named counter rather than returning quietly, so the
// gap between "97 M packets parsed" and "the picture is missing something" is a number
// instead of a hunt.
void VkRenderer_DumpStats();

// Ask the swapchain to rebuild at the next present even though the drawable size is
// unchanged — the seam a live VSync change needs (part 60): the present mode is a
// property of the swapchain, so FIFO<->MAILBOX means recreating it. Callable from
// any thread; a no-op in the readback present arm.
void VkRenderer_RequestSwapchainRebuild();

// Change the internal render scale (1..4 over 1280x720) at the next frame
// boundary — the settings panel's resolution row (part 60). Refused loudly when
// CZ_VK_RES/CZ_VK_RES_SCALE pin the scale for a measurement run.
void VkRenderer_RequestRenderScale(uint32_t scale);

// ===================================================================================
// Phase C (the D3D pivot): the SAME renderer driven from the API line
// ===================================================================================
// gpu/d3d_draw.cpp walks the packets the title's own draw flush emits (into a private
// scratch, never the ring) and hands each draw here with ITS register file and shader
// hashes, instead of this module reading pm4.cpp's globals. Exactly one of the two
// feeds can be live in a run: CZ_VKDRAW=1 activates the PM4 feed and makes these
// no-ops; CZ_D3D_DRAW=1 activates these and makes VkRenderer_Draw/OnSwap no-ops. The
// mutual exclusion is enforced at init, loudly — two feeds into one EDRAM image is a
// collision, not an arm.
struct Pm4ShaderBinding;

bool VkRenderer_D3DInit();
void VkRenderer_D3DDraw(uint8_t* base, const Pm4Draw& draw, const uint32_t* regs,
                        const Pm4ShaderBinding& vs, const Pm4ShaderBinding& ps);
// Present the accumulated frame. The front buffer is the last resolve's destination —
// at the API line the PreSwapResolve immediately before every Swap names it, so no
// side channel is needed.
void VkRenderer_D3DSwap(uint8_t* base);
