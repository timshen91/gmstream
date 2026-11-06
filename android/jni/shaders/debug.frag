#version 450
// Bring-up: the planes as they are (Y as red, Cb green, Cr blue) over a uv ramp.
layout(set = 0, binding = 0) uniform sampler2D Y;
layout(set = 0, binding = 1) uniform sampler2D Cb;
layout(set = 0, binding = 2) uniform sampler2D Cr;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main()
{
	if (uv.x < 0.5)
		o = vec4(uv, 0.5, 1.0);
	else
		o = vec4(texture(Y, uv).r, texture(Cb, uv).r, texture(Cr, uv).r, 1.0);
}
