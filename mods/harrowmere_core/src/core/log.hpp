// =============================================================================
// Harrowmere — mods/harrowmere_core/src/core/log.hpp
// Task 0.5 — Logging & tracing conventions (Phase 1 — Walk; common.md §3.4).
//
// Uniform, category-scoped logging over the engine's `logging` capability
// (family `logging`, provider `logging_stdout`), with zero hot-path cost by
// default.
//
// PUBLIC SURFACE — the only logging any system in harrowmere_core may use:
//
//     HARROWMERE_LOG_TRACE(category, fmt, ...)
//     HARROWMERE_LOG_DEBUG(category, fmt, ...)
//     HARROWMERE_LOG_INFO (category, fmt, ...)
//     HARROWMERE_LOG_WARN (category, fmt, ...)
//     HARROWMERE_LOG_ERROR(category, fmt, ...)
//
// `category` is one of the nine pinned tokens in the `cats` namespace below —
// anything else is a compile error, so the set stays closed exactly as task
// 0.5 pins it and a typo cannot invent a rogue category. `fmt` is printf-
// style, compile-checked against the arguments on GCC/Clang:
//
//     HARROWMERE_LOG_INFO(world, "world generated: %u regions, %llu npcs",
//                         regionCount, (unsigned long long)npcCount);
//
// PINNED RULES (task 0.5 — enforced here, or documented for call sites):
//   (1) Log messages are ONE line and carry no player-identifying data
//       beyond ids. write() flattens any embedded '\n'/'\r' and caps the
//       composed line, so a stray newline can never split a log record.
//   (2) Anything inside the per-tick steps (§4.4) logs ONLY through
//       HARROWMERE_LOG_TRACE — the macro compiles to nothing unless the
//       build defines HARROWMERE_TRACE (common.md §3.4). Do not add
//       "temporary" INFO lines to per-tick loops: task 8.3's step-cost
//       budgets exist to catch that soak, but don't wait for it.
//   (3) System-entry events (assembly up, world generated, save written,
//       warrant issued, ...) log at INFO with the decisive numbers inline.
//       The default compile-time floor is INFO, so rule-(3) sites are always
//       compiled in.
//
// COMPILE-TIME LEVEL SWITCH (task 0.5 Done-when: "documented")
//
//   A level is compiled into the binary only when it sits at or above the
//   floor macro HARROWMERE_LOG_LEVEL — an integer compared by the
//   preprocessor. Set it per target, e.g.
//
//       target_compile_definitions(harrowmere_core PRIVATE
//           HARROWMERE_LOG_LEVEL=HARROWMERE_LOG_LEVEL_DEBUG)
//
//   or on the compiler command line (-DHARROWMERE_LOG_LEVEL=1 works too):
//
//       value  floor macro                    compiles in
//       0      HARROWMERE_LOG_LEVEL_TRACE     everything (*)
//       1      HARROWMERE_LOG_LEVEL_DEBUG     DEBUG and above
//       2      HARROWMERE_LOG_LEVEL_INFO      INFO and above   <-- DEFAULT
//       3      HARROWMERE_LOG_LEVEL_WARN      WARN and above
//       4      HARROWMERE_LOG_LEVEL_ERROR     ERROR only
//       5      HARROWMERE_LOG_LEVEL_OFF       nothing, not even ERROR
//
//   (*) TRACE additionally requires -DHARROWMERE_TRACE: the flag gates the
//   per-tick TRACE level specifically, so a DEBUG-level dev build still
//   ships without tick chatter unless it also defines HARROWMERE_TRACE.
//
//   Stripped call sites are kept as real calls inside `if constexpr (false)`:
//   fully type-checked and format-checked (a call site cannot rot silently
//   while its level is compiled out) but never evaluated — zero runtime
//   cost even in -O0 builds, and the provider's wouldLog probe is not paid.
//   Consequences for call sites:
//     * never pass expressions with side effects to a log macro;
//     * expressions containing top-level commas must be parenthesized
//       (ordinary variadic-macro rules);
//     * inside template bodies a variable used ONLY by a stripped call may
//       still trip -Wunused-parameter/-Wunused-variable — mark it
//       [[maybe_unused]] there (non-template bodies need nothing).
//
// MESSAGE SHAPE
//
//   A composed line is "[<category>] <message>", e.g.
//       "[justice] warrant 7 issued against character 9"
//   Ids are std::uint64_t: print via %llu with an explicit cast to
//   unsigned long long. std::string arguments are not varargs-legal — pass
//   .c_str(). Lines cap at LINE_BYTES bytes (the message part is truncated
//   with a "..." marker). LINE_BYTES is logging plumbing, not a game-rule
//   constant — it never enters simulation outcomes, so it claims no
//   Appendix A row (common.md §1.3 scopes the registry to game-rule values).
//
// ENGINE SEAM & WIRING
//
//   This header is the ONLY engine-API-aware logging code in the mod — the
//   Done-when grep ("no direct capability calls outside log.hpp") reads on
//   it. The engine publishes the capability vocabulary from its own module
//   directory (modules/logging_stdout/logging_capability.hpp, consumed via
//   the engine_capability_logging INTERFACE target harrowmere_core links):
//
//       engine::LogLevel { Trace, Debug, Information, Warning, Error }
//       engine::LoggerCapability::logMessage(LogLevel, const std::string&)
//       engine::LoggerCapability::wouldLog(LogLevel) const   // min-level probe
//
//   harrowmere_plugin.cpp wires the sink in its lifecycle and never calls
//   the capability for game logs:
//
//       construct(): logging_ = &context.acquireCapability<engine::LoggerCapability>();
//                    harrowmere::log::install(logging_);
//       stop():      HARROWMERE_LOG_INFO(sim, "harrowmere_core stopping (state flushed)");
//                    harrowmere::log::uninstall();   // before the capability dies
//
//   Until install() runs — and in the doctest binary, which links objects
//   without the plugin scanner (task 0.6) — every write() drops its line
//   instead of crashing. install()/uninstall() are main-thread lifecycle
//   calls made outside tick steps; write() itself is reentrancy-safe (stack
//   buffer only, no heap, no locks) and noexcept: a logging sink that throws
//   is a broken engine contract, and an exception escaping into a tick step
//   is worse than a loud terminate.
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

// --- engine seam -------------------------------------------------------------
// Published by the engine's logging_stdout module; the include path comes from
// the engine_capability_logging INTERFACE target (task 0.1 links it).
#include "logging_capability.hpp"

namespace harrowmere::log {

// Severity ladder. The values double as preprocessor weights (see the level
// macros at the bottom of this file) and are mapped onto engine::LogLevel in
// detail::toEngineLevel().
enum class Level : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Off   = 5,  // sentinel: a floor of this value silences everything
};

// Pinned categories — task 0.5. CLOSED set: the macros accept only these
// tokens (anything else fails to compile). Extending the set is a plan
// change: deviation row first (common.md §1.4), then one new entry here.
namespace cats {
inline constexpr std::string_view sim     = "sim";      // ticks, scheduler, worldgen, seeds
inline constexpr std::string_view combat  = "combat";   // combat resolution, deaths
inline constexpr std::string_view npc     = "npc";      // needs, duties, aging, migration, LOD
inline constexpr std::string_view economy = "economy";  // prices, production/consumption
inline constexpr std::string_view events  = "events";   // event engine lifecycles
inline constexpr std::string_view justice = "justice";  // evidence, cases, warrants, jail
inline constexpr std::string_view world   = "world";    // regions, territories, seasons, weather
inline constexpr std::string_view save    = "save";     // save/load, schema, autosave
inline constexpr std::string_view ui      = "ui";       // view rebuilds, signals, panels
}  // namespace cats

// Composed-line bound ("[economy] " is the longest prefix, at 10 bytes).
// Logging plumbing, not a game rule — no Appendix A row (see header doc).
inline constexpr std::size_t LINE_BYTES = 512;

static_assert(int(Level::Trace) == 0 && int(Level::Debug) == 1 &&
              int(Level::Info)  == 2 && int(Level::Warn)  == 3 &&
              int(Level::Error) == 4 && int(Level::Off)   == 5,
              "level weights are part of the preprocessor contract below");
static_assert(cats::sim.size()     <= 15 && cats::combat.size() <= 15 &&
              cats::npc.size()     <= 15 && cats::economy.size() <= 15 &&
              cats::events.size()  <= 15 && cats::justice.size() <= 15 &&
              cats::world.size()   <= 15 && cats::save.size()    <= 15 &&
              cats::ui.size()      <= 15,
              "category names must fit the composed-line prefix budget");

// --- sink handle + the single emit path --------------------------------------
namespace detail {

// Set once by install() from the plugin lifecycle (main thread, construct),
// cleared by uninstall() (main thread, stop). Never touched from tick steps.
inline engine::LoggerCapability *g_logger = nullptr;

inline engine::LogLevel toEngineLevel(Level level) noexcept
{
    switch (level) {
        case Level::Trace: return engine::LogLevel::Trace;
        case Level::Debug: return engine::LogLevel::Debug;
        case Level::Info:  return engine::LogLevel::Information;
        case Level::Warn:  return engine::LogLevel::Warning;
        case Level::Error: return engine::LogLevel::Error;
        case Level::Off:   break;  // never emitted; silenced here
    }
    return engine::LogLevel::Information;
}

// Provider-side minimum-level probe (engine::LoggerCapability::wouldLog): it
// answers the identical question logMessage would answer, without paying the
// formatting first. Returns false when no sink is installed (tests, pre-
// install), so the null-sink case folds into the same early-out.
inline bool wouldLogAt(Level level) noexcept
{
    engine::LoggerCapability *logger = g_logger;
    if (logger == nullptr) return false;
    return logger->wouldLog(toEngineLevel(level));
}

// THE one call into the logging capability. `line` arrives pre-composed as
// "[category] message": the category is a game-layer concept (pinned above),
// so the wrapper owns it and the provider prints the finished line — exactly
// how the plugin's own lifecycle logging behaves.
inline void emit(Level level, const char *line) noexcept
{
    engine::LoggerCapability *logger = g_logger;
    if (logger == nullptr) return;  // belt and braces; wouldLogAt already gated
    logger->logMessage(toEngineLevel(level), line);
}

}  // namespace detail

// --- lifecycle wiring (called ONLY from harrowmere_plugin.cpp) ---------------
inline void install(engine::LoggerCapability *logger) noexcept { detail::g_logger = logger; }
inline void uninstall() noexcept                               { detail::g_logger = nullptr; }

// --- one-line writer ----------------------------------------------------------
// printf-style formatting into a stack buffer, compile-checked on GCC/Clang
// (format string = parameter 3, varargs from 4). Truncating, newline-
// flattening, allocation-free, noexcept.
#if defined(__GNUC__) || defined(__clang__)
#define HARROWMERE_LOG_PRINTF_FORMAT __attribute__((format(gnu_printf, 3, 4)))
#else
#define HARROWMERE_LOG_PRINTF_FORMAT /* no compile-time format check on this compiler */
#endif

HARROWMERE_LOG_PRINTF_FORMAT
inline void write(Level level, std::string_view category, const char *fmt, ...) noexcept
{
    if (!detail::wouldLogAt(level)) return;  // absent sink or provider floor

    // Compose "[category] message" in one stack buffer.
    char line[LINE_BYTES];
    std::size_t prefix = 0;
    line[prefix++] = '[';
    std::memcpy(line + prefix, category.data(), category.size());
    prefix += category.size();
    line[prefix++] = ']';
    line[prefix++] = ' ';

    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(line + prefix, LINE_BYTES - prefix, fmt, args);
    va_end(args);
    if (written < 0) return;  // encoding failure: drop the line, never throw

    std::size_t used = prefix + static_cast<std::size_t>(written);
    if (used >= LINE_BYTES) {
        // Truncated: vsnprintf already NUL-terminated at line[LINE_BYTES-1];
        // pin a "..." marker over the tail so the cut is visible.
        used = LINE_BYTES - 1;
        constexpr std::size_t kMarker = 3;
        std::memcpy(line + used - kMarker, "...", kMarker);
    }

    // Rule (1): one line — flatten anything that would split the record.
    for (std::size_t i = 0; i < used; ++i)
        if (line[i] == '\n' || line[i] == '\r') line[i] = ' ';

    // line[used] is already '\0' (vsnprintf terminator, truncation path kept).
    detail::emit(level, line);
}

}  // namespace harrowmere::log

// =============================================================================
// Macros — the pinned public surface, defined after the namespace so every
// call site is fully qualified and engine-agnostic.
// =============================================================================

#define HARROWMERE_LOG_WRITE(level_, category_, ...)                                 \
    ::harrowmere::log::write(::harrowmere::log::Level::level_,                       \
                             ::harrowmere::log::cats::category_, __VA_ARGS__)

// A stripped call stays a real (type- and format-checked) call inside a
// discarded branch: never evaluated, zero runtime cost, cannot rot. Flipping
// `false` to `true` locally promotes it to its intended level.
#define HARROWMERE_LOG_STRIPPED(level_, category_, ...)                              \
    do {                                                                             \
        if constexpr (false) {                                                       \
            HARROWMERE_LOG_WRITE(level_, category_, __VA_ARGS__);                    \
        }                                                                            \
    } while (0)

// --- compile-time level switch (documented in the header doc block) ----------
#ifndef HARROWMERE_LOG_LEVEL_TRACE
#define HARROWMERE_LOG_LEVEL_TRACE 0
#endif
#ifndef HARROWMERE_LOG_LEVEL_DEBUG
#define HARROWMERE_LOG_LEVEL_DEBUG 1
#endif
#ifndef HARROWMERE_LOG_LEVEL_INFO
#define HARROWMERE_LOG_LEVEL_INFO 2
#endif
#ifndef HARROWMERE_LOG_LEVEL_WARN
#define HARROWMERE_LOG_LEVEL_WARN 3
#endif
#ifndef HARROWMERE_LOG_LEVEL_ERROR
#define HARROWMERE_LOG_LEVEL_ERROR 4
#endif
#ifndef HARROWMERE_LOG_LEVEL_OFF
#define HARROWMERE_LOG_LEVEL_OFF 5
#endif

// Default posture: release/CI-clean. INFO stays compiled in so rule-(3)
// system-entry events always log; DEBUG/TRACE are stripped unless raised.
#ifndef HARROWMERE_LOG_LEVEL
#define HARROWMERE_LOG_LEVEL HARROWMERE_LOG_LEVEL_INFO
#endif

#if HARROWMERE_LOG_LEVEL < HARROWMERE_LOG_LEVEL_TRACE ||                            \
    HARROWMERE_LOG_LEVEL > HARROWMERE_LOG_LEVEL_OFF
#error "HARROWMERE_LOG_LEVEL must be one of the HARROWMERE_LOG_LEVEL_* values (0-5)"
#endif

// --- the five pinned macros ---------------------------------------------------
#if HARROWMERE_LOG_LEVEL <= HARROWMERE_LOG_LEVEL_ERROR
#define HARROWMERE_LOG_ERROR(category_, ...)                                         \
    HARROWMERE_LOG_WRITE(Error, category_, __VA_ARGS__)
#else
#define HARROWMERE_LOG_ERROR(category_, ...)                                         \
    HARROWMERE_LOG_STRIPPED(Error, category_, __VA_ARGS__)
#endif

#if HARROWMERE_LOG_LEVEL <= HARROWMERE_LOG_LEVEL_WARN
#define HARROWMERE_LOG_WARN(category_, ...)                                          \
    HARROWMERE_LOG_WRITE(Warn, category_, __VA_ARGS__)
#else
#define HARROWMERE_LOG_WARN(category_, ...)                                          \
    HARROWMERE_LOG_STRIPPED(Warn, category_, __VA_ARGS__)
#endif

#if HARROWMERE_LOG_LEVEL <= HARROWMERE_LOG_LEVEL_INFO
#define HARROWMERE_LOG_INFO(category_, ...)                                          \
    HARROWMERE_LOG_WRITE(Info, category_, __VA_ARGS__)
#else
#define HARROWMERE_LOG_INFO(category_, ...)                                          \
    HARROWMERE_LOG_STRIPPED(Info, category_, __VA_ARGS__)
#endif

#if HARROWMERE_LOG_LEVEL <= HARROWMERE_LOG_LEVEL_DEBUG
#define HARROWMERE_LOG_DEBUG(category_, ...)                                         \
    HARROWMERE_LOG_WRITE(Debug, category_, __VA_ARGS__)
#else
#define HARROWMERE_LOG_DEBUG(category_, ...)                                         \
    HARROWMERE_LOG_STRIPPED(Debug, category_, __VA_ARGS__)
#endif

// Rule (2): the ONLY legal level inside the per-tick steps (§4.4). It exists
// in the binary solely when the build defines HARROWMERE_TRACE AND the floor
// allows TRACE; otherwise it is the stripped no-op.
#if defined(HARROWMERE_TRACE) && HARROWMERE_LOG_LEVEL <= HARROWMERE_LOG_LEVEL_TRACE
#define HARROWMERE_LOG_TRACE(category_, ...)                                         \
    HARROWMERE_LOG_WRITE(Trace, category_, __VA_ARGS__)
#else
#define HARROWMERE_LOG_TRACE(category_, ...)                                         \
    HARROWMERE_LOG_STRIPPED(Trace, category_, __VA_ARGS__)
#endif
