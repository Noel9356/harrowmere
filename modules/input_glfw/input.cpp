#include "input.hpp"

#include "window_capability.hpp"

#include <GLFW/glfw3.h>
#include <sol/sol.hpp>

#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace engine
{

namespace
{

constexpr std::int32_t pressedAction  = GLFW_PRESS;
constexpr std::int32_t releasedAction = GLFW_RELEASE;

// keyCodeName/keyCodeFromName are published with the input capability header
// (input_capability.hpp) as inline functions: capability metadata lives with
// the capability, and every consumer binary embeds the table it uses, so no
// plugin depends on another plugin's symbols at load time.

// The dedicated Lua bridge extension object for the input module. Owns the
// sol2 wiring that installs engine.is_key_down, engine.is_mouse_down,
// engine.consume_mouse_delta, engine.consume_scroll_delta and
// engine.action_strength into every script state. Holds a raw pointer to the
// owning InputModule: the capability calls go through it, and the
// mouse/scroll delta bridges read the optional UiCapability through the
// module's lazy tri-state resolver (the ui module constructs after input, so
// the pointer cannot be captured once at construction time).
class InputLuaBridge final : public LuaBridgeExtension
{
public:
    explicit InputLuaBridge(
        InputModule *ownerModule
    )
        : module(ownerModule)
    {}

    const char *extensionName() const override
    {
        return "input";
    }

    void installLuaBridge(
        luaState *lua
    ) override
    {
        // The host (lua_scripting) guarantees the engine table exists when
        // this is called. The bridge functions call the InputCapability
        // methods directly through the captured pointer - no capability
        // acquisition, no permission check: the bridge is owned by the module
        // whose capability it exposes, so the call is an in-module dispatch.
        sol::state_view luaView(lua);
        sol::table engine = luaView["engine"];

        engine.set_function("is_key_down", [this](const std::string &keyName) {
            return module->isKeyDown(keyCodeFromName(keyName));
        });
        engine.set_function("is_mouse_down", [this](const std::string &buttonName) {
            if (buttonName == "left")
            {
                return module->isMouseButtonDown(MouseButton::Left);
            }
            if (buttonName == "right")
            {
                return module->isMouseButtonDown(MouseButton::Right);
            }
            if (buttonName == "middle")
            {
                return module->isMouseButtonDown(MouseButton::Middle);
            }
            return false;
        });
        // consume_mouse_delta / consume_scroll_delta: settled to (0, 0) while
        // a UI overlay claims the pointer (UiCapability::wantsMouse), so
        // scripts read zero motion deterministically while a debug window is
        // up. The historical behavior is preserved when no UI module is
        // present (the lazy resolver answers null and the motion drains
        // unconditionally).
        engine.set_function("consume_mouse_delta", [this]() {
            double x               = 0.0;
            double y               = 0.0;
            const UiCapability *ui = module->overlayUiCapability();
            if (ui != nullptr && ui->wantsMouse())
            {
                // The overlay owns the pointer, but the accumulators
                // still fill from the callbacks: drain and discard, or
                // the deltas pile up while hidden and burst out the
                // moment the overlay closes.
                module->consumeMouseDelta(x, y);
                return std::make_tuple(0.0, 0.0); // motion consumed by the overlay
            }
            module->consumeMouseDelta(x, y);
            return std::make_tuple(x, y);
        });
        engine.set_function("consume_scroll_delta", [this]() {
            double x               = 0.0;
            double y               = 0.0;
            const UiCapability *ui = module->overlayUiCapability();
            if (ui != nullptr && ui->wantsMouse())
            {
                // Same drain-and-discard as consume_mouse_delta: hidden
                // must not mean accumulating.
                module->consumeScrollDelta(x, y);
                return std::make_tuple(0.0, 0.0);
            }
            module->consumeScrollDelta(x, y);
            return std::make_tuple(x, y);
        });
        engine.set_function("action_strength", [this](const std::string &actionName) {
            return module->actionStrength(actionName);
        });
    }

private:
    InputModule *module;
};

} // namespace

InputModule::InputModule()
{
    componentManifest = ComponentManifest{
        .name    = "input",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        // The implementation-bound member name: this module is the GLFW
        // implementation of the "input" family (see window.glfw).
        .providedCapabilities
        = {"input.glfw",
            // The Lua bridge extension member: declares this module as a
            // provider of the "scripting.lua_bridge" family. The
            // lua_scripting host acquires every active member through
            // ComponentContext::acquireEachCapability and asks each one
            // to install its own bindings into the host's engine table.
            "scripting.lua_bridge.input"},
        .requiredCapabilities = {"logging", "window"},
        // "ui" is deliberately NOT an optional declaration, mirroring the
        // renderer's matching decision: the ui module REQUIRES this module's
        // capability (the overlay reads input state), so an optional edge back
        // to "ui" would close a construct-order cycle (renderer -> input -> ui
        // -> renderer) and fail every startup where both modules are active.
        // The delta bridges re-resolve the ui capability tri-state on first
        // use (overlayUiCapability), when every component has constructed,
        // which is why the permission stays hand-rolled here.
        .optionalCapabilities = {},
        .requiredComponents   = {},
        .requiredPermissions  = {"capability.logging", "capability.window", "capability.ui"},
        // The action map's keys are authored freely by the assembly (the array's
        // path is what the known-key registry sees; rows are dynamic data).
        // The [input.actions] sub-table is a free-form map (action names are
        // user-authored, read by engine.action_strength), so the known-key
        // registry covers it with a wildcard; the fixed [input] keys above stay
        // exact (docs/configuration.md, the unknown-key warnings).
        .knownConfigKeys = {"input.actions.*", "input.gamepad_index", "input.deadzone"},
    };
}

InputModule::~InputModule()
{
    stop();
}

void InputModule::construct(
    ComponentContext &context
)
{
    boundContext     = &context;
    logger           = &context.acquireCapability<LoggerCapability>();
    windowCapability = &context.acquireCapability<WindowCapability>();

    // The optional UI capability is NOT acquired at construct: the ui module
    // requires this module, so it always constructs AFTER input and a
    // construct-time probe would answer "absent" for every assembly that has
    // a UI module at all. The delta bridges call overlayUiCapability()
    // instead, which re-resolves tri-state on first use - by then every
    // component has constructed, so the probe is final for the run (the same
    // discipline the renderer's camera integration applies to the same
    // capability). Null is a valid state - a build without the UI module
    // drains motion unconditionally, exactly the historical behavior.

    // Which physical gamepad the capability polls (0 = the historical first
    // device); the clamp keeps a typo inside GLFW's joystick range instead of
    // silently polling nothing.
    gamepadIndex = static_cast<std::int32_t>(
        std::clamp<std::int64_t>(context.configurationInteger("gamepad_index").value_or(0), 0, 15)
    );
    // Stick rescale floor: 0.0 (the default) is the exact historical raw
    // passthrough; a configured floor kills drift around the stick's rest
    // position without touching the endpoints (1.0 stays 1.0).
    deadzone = static_cast<float>(std::clamp(context.configurationFloating("deadzone").value_or(0.0), 0.0, 0.9));
    loadActionMap();

    // Cache the publish channels once (the bus anchors channels on the heap,
    // so the references outlive construct): the event-rate publishes below
    // used to go through EventBus::publish<T>, which re-hashes the event's
    // type_index per event - key repeats and typing bursts paid the lookup
    // per event instead of zero, like every cached-channel module.
    EventBus &bus              = context.eventBus();
    keyPressedChannel          = &bus.channel<KeyPressedEvent>();
    keyReleasedChannel         = &bus.channel<KeyReleasedEvent>();
    textInputChannel           = &bus.channel<TextInputEvent>();
    gamepadConnectedChannel    = &bus.channel<GamepadConnectedEvent>();
    gamepadDisconnectedChannel = &bus.channel<GamepadDisconnectedEvent>();

    // Construct the Lua bridge extension object. The module IS the
    // InputCapability, so `this` is the capability pointer; the bridge reads
    // the optional UI capability through the module's overlayUiCapability()
    // per delta call, so it settles motion to zero as soon as the (later
    // constructed) overlay exists.
    luaBridge = std::make_unique<InputLuaBridge>(this);
}

void InputModule::start()
{
    // Read-only borrow of the provider's cached list (see the capability
    // header); the startup copy bought nothing.
    const std::vector<WindowIdentifier> &identifiers = windowCapability->windowIdentifiers();
    if (identifiers.empty())
    {
        logger->logMessage(LogLevel::Warning, "input module started without any window to attach to");
        return;
    }
    for (WindowIdentifier identifier : identifiers)
    {
        GLFWwindow *window = static_cast<GLFWwindow *>(windowCapability->nativeWindowHandle(identifier));
        if (window != nullptr)
        {
            // The input module owns the GLFW window user pointer: GLFW offers
            // exactly one per window and the window module never uses it.
            glfwSetWindowUserPointer(window, this);
            glfwSetKeyCallback(window, &InputModule::keyCallback);
            glfwSetCursorPosCallback(window, &InputModule::cursorPositionCallback);
            glfwSetScrollCallback(window, &InputModule::scrollCallback);
            glfwSetCharCallback(window, &InputModule::characterCallback);
        }
    }
    cursorPositionKnown = false;
    // Gamepad hot-plug events fire through GLFW's global callback (no window
    // involved); the first poll seeds the connection cache.
    glfwSetJoystickCallback(&InputModule::gamepadConnectionCallback);
    joystickCallbackInstalled = true;
    if (pollGamepad())
    {
        logger->logMessage(LogLevel::Information, "input module started (gamepad connected)");
    } else
    {
        logger->logMessage(LogLevel::Information, "input module started");
    }
}

void InputModule::stop()
{
    // stop must tolerate being reached before construct (dry validation
    // creates instances only to read their manifests) and after a previous
    // stop (kernel teardown destroys instances that were never started).
    if (windowCapability != nullptr)
    {
        for (WindowIdentifier identifier : windowCapability->windowIdentifiers())
        {
            GLFWwindow *window = static_cast<GLFWwindow *>(windowCapability->nativeWindowHandle(identifier));
            if (window != nullptr)
            {
                glfwSetKeyCallback(window, nullptr);
                glfwSetCursorPosCallback(window, nullptr);
                glfwSetScrollCallback(window, nullptr);
                glfwSetCharCallback(window, nullptr);
                // Leave a released cursor behind: a teardown that keeps the
                // window alive must not strand the user's pointer.
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                glfwSetWindowUserPointer(window, nullptr);
            }
        }
    }
    pendingMouseX       = 0.0;
    pendingMouseY       = 0.0;
    pendingScrollX      = 0.0;
    pendingScrollY      = 0.0;
    cursorCaptured      = false;
    cursorPositionKnown = false;
    cachedPrimaryWindow = nullptr;
    // The null-clear needs a live GLFW: start() may never have run (dry
    // validation, or a start that bailed before the callback install), and
    // calling into GLFW then just logs GLFW_NOT_INITIALIZED noise.
    if (joystickCallbackInstalled)
    {
        glfwSetJoystickCallback(nullptr);
        joystickCallbackInstalled = false;
    }
    gamepadConnectedCache = false;
    actions.clear();
    // Release the cached publish channels with the rest of the context-bound
    // state (the channels die with the bus during kernel teardown).
    keyPressedChannel          = nullptr;
    keyReleasedChannel         = nullptr;
    textInputChannel           = nullptr;
    gamepadConnectedChannel    = nullptr;
    gamepadDisconnectedChannel = nullptr;
    // Reset the Lua bridge before nulling the capability pointers it captures.
    luaBridge.reset();
    boundContext     = nullptr;
    windowCapability = nullptr;
    uiCapability     = nullptr;
    // Fresh tri-state: a re-constructed module re-resolves the overlay
    // capability against its new assembly instead of inheriting the
    // previous run's decision.
    uiResolutionState = 0;
    logger            = nullptr;
}

UiCapability *InputModule::overlayUiCapability()
{
    // Lazy tri-state resolution of the optional UI capability, evaluated on
    // the delta bridges' first call after every component has constructed:
    // 0 = undecided (probe the context now), 1 = present, -1 = absent (or
    // permission denied), final for the run. The ui module always constructs
    // AFTER this module (it requires the input capability), so no
    // construct-time probe can ever see it - the decision has to wait for
    // the first consumer call. The probe keeps the same absent-means-null
    // contract the construct-time code had; the permission grant is the
    // manifest's hand-rolled "capability.ui" entry.
    if (uiCapability != nullptr)
    {
        return uiCapability;
    }
    if (uiResolutionState == 0 && boundContext != nullptr)
    {
        if (boundContext->isCapabilityAvailable("ui"))
        {
            try
            {
                uiCapability      = &boundContext->acquireCapability<UiCapability>();
                uiResolutionState = 1;
            } catch (const std::exception &)
            {
                uiCapability      = nullptr;
                uiResolutionState = -1; // permission denied (or provider broke its contract)
            }
        } else
        {
            uiResolutionState = -1; // no ui module in this assembly
        }
    }
    return uiCapability;
}

void *InputModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    if (capabilityName == "input.glfw")
    {
        return static_cast<InputCapability *>(this);
    }
    // The "scripting.lua_bridge.input" member: returns the dedicated
    // InputLuaBridge object this module owns (constructed in construct()).
    if (capabilityName == "scripting.lua_bridge.input")
    {
        return luaBridge.get();
    }
    return nullptr;
}

GLFWwindow *InputModule::primaryWindow()
{
    if (cachedPrimaryWindow != nullptr)
    {
        return cachedPrimaryWindow;
    }
    if (windowCapability == nullptr)
    {
        return nullptr;
    }
    const std::vector<WindowIdentifier> &identifiers = windowCapability->windowIdentifiers();
    if (identifiers.empty())
    {
        return nullptr;
    }
    cachedPrimaryWindow = static_cast<GLFWwindow *>(windowCapability->nativeWindowHandle(identifiers.front()));
    return cachedPrimaryWindow;
}

bool InputModule::isKeyDown(
    KeyCode keyCode
)
{
    GLFWwindow *window = primaryWindow();
    if (window == nullptr)
    {
        return false;
    }
    return glfwGetKey(window, static_cast<int>(keyCode)) == GLFW_PRESS;
}

bool InputModule::isMouseButtonDown(
    MouseButton button
)
{
    GLFWwindow *window = primaryWindow();
    if (window == nullptr)
    {
        return false;
    }
    return glfwGetMouseButton(window, static_cast<int>(button)) == GLFW_PRESS;
}

void InputModule::consumeMouseDelta(
    double &deltaX, double &deltaY
)
{
    deltaX        = pendingMouseX;
    deltaY        = pendingMouseY;
    pendingMouseX = 0.0;
    pendingMouseY = 0.0;
}

void InputModule::cursorPosition(
    double &positionX, double &positionY
)
{
    // Queried straight from the platform: absolute position is state, not an
    // accumulator, so a poll here cannot race the delta consumers above.
    positionX          = 0.0;
    positionY          = 0.0;
    GLFWwindow *window = primaryWindow();
    if (window == nullptr)
    {
        return;
    }
    glfwGetCursorPos(window, &positionX, &positionY);
}

void InputModule::consumeScrollDelta(
    double &deltaX, double &deltaY
)
{
    deltaX         = pendingScrollX;
    deltaY         = pendingScrollY;
    pendingScrollX = 0.0;
    pendingScrollY = 0.0;
}

void InputModule::setCursorCaptured(
    bool captured
)
{
    if (windowCapability == nullptr)
    {
        return;
    }
    const std::vector<WindowIdentifier> &identifiers = windowCapability->windowIdentifiers();
    if (identifiers.empty())
    {
        return;
    }
    GLFWwindow *window = static_cast<GLFWwindow *>(windowCapability->nativeWindowHandle(identifiers.front()));
    if (window == nullptr || captured == cursorCaptured)
    {
        return;
    }
    // Disabled hides and locks the cursor; raw motion (when the platform
    // supports it) reports unaccelerated deltas, which is what a fly camera
    // wants. The pending delta is cleared around the transition: mode changes
    // can emit a cursor position event with a large jump.
    glfwSetInputMode(window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (glfwRawMouseMotionSupported() == GLFW_TRUE)
    {
        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
    }
    pendingMouseX       = 0.0;
    pendingMouseY       = 0.0;
    cursorPositionKnown = false;
    cursorCaptured      = captured;
}

bool InputModule::isCursorCaptured() const
{
    return cursorCaptured;
}

bool InputModule::isGamepadConnected()
{
    return pollGamepad();
}

bool InputModule::isGamepadButtonDown(
    GamepadButton button
)
{
    if (!pollGamepad())
    {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(button);
    return index < gamepadButtons.size() && gamepadButtons[index] != 0;
}

float InputModule::gamepadAxis(
    GamepadAxis axis
)
{
    if (!pollGamepad())
    {
        return 0.0f;
    }
    const std::size_t index = static_cast<std::size_t>(axis);
    return index < gamepadAxes.size() ? gamepadAxes[index] : 0.0f;
}

float InputModule::actionStrength(
    const std::string &actionName
)
{
    const auto found = actions.find(actionName);
    if (found == actions.end())
    {
        return 0.0f; // an unconfigured action reads as "not pressed"
    }
    // ONE gamepad poll for the whole action - and only when the action
    // actually binds a gamepad control: keyboard and mouse candidates read
    // the window state, never the cached gamepad arrays, so an action with
    // no gamepad binding (the whole fly-camera action map on a keyboard
    // run - held() routes through here several times per frame) pays zero
    // device polls. The previous shape polled glfwGetGamepadState (plus the
    // six-axis deadzone rescale) once per QUERY regardless of what the
    // action bound. Gamepad-bound actions keep the poll: the loop below
    // reads the state pollGamepad() cached into gamepadAxes/gamepadButtons,
    // and the connection-transition events (GamepadConnectedEvent /
    // GamepadDisconnectedEvent) fire from the same poll they always did.
    bool bindsGamepad = false;
    for (const ActionBinding &binding : found->second)
    {
        if (binding.kind == ActionBinding::Kind::GamepadButton || binding.kind == ActionBinding::Kind::GamepadAxis)
        {
            bindsGamepad = true;
            break;
        }
    }
    const bool gamepadLive = bindsGamepad && pollGamepad();
    float strength         = 0.0f;
    for (const ActionBinding &binding : found->second)
    {
        float candidate = 0.0f;
        switch (binding.kind)
        {
        case ActionBinding::Kind::Key:
            candidate = isKeyDown(static_cast<KeyCode>(binding.code)) ? 1.0f : 0.0f;
            break;
        case ActionBinding::Kind::MouseButton:
            candidate = isMouseButtonDown(static_cast<MouseButton>(binding.code)) ? 1.0f : 0.0f;
            break;
        case ActionBinding::Kind::GamepadButton: {
            if (gamepadLive)
            {
                const std::size_t index = static_cast<std::size_t>(binding.code);
                candidate               = index < gamepadButtons.size() && gamepadButtons[index] != 0 ? 1.0f : 0.0f;
            }
            break;
        }
        case ActionBinding::Kind::GamepadAxis: {
            if (gamepadLive)
            {
                const std::size_t index = static_cast<std::size_t>(binding.code);
                candidate = std::clamp(index < gamepadAxes.size() ? gamepadAxes[index] : 0.0f, 0.0f, 1.0f);
            }
            break;
        }
        }
        strength = std::max(strength, candidate);
    }
    return strength;
}

void InputModule::loadActionMap()
{
    if (boundContext == nullptr)
    {
        return;
    }
    const std::vector<std::string> actionNames = boundContext->configurationTableKeysInSection("input", "actions");
    for (const std::string &actionName : actionNames)
    {
        const std::string value
            = boundContext->configurationStringInSection("input", "actions." + actionName).value_or("");
        std::vector<ActionBinding> bindings;
        // One action = one comma-separated binding string ("Space, gamepad:A").
        std::size_t position = 0;
        while (position < value.size())
        {
            std::size_t comma = value.find(',', position);
            if (comma == std::string::npos)
            {
                comma = value.size();
            }
            std::string token = value.substr(position, comma - position);
            position          = comma + 1;
            // Trim.
            const auto isSpace = [](char character) { return character == ' ' || character == '\t'; };
            while (!token.empty() && isSpace(token.front()))
            {
                token.erase(token.begin());
            }
            while (!token.empty() && isSpace(token.back()))
            {
                token.pop_back();
            }
            if (token.empty())
            {
                continue;
            }
            ActionBinding binding;
            if (token.rfind("gamepad:", 0) == 0)
            {
                std::int32_t code = -1;
                bool isButton     = false;
                if (gamepadControlFromName(token.substr(8), code, isButton))
                {
                    binding.kind = isButton ? ActionBinding::Kind::GamepadButton : ActionBinding::Kind::GamepadAxis;
                    binding.code = code;
                    bindings.push_back(binding);
                } else if (logger != nullptr)
                {
                    logger->logMessage(
                        LogLevel::Warning,
                        "input module: action '" + actionName + "' names unknown gamepad control '" + token.substr(8)
                            + "'"
                    );
                }
            } else if (token.rfind("mouse:", 0) == 0)
            {
                const std::string buttonName = token.substr(6);
                // The three named buttons plus the platform's extra buttons
                // (GLFW exposes up to eight; 4..7 are the typical
                // back/forward pair and whatever the device offers beyond).
                // Numeric names keep the mapping honest: which physical
                // button a number lands on is device-specific, so the
                // stable names stay preferred in configurations.
                if (buttonName == "left")
                {
                    binding.kind = ActionBinding::Kind::MouseButton;
                    binding.code = static_cast<std::int32_t>(MouseButton::Left);
                    bindings.push_back(binding);
                } else if (buttonName == "right")
                {
                    binding.kind = ActionBinding::Kind::MouseButton;
                    binding.code = static_cast<std::int32_t>(MouseButton::Right);
                    bindings.push_back(binding);
                } else if (buttonName == "middle")
                {
                    binding.kind = ActionBinding::Kind::MouseButton;
                    binding.code = static_cast<std::int32_t>(MouseButton::Middle);
                    bindings.push_back(binding);
                } else if (buttonName.size() == 1 && buttonName[0] >= '4' && buttonName[0] <= '7')
                {
                    binding.kind = ActionBinding::Kind::MouseButton;
                    binding.code = static_cast<std::int32_t>(buttonName[0] - '0');
                    bindings.push_back(binding);
                } else if (logger != nullptr)
                {
                    logger->logMessage(
                        LogLevel::Warning,
                        "input module: action '" + actionName + "' names unknown mouse button '" + buttonName + "'"
                    );
                }
            } else
            {
                const KeyCode keyCode = keyCodeFromName(token);
                if (keyCode != KeyCode::Unknown)
                {
                    binding.kind = ActionBinding::Kind::Key;
                    binding.code = static_cast<std::int32_t>(keyCode);
                    bindings.push_back(binding);
                } else if (logger != nullptr)
                {
                    logger->logMessage(
                        LogLevel::Warning, "input module: action '" + actionName + "' names unknown key '" + token + "'"
                    );
                }
            }
        }
        if (bindings.empty())
        {
            if (logger != nullptr)
            {
                logger->logMessage(
                    LogLevel::Warning,
                    "input module: action '" + actionName + "' resolved to no usable binding; it always reads 0"
                );
            }
            continue;
        }
        actions.emplace(actionName, std::move(bindings));
    }
    if (!actions.empty() && logger != nullptr)
    {
        std::string names;
        for (const auto &entry : actions)
        {
            if (!names.empty())
            {
                names += ", ";
            }
            names += entry.first;
        }
        logger->logMessage(LogLevel::Information, "input module: actions mapped: " + names);
    }
}

bool InputModule::pollGamepad()
{
    if (windowCapability == nullptr)
    {
        return false;
    }
    GLFWgamepadstate state;
    const bool connected = glfwGetGamepadState(gamepadIndex, &state) == GLFW_TRUE;
    if (connected)
    {
        for (std::size_t axis = 0; axis < gamepadAxes.size(); ++axis)
        {
            float axisValue = state.axes[axis];
            // Deadzone rescale: below the floor reads exactly 0 (stick drift
            // and resting sticks stop leaking into actions), above it the
            // value remaps linearly from [floor, 1] to [0, 1] so the response
            // stays continuous and the endpoints do not move. A 0.0 floor -
            // the default - passes every value through untouched.
            if (deadzone > 0.0f)
            {
                const float magnitude = std::abs(axisValue);
                if (magnitude <= deadzone)
                {
                    axisValue = 0.0f;
                } else
                {
                    // Some devices report |axis| slightly above 1.0, which
                    // the rescale maps past the endpoint; the clamp keeps
                    // every published value inside [-1, 1].
                    axisValue = std::clamp(
                        (axisValue < 0.0f ? -1.0f : 1.0f) * ((magnitude - deadzone) / (1.0f - deadzone)), -1.0f, 1.0f
                    );
                }
            }
            gamepadAxes[axis] = axisValue;
        }
        for (std::size_t button = 0; button < gamepadButtons.size(); ++button)
        {
            gamepadButtons[button] = state.buttons[button];
        }
    }
    if (connected != gamepadConnectedCache && boundContext != nullptr)
    {
        gamepadConnectedCache = connected;
        if (connected)
        {
            gamepadConnectedChannel->publish(GamepadConnectedEvent{});
        } else
        {
            gamepadDisconnectedChannel->publish(GamepadDisconnectedEvent{});
        }
    }
    return connected;
}

void InputModule::characterCallback(
    GLFWwindow *window, unsigned int codepoint
)
{
    InputModule *module = static_cast<InputModule *>(glfwGetWindowUserPointer(window));
    if (module == nullptr || module->boundContext == nullptr)
    {
        return;
    }
    TextInputEvent event;
    event.codepoint = codepoint;
    module->textInputChannel->publish(event);
}

void InputModule::gamepadConnectionCallback(
    int /*joystickId*/, int /*event*/
)
{
    // The per-query poll publishes the connect/disconnect events (it has the
    // module pointer); this callback only exists so GLFW sees a listener and
    // keeps its internal state warm. State is re-polled lazily.
}

void InputModule::cursorPositionCallback(
    GLFWwindow *window, double x, double y
)
{
    InputModule *module = static_cast<InputModule *>(glfwGetWindowUserPointer(window));
    if (module == nullptr)
    {
        return;
    }
    if (!module->cursorPositionKnown)
    {
        // Absolute position without a previous one: not motion.
        module->cursorPositionKnown = true;
        module->lastCursorX         = x;
        module->lastCursorY         = y;
        return;
    }
    module->pendingMouseX += x - module->lastCursorX;
    module->pendingMouseY += y - module->lastCursorY;
    module->lastCursorX = x;
    module->lastCursorY = y;
}

void InputModule::scrollCallback(
    GLFWwindow *window, double x, double y
)
{
    InputModule *module = static_cast<InputModule *>(glfwGetWindowUserPointer(window));
    if (module == nullptr)
    {
        return;
    }
    module->pendingScrollX += x;
    module->pendingScrollY += y;
}

void InputModule::keyCallback(
    GLFWwindow *window, std::int32_t platformKeyCode, std::int32_t scanCode, std::int32_t action, std::int32_t modifiers
)
{
    (void)scanCode;
    (void)modifiers;

    InputModule *module = static_cast<InputModule *>(glfwGetWindowUserPointer(window));
    if (module == nullptr || module->boundContext == nullptr)
    {
        return;
    }

    const KeyCode keyCode = static_cast<KeyCode>(platformKeyCode);
    if (action == pressedAction)
    {
        KeyPressedEvent event;
        event.keyCode = keyCode;
        module->keyPressedChannel->publish(event);
    } else if (action == releasedAction)
    {
        KeyReleasedEvent event;
        event.keyCode = keyCode;
        module->keyReleasedChannel->publish(event);
    }
}

} // namespace engine
