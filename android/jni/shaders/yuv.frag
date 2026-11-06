#version 450
// Riptide's planes (YCbCr 4:2:0, BT.709, full range) to RGB.
layout(set = 0, binding = 0) uniform sampler2D Y;
layout(set = 0, binding = 1) uniform sampler2D Cb;
layout(set = 0, binding = 2) uniform sampler2D Cr;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main()
{
	float y = texture(Y, uv).r;
	float cb = texture(Cb, uv).r - 0.5;
	float cr = texture(Cr, uv).r - 0.5;
	o = vec4(y + 1.5748 * cr, y - 0.187324 * cb - 0.468124 * cr, y + 1.8556 * cb, 1.0);
}
