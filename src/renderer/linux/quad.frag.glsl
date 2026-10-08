#version 330 core
in vec2 uv; out vec4 fragColor;
uniform sampler2D image; uniform sampler1D gammaRamp;
uniform vec4 color; uniform int mode; uniform int present; uniform int fxaa;
/* FXAA (after Timothy Lottes' FXAA 3.11, console variant): edges found from luma, blurred along their direction,
 * in the presented image's own pixels (before scaling to the window and the gamma ramp) */
vec3 fxaa_rgb(vec2 p) {
 vec2 px=1.0/vec2(textureSize(image,0)); const vec3 L=vec3(0.299,0.587,0.114);
 vec3 nw=texture(image,p+vec2(-1.0,-1.0)*px).rgb, ne=texture(image,p+vec2(1.0,-1.0)*px).rgb;
 vec3 sw=texture(image,p+vec2(-1.0,1.0)*px).rgb, se=texture(image,p+vec2(1.0,1.0)*px).rgb, m=texture(image,p).rgb;
 float lnw=dot(nw,L), lne=dot(ne,L), lsw=dot(sw,L), lse=dot(se,L), lm=dot(m,L);
 float lmin=min(lm,min(min(lnw,lne),min(lsw,lse))), lmax=max(lm,max(max(lnw,lne),max(lsw,lse)));
 if(lmax-lmin<max(0.0312,lmax*0.125)) return m;                 /* no edge here */
 vec2 dir=vec2(-((lnw+lne)-(lsw+lse)),(lnw+lsw)-(lne+lse));
 float reduce=max((lnw+lne+lsw+lse)*(0.25/8.0),1.0/128.0), rmin=1.0/(min(abs(dir.x),abs(dir.y))+reduce);
 dir=clamp(dir*rmin,vec2(-8.0),vec2(8.0))*px;
 vec3 a=0.5*(texture(image,p+dir*(1.0/3.0-0.5)).rgb+texture(image,p+dir*(2.0/3.0-0.5)).rgb);
 vec3 b=a*0.5+0.25*(texture(image,p-dir*0.5).rgb+texture(image,p+dir*0.5).rgb);
 float lb=dot(b,L); return (lb<lmin || lb>lmax) ? a : b;
}
void main() {
 vec4 c=mode==1 ? color : texture(image,uv);
 if(present!=0 && fxaa!=0) c.rgb=fxaa_rgb(uv);
 if(present!=0) { ivec3 i=ivec3(clamp(c.rgb,0.0,1.0)*255.0+0.5); c=vec4(texelFetch(gammaRamp,i.r,0).r,texelFetch(gammaRamp,i.g,0).g,texelFetch(gammaRamp,i.b,0).b,1.0); }
 fragColor=c;
}
