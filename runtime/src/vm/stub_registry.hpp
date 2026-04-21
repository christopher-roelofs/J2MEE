#pragma once
#include "class_def.hpp"

#include <cstdio>
#include <string>

// ─── Stub registry ────────────────────────────────────────────────────────────
// Tracks native methods that aren't real implementations so we notice when a
// game depends on one. Two flavors:
//
//   Noop        — return value is spec-legal (null Control, false from
//                 platformRequest, empty lifecycle callbacks). Game behavior
//                 is correct; we just want an inventory.
//   Placeholder — return value is a plausible lie (e.g. setMediaTime returning
//                 the requested time without seeking). Game progresses but
//                 loses the feature. These are the ones we need to revisit.
//
// Both kinds log on first call and appear in the end-of-run report.

enum class StubKind { Noop, Placeholder };

// Wrap a NativeFunc with first-call logging + hit counting. Call sites supply
// the fully-qualified symbol (class.name + descriptor) and a short note that
// tells future-us exactly what's fake about it.
NativeFunc wrap_stub(StubKind kind,
                     std::string symbol,
                     std::string note,
                     NativeFunc impl);

// Print the hit report to stderr. Called once at shutdown by main.cpp.
void dump_stub_report();
