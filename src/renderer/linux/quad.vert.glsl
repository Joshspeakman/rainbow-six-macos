#version 330 core
uniform vec4 dst; uniform vec4 src; uniform int present;
out vec2 uv;
void main() {
 vec2 c=vec2((gl_VertexID & 1) != 0 ? 1.0:0.0, (gl_VertexID & 2) != 0 ? 1.0:0.0);
 vec2 d=mix(dst.xy,dst.zw,c);
 gl_Position=vec4(d*2.0-1.0,0.0,1.0);
 if(present!=0) gl_Position.y=-gl_Position.y;
 uv=mix(src.xy,src.zw,c);
}
