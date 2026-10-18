// prof.hz: a sampling profiler of the emulator's thread, for a handheld without perf (BananaOS). Off by default.
// A timer on the thread's own CPU clock (timer_create(CLOCK_THREAD_CPUTIME_ID), SIGEV_THREAD_ID) sends it SIGPROF
// prof.hz times a second of its CPU time; the handler only writes the interrupted PC into a preallocated array. At
// the end (prof.frames sampled, or the game's quit) a report: the share in JIT-generated code, the shares by module,
// the top symbols of the binary (its .symtab, or prof.syms's: the unstripped build of the same binary, since the
// device's is stripped) and of the libraries (dladdr), the same by C++ class, and, with MELONDS_JIT_PERFMAP=1, the
// hottest JIT blocks by guest address. The emulation never sees it: the same frames, RAM and sound.
#pragma once
#include <memory>

namespace bananads {

class Config;

class Profiler {
public:
    // nullptr unless prof.hz > 0
    static std::unique_ptr<Profiler> create(const Config& cfg);
    virtual ~Profiler() = default;
    // on the emulator's thread, before each frame (0, 1, ...): starts the timer at prof.from, stops it and reports
    // after prof.frames frames (0: until finish())
    virtual void frame(long n) = 0;
    // any thread, once the emulator's thread is done (or gone): stops the timer if it runs and reports
    virtual void finish() = 0;
};

}  // namespace bananads
