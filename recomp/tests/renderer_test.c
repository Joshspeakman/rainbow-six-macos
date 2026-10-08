/* Exercise the real OpenGL renderer's color, fog, blending and display path. */
#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../../src/renderer/linux/renderer.h"
#include "../../src/dsr/dsr_proto.h"
static DSRRenderer *r;
static void cmd(unsigned op, const void *p, unsigned n) { dsr_gl_exec(r,op,p,n); }
static void rs(unsigned key,unsigned value) { uint32_t p[]={key,value}; cmd(DSR_RENDER_STATE,p,sizeof p); }
static void tss(unsigned stage,unsigned key,unsigned value) { uint32_t p[]={stage,key,value}; cmd(DSR_TSS,p,sizeof p); }
static void color(unsigned rgba, int rhw) {
 struct V {float x,y,z,w; uint32_t color;} v[4]={{-.5,-.5,.5,1,rgba},{.5,-.5,.5,1,rgba},{-.5,.5,.5,1,rgba},{.5,.5,.5,1,rgba}};
 uint32_t data[4+4*5]={5,rhw?0x44:0x42,4,0};
 if(rhw) {
  for(int i=0;i<4;i++) { v[i].x=(i&1)?31:0; v[i].y=(i&2)?31:0; }
  memcpy(data+4,v,sizeof v); cmd(DSR_DRAW,data,sizeof data);
 } else {
  uint32_t *p=data+4; for(int i=0;i<4;i++) { memcpy(p,&v[i],12); p[3]=rgba; p+=4; }
  cmd(DSR_DRAW,data,16+4*16);
 }
}
static void pixel(const char *name,int red,int green,int blue,int presented) {
 unsigned char px[4];
 if(presented) glReadPixels(16,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
 else {
  volatile uint32_t done=0; uint32_t out=0; dsr_gl_readback(r,&out,4,&done);
  uint32_t p[]={1,16,16,1,1,1}; cmd(DSR_READBACK,p,sizeof p);
  px[0]=out>>16; px[1]=out>>8; px[2]=out;
 }
 printf("%s: %u %u %u (expected %d %d %d)\n",name,px[0],px[1],px[2],red,green,blue);
 assert(abs(px[0]-red)<=2 && abs(px[1]-green)<=2 && abs(px[2]-blue)<=2);
}
static void wide_display(SDL_Window *window,int width,int height) {
 SDL_SetWindowSize(window,width,height);
 /* Mesa refreshes a GLX drawable's backing buffers at swap. Match the host's
  * normal event/swap cycle before reading pixels from the enlarged window. */
 SDL_PumpEvents(); SDL_GL_SwapWindow(window);
 int actual_w,actual_h; SDL_GL_GetDrawableSize(window,&actual_w,&actual_h);
 assert(actual_w==width && actual_h==height);
 uint32_t surface[]={4,width,height,0,1,0,0,1}; cmd(DSR_SURFACE_CREATE,surface,sizeof surface);
 uint32_t rt[]={4,0}; cmd(DSR_SET_RT,rt,sizeof rt);
 uint32_t vp[]={0,0,width,height,0,0x3f800000}; cmd(DSR_VIEWPORT,vp,sizeof vp);
 uint32_t fill[]={1,0xff4080c0,0x3f800000,0,0}; cmd(DSR_CLEAR,fill,sizeof fill);
 uint32_t present[]={4}; cmd(DSR_PRESENT,present,sizeof present); dsr_gl_present(r,width,height);
 unsigned char px[4];
 for(int x=0;x<width;x+=width-1) {
  glReadPixels(x,height/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
  assert(px[0]==64 && px[1]==128 && px[2]==192);
 }
 /* Original 4:3 menus stay proportional on an ultrawide display. */
 surface[1]=800; surface[2]=600; cmd(DSR_SURFACE_CREATE,surface,sizeof surface);
 vp[2]=800; vp[3]=600; cmd(DSR_VIEWPORT,vp,sizeof vp); cmd(DSR_CLEAR,fill,sizeof fill);
 dsr_gl_present(r,width,height);
 GLint viewport[4]; glGetIntegerv(GL_VIEWPORT,viewport);
 assert(viewport[2]==height*4/3 && viewport[3]==height && viewport[0]==(width-height*4/3)/2);
 glReadPixels(0,height/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px); assert(px[0]==0 && px[1]==0 && px[2]==0);
 glReadPixels(width/2,height/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px); assert(px[0]==64 && px[1]==128 && px[2]==192);
 printf("%dx%d full-width presentation and proportional 4:3 menus: passed\n",width,height);
}
int main(int argc,char **argv) {
 setbuf(stdout,NULL);
 assert(argc==2); assert(SDL_Init(SDL_INIT_VIDEO)==0);
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
 SDL_Window *window=SDL_CreateWindow("Renderer test",0,0,32,32,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN); assert(window);
 SDL_GLContext context=SDL_GL_CreateContext(window); assert(context);
 r=dsr_gl_create(argv[1]); assert(r);
 uint32_t surf[]={1,32,32,0,1,0,0,1}; cmd(DSR_SURFACE_CREATE,surf,sizeof surf);
 uint32_t rt[]={1,0}; cmd(DSR_SET_RT,rt,sizeof rt);
 uint32_t vp[]={0,0,32,32,0,0x3f800000}; cmd(DSR_VIEWPORT,vp,sizeof vp);
 rs(22,1); rs(7,0); tss(0,1,2); tss(0,2,0); tss(0,5,0);
 color(0xff4080c0,1); pixel("BGRA vertex color",64,128,192,0);
 color(0xff4080c0,0); pixel("world vertex color",64,128,192,0);
 rs(28,1); rs(140,3); rs(36,0); rs(37,0x3f800000); rs(34,0x00808080);
 color(0xff4080c0,0); pixel("50% vertex fog",96,128,160,0);
 rs(28,0);
 /* The world combines colored vertices with textures and a second light layer. */
 uint32_t texture[]={2,1,1,0,1,0,0,1}; cmd(DSR_SURFACE_CREATE,texture,sizeof texture);
 uint32_t upload[]={2,0,0,1,1,4,0xff804020}; cmd(DSR_SURFACE_UPLOAD,upload,sizeof upload);
 uint32_t binding[]={0,2}; cmd(DSR_TEXTURE,binding,sizeof binding);
 tss(0,1,4); tss(0,2,2); tss(0,3,0);
 color(0xff808080,0); pixel("world texture times vertex lighting",64,32,16,0);
 texture[0]=3; cmd(DSR_SURFACE_CREATE,texture,sizeof texture);
 upload[0]=3; upload[6]=0xff808080; cmd(DSR_SURFACE_UPLOAD,upload,sizeof upload);
 binding[0]=1; binding[1]=3; cmd(DSR_TEXTURE,binding,sizeof binding);
 tss(1,1,4); tss(1,2,2); tss(1,3,1);
 color(0xff808080,0); pixel("second texture lighting",32,16,8,0);
 tss(0,1,5); color(0xff808080,0); pixel("2x modulation",64,32,16,0);
 tss(1,1,1); tss(0,1,2); tss(0,2,0); color(0xff4080c0,1);
 rs(27,1); rs(19,5); rs(20,6); color(0x80c08040,1); pixel("alpha blending",128,128,128,0);
 uint32_t present[]={1}; cmd(DSR_PRESENT,present,sizeof present);
 dsr_gl_present(r,32,32); pixel("identity display gamma",128,128,128,1);
 uint16_t gamma[3][256]; for(int k=0;k<3;k++) for(int i=0;i<256;i++) gamma[k][i]=i*128;
 cmd(DSR_GAMMA,gamma,sizeof gamma); dsr_gl_present(r,32,32); pixel("game gamma applied once",64,64,64,1);
 for(int k=0;k<3;k++) for(int i=0;i<256;i++) gamma[k][i]=i*257;
 cmd(DSR_GAMMA,gamma,sizeof gamma);
 /* Explicit clear rectangles clip to the viewport; outside depth stays 0. */
 uint32_t depth[]={5,32,32,0,3,0,0,1}; cmd(DSR_SURFACE_CREATE,depth,sizeof depth);
 uint32_t with_depth[]={1,5}; cmd(DSR_SET_RT,with_depth,sizeof with_depth);
 uint32_t full_clear[]={3,0xff000000,0,0,0}; cmd(DSR_CLEAR,full_clear,sizeof full_clear);
 uint32_t box_clear[]={3,0xff00ff00,0x3f800000,0,1,12,12,20,20}; cmd(DSR_CLEAR,box_clear,sizeof box_clear);
 rs(27,0); rs(7,1); rs(14,1); rs(23,4); color(0xffff0000,1);
 pixel("paper doll depth clear inside box",255,0,0,0);
 volatile uint32_t done=0; uint32_t px=0; dsr_gl_readback(r,&px,4,&done);
 uint32_t outside[]={1,4,4,1,1,12}; cmd(DSR_READBACK,outside,sizeof outside);
 assert(done==12 && px==0xff000000); /* clipped out by depth */
 outside[1]=0xffffffffu; outside[5]=13; cmd(DSR_READBACK,outside,sizeof outside);
 assert(done==(13u|0x80000000u)); /* failed reads must not report success */
 cmd(DSR_DRAW,NULL,0); cmd(DSR_TRANSFORM,NULL,0); /* truncated commands */
 /* DS2 uses stencil for its D3D9 rendering passes. A clear must ignore prior write masks. */
 cmd(DSR_SET_RT,with_depth,sizeof with_depth); rs(7,0); rs(52,1); rs(56,3); rs(57,7); rs(58,255); rs(59,0);
 uint32_t stencil_clear[]={5,0xff000000,0x3f800000,7,0}; cmd(DSR_CLEAR,stencil_clear,sizeof stencil_clear);
 color(0xff00ff00,1); pixel("stencil equal accepts",0,255,0,0);
 rs(57,3); color(0xffff0000,1); pixel("stencil unequal rejects",0,255,0,0);
 rs(52,0); rs(7,0);
 wide_display(window,3440,1440); wide_display(window,5120,1440);
 assert(glGetError()==GL_NO_ERROR);
 dsr_gl_destroy(r); SDL_GL_DeleteContext(context); SDL_DestroyWindow(window); SDL_Quit();
 puts("renderer checks passed");
}
