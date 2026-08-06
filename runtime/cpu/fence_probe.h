// The CZ_FENCE_PROBE line budget, shared outside cpu/guest_probe.cpp.
//
// The probe's hooks live in guest_probe.cpp because that is where instruments go, but
// one of the functions it watches — sub_8284B9C0, the frame-end async submit — also
// has to be SERVICED by gpu/d3d_hooks.cpp, and two strong PPC_FUNC definitions of one
// guest symbol cannot link (the same constraint the hook table records for
// sub_8284B568). So that hook lives with the services and borrows the probe's budget
// through here, rather than keeping a second counter that would make the two halves of
// one picture cap at different points.
#pragma once

// True while the shared CZ_FENCE_PROBE budget has lines left; false when the probe is
// off. Consumes one line per true return.
bool FenceProbe_Line();
