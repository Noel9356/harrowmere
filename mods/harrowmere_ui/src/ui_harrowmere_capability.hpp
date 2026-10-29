#ifndef HARROWMERE_UI_SRC_UI_HARROWMERE_CAPABILITY_HPP
#define HARROWMERE_UI_SRC_UI_HARROWMERE_CAPABILITY_HPP

// The published capability surface of the harrowmere_ui native mod - the same
// provider-owns-its-vocabulary rule the core mod keeps (and the engine
// modules before it). v0.1 is an inert placeholder: the UI mod provides the
// capability so the manifest wiring is real and validated, but nothing
// consumes it until the panels land (7.3P1). Keep this header platform-free
// and dependency-free; the purity guard enforces it mechanically.

namespace harrowmere
{

// Capability "ui.harrowmere": the game's UI surface. Consumers acquire
// through the exact member name; the family ("ui") stays the engine's
// implementation-agnostic demand shape.
class UiHarrowmereCapability
{
public:
    static constexpr const char *capabilityName = "ui.harrowmere";
    virtual ~UiHarrowmereCapability()           = default;
};

} // namespace harrowmere

#endif // HARROWMERE_UI_SRC_UI_HARROWMERE_CAPABILITY_HPP
