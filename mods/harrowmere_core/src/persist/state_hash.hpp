#ifndef HARROWMERE_CORE_PERSIST_STATE_HASH_HPP
#define HARROWMERE_CORE_PERSIST_STATE_HASH_HPP

// persist/state_hash.hpp — the canonical state hasher (task 8.1's file;
// pulled forward by 7.1 and 8.3, which both consume it: 7.1's roundtrip test
// hashes the canonical state before/after a save/load cycle, and 8.3's soak
// dumps the month index + hash on an invariant failure for reproduction).
//
// Spec (8.1): FNV-1a over a deterministic serialization of every store —
// arrays in map order; floats excluded, positions hash as llround(x*1000)
// (R5's f64 stays out of game rules, so the hash must not see it either).
//
// What the hash covers: exactly the persisted field set (the save schema's
// stores). cachedPower/powerDirty are transient (7.1's do-not-save list) and
// excluded — hashing them would flake on any mid-flush state; the 8.1 note
// "hash after step 12" exists precisely because mid-tick dirty states are
// not canonical. The "runtime" pacing block is not world state and is
// excluded too.
//
// The hash is IN-PROCESS canonical: two builds of the same state in one
// process produce the same digest byte-for-byte. It is not a cross-build
// stable ABI value (std::string mixing rides the labels below, which are
// pinned constants, but the contract's consumers — the roundtrip test, the
// fuzz property and the soak — all compare within one process/binary).

#include <cstdint>
#include <string>

#include "persist/save.hpp" // SaveSources (the walked bundle)

namespace harrowmere::persist
{

// The hasher: FNV-1a 64-bit over the mixed field stream. Every field mixes a
// text label before its value so field boundaries can never collide (mixing
// two adjacent u64s is otherwise ambiguous with one different u64 pair).
class StateHasher
{
public:
    void mix(std::uint64_t value);
    void mixSigned(std::int64_t value);
    void mixBool(bool value);
    void mixF64(double value); // llround(value * 1000); NaN/inf hash as sentinels (never in a canonical state)
    void mixText(const std::string &text);

    [[nodiscard]] std::uint64_t digest() const noexcept
    {
        return state_;
    }

private:
    std::uint64_t state_ = 14695981039346656037ULL; // FNV-1a 64-bit offset basis
};

// Hashes every persisted store in SaveSources in the schema's order. All
// pointers must be set (the same contract as buildSave).
[[nodiscard]] std::uint64_t canonicalStateHash(const SaveSources &sources);

} // namespace harrowmere::persist

#endif // HARROWMERE_CORE_PERSIST_STATE_HASH_HPP
