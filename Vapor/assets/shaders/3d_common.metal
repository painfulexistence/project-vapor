#include <metal_stdlib>
using namespace metal;

float3x3 inverse(float3x3 const m) {
    float const A = m[1][1] * m[2][2] - m[2][1] * m[1][2];
    float const B = -(m[0][1] * m[2][2] - m[2][1] * m[0][2]);
    float const C = m[0][1] * m[1][2] - m[1][1] * m[0][2];
    float const D = -(m[1][0] * m[2][2] - m[2][0] * m[1][2]);
    float const E = m[0][0] * m[2][2] - m[2][0] * m[0][2];
    float const F = -(m[0][0] * m[1][2] - m[1][0] * m[0][2]);
    float const G = m[1][0] * m[2][1] - m[2][0] * m[1][1];
    float const H = -(m[0][0] * m[2][1] - m[2][0] * m[0][1]);
    float const I = m[0][0] * m[1][1] - m[1][0] * m[0][1];

    float const det = m[0][0] * A + m[1][0] * B + m[2][0] * C;
    float const invDet = 1.f / det;
    return invDet * float3x3{
        float3(A, B, C),
        float3(D, E, F),
        float3(G, H, I)
    };
}