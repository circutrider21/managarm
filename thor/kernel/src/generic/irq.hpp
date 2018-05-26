#ifndef THOR_GENERIC_IRQ_HPP
#define THOR_GENERIC_IRQ_HPP

#include <frigg/debug.hpp>
#include <frigg/linked.hpp>
#include <frigg/string.hpp>
#include <frg/list.hpp>
#include "error.hpp"
#include "kernel_heap.hpp"

namespace thor {

struct AwaitIrqNode {
	friend struct IrqObject;

	virtual void onRaise(Error error, uint64_t sequence) = 0;

private:
	frg::default_list_hook<AwaitIrqNode> _queueNode;
};

// ----------------------------------------------------------------------------

struct IrqPin;

// Represents a slot in the CPU's interrupt table.
// Slots might be global or per-CPU.
struct IrqSlot {
	// Links an IrqPin to this slot.
	// From now on all IRQ raises will go to this IrqPin.
	void link(IrqPin *pin);

	// The kernel calls this function when an IRQ is raised.
	void raise();

private:
	IrqPin *_pin;
};

// ----------------------------------------------------------------------------

enum class IrqStatus {
	null,
	acked,
	nacked
};

struct IrqSink {
	friend struct IrqPin;

	IrqSink(frigg::String<KernelAlloc> name);

	const frigg::String<KernelAlloc> &name() {
		return _name;
	}

	virtual IrqStatus raise() = 0;

	// TODO: This needs to be thread-safe.
	IrqPin *getPin();

	frg::default_list_hook<IrqSink> hook;

protected:
	uint64_t currentSequence() {
		return _currentSequence;
	}

private:
	frigg::String<KernelAlloc> _name;

	IrqPin *_pin;

	// The following fields are protected by the mutex of IrqPin.
private:
	uint64_t _currentSequence;
	IrqStatus _status;
};

enum class IrqStrategy {
	null,
	justEoi,
	maskThenEoi
};

enum class TriggerMode {
	null,
	edge,
	level
};

enum class Polarity {
	null,
	high,
	low
};

// Represents a (not necessarily physical) "pin" of an interrupt controller.
// This class handles the IRQ configuration and acknowledgement.
struct IrqPin {
private:
	using Mutex = frigg::TicketLock;

public:
	static void attachSink(IrqPin *pin, IrqSink *sink);
	static Error ackSink(IrqSink *sink);
	static Error nackSink(IrqSink *sink, uint64_t sequence);
	static Error kickSink(IrqSink *sink);

public:
	IrqPin(frigg::String<KernelAlloc> name);

	IrqPin(const IrqPin &) = delete;
	
	IrqPin &operator= (const IrqPin &) = delete;

	const frigg::String<KernelAlloc> &name() {
		return _name;
	}

	void configure(TriggerMode mode, Polarity polarity);

	// This function is called from IrqSlot::raise().
	void raise();

private:
	void _acknowledge();
	void _kick();

public:
	void warnIfPending();

protected:
	virtual IrqStrategy program(TriggerMode mode, Polarity polarity) = 0;

	virtual void mask() = 0;
	virtual void unmask() = 0;

	// Sends an end-of-interrupt signal to the interrupt controller.
	virtual void sendEoi() = 0;

private:
	void _callSinks();

	frigg::String<KernelAlloc> _name;

	// Must be protected against IRQs.
	Mutex _mutex;

	IrqStrategy _strategy;

	uint64_t _raiseSequence;
	uint64_t _sinkSequence;
	bool _wasAcked;
	unsigned int _deferCounter;

	// Timestamp of the last acknowledge() operation.
	// Relative to currentNanos().
	uint64_t _raiseClock;
	
	bool _warnedAfterPending;

	// TODO: This list should change rarely. Use a RCU list.
	frg::intrusive_list<
		IrqSink,
		frg::locate_member<
			IrqSink,
			frg::default_list_hook<IrqSink>,
			&IrqSink::hook
		>
	> _sinkList;
};

// ----------------------------------------------------------------------------

// This class implements the user-visible part of IRQ handling.
struct IrqObject : IrqSink {
private:
	using Mutex = frigg::TicketLock;

public:
	IrqObject(frigg::String<KernelAlloc> name);

	IrqStatus raise() override;

	void submitAwait(AwaitIrqNode *node, uint64_t sequence);

private:
	// Must be protected against IRQs.
	Mutex _mutex;

	// Protected by the mutex.
	frg::intrusive_list<
		AwaitIrqNode,
		frg::locate_member<
			AwaitIrqNode,
			frg::default_list_hook<AwaitIrqNode>,
			&AwaitIrqNode::_queueNode
		>
	> _waitQueue;
};

} // namespace thor

#endif // THOR_GENERIC_IRQ_HPP
