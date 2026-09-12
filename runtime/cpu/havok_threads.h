#pragma once

// The number of HavokWorkerThreads the title's physics init creates (part 118). 2 is
// stock and the same-binary control; CZ_HAVOK_WORKERS=N overrides. Read by the hooks in
// havok_threads.cpp and by whatever prints the configuration.
unsigned HavokWorkers();
