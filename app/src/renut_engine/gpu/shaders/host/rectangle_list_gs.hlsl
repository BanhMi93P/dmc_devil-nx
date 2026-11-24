// Xenos rectangle lists: every three vertices are three corners of a screen
// rectangle and the GPU makes the fourth. The host draws them as triangle lists
// through this shader, which emits each rectangle as a two-triangle strip.
//
// The three corners do not come in a fixed order. N&B's D3D writes its own
// (ClearRects_D3D, 0x821D3D20) as (left, top), (right, top), (right, bottom),
// the right angle at the second; N&B's full-screen passes draw (-1, 1),
// (1, 1), (-1, -1), the right angle at the first. So the shader finds the
// corner whose two edges are perpendicular, and the fourth corner is the one
// across the diagonal from it.
//
// The vertex layout is what XenosRecomp (RENUT_RECOMP) writes for every vertex
// shader and pixel shader: the position, then the 25 interpolators in this
// order, then the point sprite coordinate and size (see point_sprite_gs.hlsl).
// SPIR-V matches stages by location, which follows the order.

struct Vertex
{
    float4 position : SV_Position;
    float4 texcoord[16] : TEXCOORD0;
    float4 color[2] : COLOR0;
    float4 normal[3] : NORMAL0;
    float4 tangent : TANGENT0;
    float4 binormal : BINORMAL0;
    float4 fog : FOG0;  // kameo-nx (2026-11-04): see point_sprite_gs.hlsl
    float4 position2 : POSITION2;  // renut-ultimate: see point_sprite_gs.hlsl
    float2 pointCoord : POINTCOORD;
    float pointSize : POINTSIZE;
    // renut-ultimate (the user clip plane, every vertex shader writes it): passed through, so the GPU clips the sprite with it.
    float clipDistance : SV_ClipDistance0;
};

float2 ScreenPosition(Vertex v)
{
    return v.position.xy / v.position.w;
}

// How far the corner at p is from a right angle: 0 when its edges to a and b
// are perpendicular; the other two corners of a rectangle are acute.
float Skew(float2 p, float2 a, float2 b)
{
    return abs(dot(a - p, b - p));
}

// The strip from the right angle C, then A and B, then the corner across the diagonal A-B (a + b - c).
// Written as a macro indexing `input` directly so DXC/SPIRV-Cross do not hoist all 3 Vertex structs
// into live registers before EmitVertex(), which exceeded uam's GS register allocation after SV_ClipDistance0.
#define EMIT_RECTANGLE(C, A, B) \
    do { \
        output.Append(input[C]); \
        output.Append(input[A]); \
        output.Append(input[B]); \
        Vertex d; \
        d.position = input[A].position + input[B].position - input[C].position; \
        [unroll] for (uint i = 0; i < 16; ++i) \
            d.texcoord[i] = input[A].texcoord[i] + input[B].texcoord[i] - input[C].texcoord[i]; \
        [unroll] for (uint j = 0; j < 2; ++j) \
            d.color[j] = input[A].color[j] + input[B].color[j] - input[C].color[j]; \
        [unroll] for (uint k = 0; k < 3; ++k) \
            d.normal[k] = input[A].normal[k] + input[B].normal[k] - input[C].normal[k]; \
        d.tangent = input[A].tangent + input[B].tangent - input[C].tangent; \
        d.binormal = input[A].binormal + input[B].binormal - input[C].binormal; \
        d.fog = input[A].fog + input[B].fog - input[C].fog; \
        d.position2 = input[A].position2 + input[B].position2 - input[C].position2; \
        d.pointCoord = input[A].pointCoord + input[B].pointCoord - input[C].pointCoord; \
        d.pointSize = input[A].pointSize + input[B].pointSize - input[C].pointSize; \
        d.clipDistance = input[A].clipDistance + input[B].clipDistance - input[C].clipDistance; \
        output.Append(d); \
    } while (false)

[maxvertexcount(4)]
void main(triangle Vertex input[3], inout TriangleStream<Vertex> output)
{
    const float2 p0 = ScreenPosition(input[0]);
    const float2 p1 = ScreenPosition(input[1]);
    const float2 p2 = ScreenPosition(input[2]);
    const float s0 = Skew(p0, p1, p2);
    const float s1 = Skew(p1, p2, p0);
    const float s2 = Skew(p2, p0, p1);
    uint corner = 0;
    if (s1 < s0 && s1 <= s2)
        corner = 1;
    else if (s2 < s0 && s2 < s1)
        corner = 2;

    // Starting the strip from the right angle, in the given cyclic order,
    // keeps the winding of the three given vertices for both triangles.
    // Port 164 (from fnxf_xiii, its note of 2026-10-27): each corner by a constant index, one branch per right angle.
    // input[corner] with a run-time index made SPIRV-Cross copy the three vertices into a local array of structs
    // (fnxf_xiii: 52 KB of scratch a warp and 255 registers in uam's GS). The same vertices in the same order.
    if (corner == 0)
        EMIT_RECTANGLE(0, 1, 2);
    else if (corner == 1)
        EMIT_RECTANGLE(1, 2, 0);
    else
        EMIT_RECTANGLE(2, 0, 1);
}
