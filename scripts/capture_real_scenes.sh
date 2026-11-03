#!/usr/bin/env bash
# capture_real_scenes.sh — run the engine with lavapipe + Xvfb, trigger screenshots.
set -euo pipefail

ROOT=/home/z/my-project/harrowmere
OUT=/tmp/real_shots
rm -rf "$OUT"; mkdir -p "$OUT"

# Kill any stale Xvfb and remove the stale socket.
pkill Xvfb 2>/dev/null || true
rm -f /tmp/.X11-unix/X99 2>/dev/null || true
sleep 1

# Start a fresh Xvfb.
Xvfb :99 -screen 0 800x600x24 -ac &
XVFB_PID=$!
sleep 2

# Verify Xvfb is up.
if ! kill -0 $XVFB_PID 2>/dev/null; then
    echo "ERROR: Xvfb failed to start"
    exit 1
fi
echo "Xvfb started (PID=$XVFB_PID)"

# Export the env for all subsequent commands.
export DISPLAY=:99
export GLFW_PLATFORM=x11
export XDG_RUNTIME_DIR=/tmp/xdg-runtime
export VK_ICD_FILENAMES=/tmp/mesa/usr/share/vulkan/icd.d/lvp_icd.json
export VK_DRIVER_FILES=/tmp/mesa/usr/share/vulkan/icd.d/lvp_icd.json
export LD_LIBRARY_PATH=/tmp/mesa/usr/lib/x86_64-linux-gnu:/tmp/xdotool/usr/lib/x86_64-linux-gnu
export PATH=/tmp/xdotool/usr/bin:/tmp/glsl/usr/bin:$PATH
mkdir -p /tmp/xdg-runtime; chmod 700 /tmp/xdg-runtime

# Start the engine in the background. Give lavapipe time to render frames.
echo "starting engine..."
$ROOT/build/runtime/harrowmere \
    --set engine.quit_after_seconds=30.0 \
    --set screenshot.output_directory=$OUT \
    --set screenshot.key=F12 \
    --set window.width=800 \
    --set window.height=600 \
    --set harrowmere.seed=20261103 \
    > /tmp/engine.log 2>&1 &
ENGINE_PID=$!
echo "engine PID=$ENGINE_PID"

# Wait for the engine to create a window and start rendering.
echo "waiting for engine to boot (10s)..."
sleep 10

# Check if the engine is still running.
if ! kill -0 $ENGINE_PID 2>/dev/null; then
    echo "ERROR: engine exited early"
    cat /tmp/engine.log
    exit 1
fi

# Check if the window was created.
if grep -q "created window" /tmp/engine.log; then
    echo "window created — pressing F12 for screenshot"
    xdotool key F12 2>&1 || echo "xdotool F12 failed"
    sleep 3
    echo "pressing F12 again (second screenshot)"
    xdotool key F12 2>&1 || echo "xdotool F12 failed"
    sleep 5
else
    echo "WARNING: no window created — engine degraded to headless"
    head -30 /tmp/engine.log
fi

# Wait for the engine to finish.
echo "waiting for engine to finish..."
wait $ENGINE_PID 2>/dev/null || true

echo "--- screenshots ---"
ls -la $OUT/ 2>&1
echo "--- engine log tail ---"
tail -10 /tmp/engine.log

# Cleanup.
kill $XVFB_PID 2>/dev/null || true
