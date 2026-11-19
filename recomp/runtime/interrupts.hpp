// Interrupts, delivered at safe points (docs/runtime.md). Runtime code: no
// std::format.
//
// The Wii raises an interrupt between any two instructions; lifted code
// can't be entered there, so this runtime delivers them where the game lets
// them in: when it enables interrupts (OSEnableInterrupts, or
// OSRestoreInterrupts with interrupts on), and while the scheduler is idle.
// A handler runs on the running guest thread, with interrupts off, and
// leaves every register as it found it, as the console's exception entry
// and exit do.
#pragma once

#include <cstdint>
#include <functional>

#include "guest.hpp"

namespace qilin::rt {

// The interrupt enable the game sees (MSR[EE]).
bool interrupts_enabled();
void set_interrupts_enabled(bool on);

// Queues a handler from any host thread; it runs at the next safe point as
// the SDK's dispatcher runs one: scheduler disabled, then enabled again and
// a reschedule (which may switch threads).
void post_interrupt(std::function<void(Context&)> handler);

// The decrementer (PPCMtdec): an interrupt `ticks` + 1 time-base ticks from
// now. Writing it again replaces the deadline, as on the hardware.
void set_decrementer(uint32_t ticks);

// A periodic interrupt (VI's retrace): `handler` runs every `period` ticks of
// the time base, as a queued handler runs. A late one runs once and the next
// is a period after it: the game sees no burst of retraces after a stall.
void add_periodic(uint64_t period, std::function<void(Context&)> handler);

// The exception handlers the OS installed (__OSSetExceptionHandler), by
// exception number.
uint32_t exception_handler(unsigned exception);
uint32_t set_exception_handler(unsigned exception, uint32_t handler);  // returns the old one

// A safe point: delivers whatever is due. `c` comes back unchanged.
void deliver_due(Context& c);
// The idle loop: waits for the next interrupt and delivers it. False if
// nothing could ever arrive (no decrementer set, nothing queued, nothing
// that could queue one).
bool wait_and_deliver(Context& c);

// OSLoadContext of the context an interrupt interrupted: thrown, to unwind
// the handler back to the delivery (the call never returns on the console).
struct Resume {};
bool delivering();

}  // namespace qilin::rt
