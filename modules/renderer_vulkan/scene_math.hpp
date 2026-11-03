#ifndef ENGINE_MODULES_RENDERER_SCENE_MATH_HPP
#define ENGINE_MODULES_RENDERER_SCENE_MATH_HPP

// Scene-side camera and object transform math for the renderer, on GLM
// (OpenGL Mathematics). The renderer needs perspective, lookAt, multiply and
// inverse - exactly the standard vocabulary GLM provides - and the storage
// layout (glm::mat4, column-major float[16]) is what the shaders consume, so
// no conversion step and no second source of truth. GLM is configured once,
// tree-wide, through the engine_glm_bindings CMake wrapper
// (GLM_FORCE_DEPTH_ZERO_TO_ONE: Vulkan clip space, z in [0, 1]) - a macro
// like that must hold for every translation unit or inline functions would
// have two different definitions across a binary.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <array>
#include <cmath>

namespace engine
{

// Shared angle constants: one definition because the C++ sites previously
// inlined the literals at two different precisions.
inline constexpr float pi    = 3.14159265358979323846f;
inline constexpr float twoPi = 2.0f * pi;

inline constexpr float degreesToRadians(
    float degrees
)
{
    return degrees * (pi / 180.0f);
}

using Mat4 = glm::mat4; // column major: m[col][row], float[16] in memory

inline Mat4 multiplyMatrix(
    const Mat4 &left, const Mat4 &right
)
{
    return left * right;
}

// Vulkan convention: depth maps to [0, 1] (GLM_FORCE_DEPTH_ZERO_TO_ONE covers
// that), but clip-space Y still points DOWN. GLM builds OpenGL-style Y-up
// projections: the macro changes the depth range only - [1][1] stays positive,
// and the uncorrected matrix renders vertically mirrored under Vulkan. The
// wrapper negates the focal term itself. The Y-flip also reverses triangle
// winding, which is harmless here: every renderer pipeline runs with
// CullModeFlagBits::eNone (scene, composite and bloom-final rasterization
// states), so no triangle disappears from the flipped winding.
inline Mat4 perspectiveMatrix(
    float verticalFieldOfViewRadians, float aspect, float nearPlane, float farPlane
)
{
    Mat4 projection = glm::perspective(verticalFieldOfViewRadians, aspect, nearPlane, farPlane);
    projection[1][1] *= -1.0f; // Vulkan clip-space Y points down; GLM builds Y-up
    return projection;
}

inline Mat4 lookAtMatrix(
    const float eye[3], const float center[3], const float up[3]
)
{
    return glm::lookAtRH(glm::make_vec3(eye), glm::make_vec3(center), glm::make_vec3(up));
}

// General 4x4 inverse. GLM's computeInverse (Gauss-Jordan, partial pivoting
// in the SSE path, cofactor expansion otherwise) replaces the hand-rolled
// elimination; called twice per frame at most, the cost model is unchanged.
inline Mat4 inverseMatrix(
    const Mat4 &matrix
)
{
    return glm::inverse(matrix);
}

// Translation * rotationY * scale.
inline Mat4 modelMatrix(
    const float position[3], float rotationYRadians, const float scale[3]
)
{
    return glm::translate(glm::mat4{1.0f}, glm::make_vec3(position))
         * glm::rotate(glm::mat4{1.0f}, rotationYRadians, glm::vec3{0.0f, 1.0f, 0.0f})
         * glm::scale(glm::mat4{1.0f}, glm::make_vec3(scale));
}

} // namespace engine

#endif // ENGINE_MODULES_RENDERER_SCENE_MATH_HPP
