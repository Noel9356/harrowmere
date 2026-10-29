#ifndef ENGINE_MODULES_INPUT_HPP
#define ENGINE_MODULES_INPUT_HPP

#include "input_capability.hpp"
#include "logging_capability.hpp"
#include "lua_bridge_extension.hpp"
#include "ui_capability.hpp"
#include "window_capability.hpp"
#include "engine/kernel/component_interface.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct GLFWwindow;

namespace engine
{

// Module "input_glfw": provides the InputCapability and translates platform keyboard
// callbacks into KeyPressedEvent and KeyReleasedEvent on the event bus. The
// module attaches its callbacks to windows created by the window module, which
// demonstrates one module consuming the capability of another without any
// shared implementation detail.
//
// The module additionally provides the "scripting.lua_bridge.input" Lua bridge
// extension: it installs engine.is_key_down, engine.is_mouse_down,
// engine.consume_mouse_delta, engine.consume_scroll_delta and
// engine.action_strength into every script state through the LuaBridgeExtension
// contract published by lua_scripting. The host discovers this extension
// through the multi-provider acquisition of the "scripting.lua_bridge" family,
// so adding this capability to the module never touches the lua_scripting
// source - the extension ships next to the module whose capability it bridges.
//
// The mouse/scroll delta bridges consult UiCapability::wantsMouse() before
// draining motion: when a debug overlay is up, scripts read settled zero motion
// deterministically (no reliance on event or subscription order between the
// bridge and the overlay's own drain). The UI capability is deliberately NOT
// an optional manifest declaration - the ui module requires this module's
// capability, so an optional edge back to "ui" would close a construct-order
// cycle (renderer -> input -> ui -> renderer) and fail every startup where
// both modules are active. The bridges resolve the capability lazily instead
// (overlayUiCapability), on their first call after every component has
// constructed, and degrade to a no-UI passthrough when no ui module is part
// of the assembly.
class InputModule final : public ModuleInterface, public InputCapability
{
public:
    InputModule();
    ~InputModule() override;

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    bool isKeyDown(KeyCode keyCode) override;
    bool isMouseButtonDown(MouseButton button) override;
    void consumeMouseDelta(double &deltaX, double &deltaY) override;
    void cursorPosition(double &positionX, double &positionY) override;
    void consumeScrollDelta(double &deltaX, double &deltaY) override;
    void setCursorCaptured(bool captured) override;
    bool isCursorCaptured() const override;

    bool isGamepadConnected() override;
    bool isGamepadButtonDown(GamepadButton button) override;
    float gamepadAxis(GamepadAxis axis) override;
    float actionStrength(const std::string &actionName) override;

    // The optional UI capability, lazily resolved (tri-state: undecided /
    // present / absent for the run). The Lua delta bridges call this per
    // delta request; see the uiCapability member for why the resolution
    // cannot happen at construct time. Public because the module-private
    // InputLuaBridge (input.cpp) reads it, not because external consumers
    // are expected to.
    UiCapability *overlayUiCapability();

private:
    static void keyCallback(
        GLFWwindow *window,
        std::int32_t platformKeyCode,
        std::int32_t scanCode,
        std::int32_t action,
        std::int32_t modifiers
    );
    static void cursorPositionCallback(GLFWwindow *window, double x, double y);
    static void scrollCallback(GLFWwindow *window, double x, double y);
    static void characterCallback(GLFWwindow *window, unsigned int codepoint);
    static void gamepadConnectionCallback(int joystickId, int event);

    // Polls the first gamepad; publishes connect/disconnect events on state
    // flips and caches the GLFW state for the query calls.
    bool pollGamepad();

    // The module's GLFWwindow resolved from the front window identifier (the
    // window every query targets). Cached after first resolution: the window
    // set is fixed for the run (windows are created at the window module's
    // start and never removed until teardown), so the per-frame polls
    // (isKeyDown/isMouseButtonDown/cursorPosition, the action map, the UI's
    // keyboard poll) used to pay a fresh identifier-vector allocation plus
    // two map walks per call - hundreds per frame with the overlay up.
    // Invalidated at stop; a null cache resolves lazily on the next query.
    GLFWwindow *primaryWindow();

    ComponentManifest componentManifest;
    ComponentContext *boundContext     = nullptr;
    LoggerCapability *logger           = nullptr;
    WindowCapability *windowCapability = nullptr;
    // The optional UI capability, resolved lazily by overlayUiCapability()
    // on the delta bridges' first call (the ui module always constructs
    // AFTER this module, so a construct-time probe would answer "absent" in
    // every assembly that has a UI module at all): the mouse/scroll delta
    // bridges consult wantsMouse() to settle motion to zero while a debug
    // overlay is up. Null is a valid state - a build without the UI module
    // drains motion unconditionally, exactly the historical behavior.
    UiCapability *uiCapability = nullptr;
    // Tri-state for overlayUiCapability: 0 = undecided (probe on next
    // call), 1 = resolved present, -1 = resolved absent (or permission
    // denied), decided for the run. Reset at stop so a re-constructed
    // module re-resolves against its new assembly.
    int uiResolutionState = 0;

    // Continuous input accumulators: platform callbacks add, frame consumers
    // drain. Guarded by the main-thread-only GLFW contract (callbacks, poll
    // and the frame loop all run on the caller's thread), so no mutex.
    double pendingMouseX  = 0.0;
    double pendingMouseY  = 0.0;
    double pendingScrollX = 0.0;
    double pendingScrollY = 0.0;
    bool cursorCaptured   = false;
    // First cursor callback after attaching has no previous position: the
    // platform hands us an absolute position, not motion.
    bool cursorPositionKnown = false;
    double lastCursorX       = 0.0;
    double lastCursorY       = 0.0;

    // Gamepad state: refreshed on every query (the GLFW gamepad API polls the
    // device synchronously); the connection cache flips publish the bus
    // events. GLFW_GAMEPAD_STATE - the struct lives in glfw3.h, so the
    // member is kept as the raw button/axis arrays to avoid leaking GLFW
    // types through this header.
    bool gamepadConnectedCache = false;
    // Tracks whether start() installed GLFW's global joystick callback: the
    // null-clearing in stop() must not run before glfwInit happened (dry
    // validation tears down instances that never started), or GLFW logs a
    // GLFW_NOT_INITIALIZED error every time.
    bool joystickCallbackInstalled  = false;
    GLFWwindow *cachedPrimaryWindow = nullptr;
    // Publish channels cached at construct (see construct() for why): the
    // event-rate publishes key/text/gamepad events without a per-event
    // type_index lookup. Nulled at stop like the other context-bound state.
    EventChannel<KeyPressedEvent> *keyPressedChannel                   = nullptr;
    EventChannel<KeyReleasedEvent> *keyReleasedChannel                 = nullptr;
    EventChannel<TextInputEvent> *textInputChannel                     = nullptr;
    EventChannel<GamepadConnectedEvent> *gamepadConnectedChannel       = nullptr;
    EventChannel<GamepadDisconnectedEvent> *gamepadDisconnectedChannel = nullptr;
    std::array<float, 6> gamepadAxes{};
    std::array<unsigned char, 15> gamepadButtons{};
    // [input].gamepad_index: which GLFW joystick the capability polls
    // (default 0 = the historical first gamepad). [input].deadzone: the
    // stick rescale floor - axes with |value| below it read exactly 0, the
    // rest remap linearly from the floor to 1.0 (0.0 = the historical raw
    // passthrough).
    std::int32_t gamepadIndex = 0;
    float deadzone            = 0.0f;

    // Action map ([input.actions]): action name -> device bindings.
    struct ActionBinding
    {
        enum class Kind
        {
            Key,
            MouseButton,
            GamepadButton,
            GamepadAxis // analog: triggers contribute their 0..1 value
        };
        Kind kind         = Kind::Key;
        std::int32_t code = 0;
    };
    std::unordered_map<std::string, std::vector<ActionBinding>> actions;
    void loadActionMap();
    // The Lua bridge extension object owned by this module. Constructed in
    // construct() and returned from capabilityPointerFor for the
    // "scripting.lua_bridge.input" member. Held as the base LuaBridgeExtension
    // pointer: the concrete InputLuaBridge type (defined in input.cpp) stays
    // private to this module's translation unit, so the header never names it.
    std::unique_ptr<LuaBridgeExtension> luaBridge;
};

} // namespace engine

#endif // ENGINE_MODULES_INPUT_HPP
