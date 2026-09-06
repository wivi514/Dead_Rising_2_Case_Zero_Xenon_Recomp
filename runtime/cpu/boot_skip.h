// The boot-logo skip's enabled check — CZ_SKIP_INTRO env (wins) over the
// persisted `skip_intro_logos` setting. Latched on first call, which is fine:
// both consumers (the VFS layer pick and the trace) run after settings load and
// the value is only meaningful for the run's whole boot. See boot_skip.cpp for
// the mechanism and the two refuted ones.
#pragma once

bool BootSkip_Enabled();
