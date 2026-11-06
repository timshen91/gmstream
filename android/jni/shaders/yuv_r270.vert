#version 450
// As yuv_r90.vert, turned the other way.
layout(location = 0) out vec2 uv;
void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	uv = vec2(p.y, 1.0 - p.x);
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
