// Guest threads on host threads (threads.cpp). Runtime code: no std::format.
#pragma once

#include "guest.hpp"

namespace qilin::rt {

// Thrown through lifted code to end a host thread whose guest thread will
// never run again (it exited, or its context was reused for a new thread).
struct ThreadEnd {};

// Reads SelectThread's globals from its instructions; `boot` is the start-up
// register state (r13, and the special registers new threads start with).
// False, with the reason on stderr, if they aren't the supported build's.
bool init_threads(const Context& boot);
// After OSInit: the host thread running start-up runs the OS's default
// thread (the current thread __OSThreadInit set).
void adopt_boot_thread();
// For a host thread whose guest thread has exited while others go on.
[[noreturn]] void wait_forever();

}  // namespace qilin::rt
