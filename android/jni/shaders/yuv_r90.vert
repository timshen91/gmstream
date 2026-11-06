#version 450
// As yuv.vert, for a buffer in the panel's own (portrait) orientation: the
// picture turned a quarter, so the display shows it without rotating it.
layout(location = 0) out vec2 uv;
void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	uv = vec2(1.0 - p.y, p.x);
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
