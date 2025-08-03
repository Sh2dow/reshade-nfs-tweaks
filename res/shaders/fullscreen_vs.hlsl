// SM 3.0-compatible full-screen triangle vertex shader

struct VS_INPUT
{
    float id : POSITION; // You'll need to pass vertex IDs 0, 1, 2 manually in the vertex buffer
};

struct VS_OUTPUT
{
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

VS_OUTPUT main(VS_INPUT IN)
{
    VS_OUTPUT OUT;
    float2 uv;

    int id = (int)IN.id;

    uv.x = (id == 1) ? 2.0 : 0.0;
    uv.y = (id == 2) ? 2.0 : 0.0;

    OUT.uv = uv;
    OUT.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return OUT;
}
