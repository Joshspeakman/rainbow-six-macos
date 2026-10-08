/* Native OpenGL 3.3 consumer of the D3D7/9 fixed-function command stream as the Metal backend.
 * Texture row zero is the game's top row. Render-target Y is inverted in the vertex
 * shader; presentation flips it back. Uploads/readbacks therefore keep Windows order. */
#include "renderer.h"
#include "../../dsr/dsr_proto.h"
#include <GL/glew.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include <time.h>
#define MAXSURF 65536
long dsr_stat[8];
typedef struct { GLuint tex; uint32_t w,h,fmt,level,root,levels,face; GLenum target; int used; GLuint ms; uint32_t samples; int ms_dirty; uint32_t kind; } Surf;   /* ms: multisampled storage drawn into, resolved into tex when read */   /* target: GL_TEXTURE_2D or GL_TEXTURE_CUBE_MAP (face 0-5) */
typedef struct {   // must match game.*.glsl (std140) Uniforms
    float wvp[16], wv[16], tex0[16];
    float vp[4], fog[4], fogcolor[4], tfactor[4];
    int32_t c0[4], a0[4], c1[4], a1[4];
    float at[4];
    int32_t misc[4];
    int32_t xrgb[4];     // texture stage 0/1 is XRGB (alpha reads as 1)
    float tex1[16];      // texture-1 transform
    int32_t s1[4];       // stage 1: texcoordindex, texturetransformflags
} Uniforms;

struct DSRRenderer {
 Surf s[MAXSURF]; uint32_t rs[256],tss[16][32],tex[16],rt,ds,presented;
 float xf[32][16],vp[6]; GLuint rfbo,dfbo; uint32_t mode_w,mode_h; GLuint fbo,vao,vbo,ebo,ubo,game,quad,dummy,scratch,gamma,sampler[16];
 uint16_t ramps[3][256]; void *readback; size_t readback_size; volatile uint32_t *readback_done;
 struct GProg *progs; uint32_t nprogs, cur; GLuint dummy_cube, dummy_3d, pvbo, pebo, svao;
 int32_t scissor[4]; int framelog; uint32_t nframes; double t0; volatile uint32_t *qres; GLuint qobj[1024]; uint32_t qserial[1024]; uint16_t qpend[1024]; int nqpend; int qactive;   /* occlusion queries (DSR_QUERY) */
};
/* the game's own shader pairs (DSR_GLSL_CREATE, d3d9_shader.c): MojoShader GLSL, attributes bound to the slots of
 * programmable draws, MojoShader's per-type constant arrays */
typedef struct GProg { uint32_t id, hash; GLuint prog; GLint vf, pf, vi, pi, vb, pb, flip, at, pixfix; GLenum samptype[16]; int nsamp; } GProg;
static void mat_ident(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
static void mat_mul(float *o, const float *a, const float *b)   // D3D row-major: o = a * b
{
    float t[16]; for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { float s = 0; for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c]; t[r * 4 + c] = s; }
    memcpy(o, t, 64);
}


static Surf *surf(DSRRenderer *r,uint32_t id) { return id && id<MAXSURF && r->s[id].used ? &r->s[id] : NULL; }
static GLenum face_target(const Surf *s) { return s->target==GL_TEXTURE_CUBE_MAP?GL_TEXTURE_CUBE_MAP_POSITIVE_X+s->face:GL_TEXTURE_2D; }
/* d3d9.c's surface formats (DSR_SURFACE_CREATE): 1 ARGB8, 2 XRGB8, 3 depth/stencil, 4-9 float formats */
static void gl_format(uint32_t fmt,GLint *internal,GLenum *format,GLenum *type) {
 static const GLint fi[6]={GL_R16F,GL_RG16F,GL_RGBA16F,GL_R32F,GL_RG32F,GL_RGBA32F}; static const GLenum ff[6]={GL_RED,GL_RG,GL_RGBA,GL_RED,GL_RG,GL_RGBA};
 if(fmt==3) { *internal=GL_DEPTH24_STENCIL8; *format=GL_DEPTH_STENCIL; *type=GL_UNSIGNED_INT_24_8; }
 else if(fmt>=4 && fmt<=9) { *internal=fi[fmt-4]; *format=ff[fmt-4]; *type=GL_FLOAT; }
 else { *internal=GL_RGBA8; *format=GL_BGRA; *type=GL_UNSIGNED_BYTE; }
}
static char *readfile(const char *dir,const char *name) {
 char path[2048]; snprintf(path,sizeof path,"%s/%s",dir,name); FILE *f=fopen(path,"rb");
 if(!f) { perror(path); return NULL; } fseek(f,0,SEEK_END); long n=ftell(f); rewind(f);
 char *s=malloc((size_t)n+1); if(!s || fread(s,1,(size_t)n,f)!=(size_t)n) { fclose(f); free(s); return NULL; }
 fclose(f); s[n]=0; return s;
}
static GLuint program(const char *dir,const char *vn,const char *fn) {
 GLuint ids[2],p=glCreateProgram(); const char *names[2]={vn,fn};
 for(int k=0;k<2;k++) { char *s=readfile(dir,names[k]); if(!s) exit(1);
  ids[k]=glCreateShader(k?GL_FRAGMENT_SHADER:GL_VERTEX_SHADER); const char *src=s;
  glShaderSource(ids[k],1,&src,NULL); glCompileShader(ids[k]); free(s); GLint ok; glGetShaderiv(ids[k],GL_COMPILE_STATUS,&ok);
  if(!ok) { char log[8192]; glGetShaderInfoLog(ids[k],sizeof log,NULL,log); fprintf(stderr,"%s: %s\n",names[k],log); exit(1); }
  glAttachShader(p,ids[k]);
 }
 glLinkProgram(p); GLint ok; glGetProgramiv(p,GL_LINK_STATUS,&ok);
 if(!ok) { char log[8192]; glGetProgramInfoLog(p,sizeof log,NULL,log); fprintf(stderr,"shader link: %s\n",log); exit(1); }
 for(int k=0;k<2;k++) glDeleteShader(ids[k]); return p;
}
static void ramp(DSRRenderer *r) {
 uint16_t rgba[256][4]; for(int i=0;i<256;i++) { for(int k=0;k<3;k++) rgba[i][k]=r->ramps[k][i]; rgba[i][3]=65535; }
 glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_1D,r->gamma);
 glTexImage1D(GL_TEXTURE_1D,0,GL_RGBA16,256,0,GL_RGBA,GL_UNSIGNED_SHORT,rgba);
 glTexParameteri(GL_TEXTURE_1D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_1D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
 glActiveTexture(GL_TEXTURE0);
}
DSRRenderer *dsr_gl_create(const char *dir) {
 glewExperimental=GL_TRUE; if(glewInit()!=GLEW_OK) return NULL; glGetError();
 DSRRenderer *r=calloc(1,sizeof *r); if(!r) return NULL;
 fprintf(stderr,"renderer: OpenGL %s, %s\n",glGetString(GL_VERSION),glGetString(GL_RENDERER));
 /* D3D7 and the Metal backend blend stored UNORM values, not linear-light sRGB. */
 glDisable(GL_FRAMEBUFFER_SRGB);
 r->game=program(dir,"game.vert.glsl","game.frag.glsl"); r->quad=program(dir,"quad.vert.glsl","quad.frag.glsl");
 glGenVertexArrays(1,&r->vao); glBindVertexArray(r->vao); glGenBuffers(1,&r->vbo); glGenBuffers(1,&r->ebo); glGenBuffers(1,&r->ubo);
 glBindBuffer(GL_UNIFORM_BUFFER,r->ubo); glBufferData(GL_UNIFORM_BUFFER,sizeof(Uniforms),NULL,GL_STREAM_DRAW);
 glBindBufferBase(GL_UNIFORM_BUFFER,0,r->ubo); glUniformBlockBinding(r->game,glGetUniformBlockIndex(r->game,"GameUniforms"),0);
 glUseProgram(r->game); glUniform1i(glGetUniformLocation(r->game,"t0"),0); glUniform1i(glGetUniformLocation(r->game,"t1"),1);
 glUseProgram(r->quad); glUniform1i(glGetUniformLocation(r->quad,"image"),0); glUniform1i(glGetUniformLocation(r->quad,"gammaRamp"),1);
 glGenFramebuffers(1,&r->fbo); glGenTextures(1,&r->dummy); glGenTextures(1,&r->scratch); glGenTextures(1,&r->gamma);
 glBindTexture(GL_TEXTURE_2D,r->dummy); uint32_t white=0xffffffff;
 glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_BGRA,GL_UNSIGNED_BYTE,&white);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
 glBindTexture(GL_TEXTURE_2D,r->scratch); glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,256,256,0,GL_BGRA,GL_UNSIGNED_BYTE,NULL);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    for (int k = 0; k < 32; k++) mat_ident(r->xf[k]);
    for (int k = 0; k < 256; k++) r->rs[k] = 0;
    r->rs[7] = 1; r->rs[14] = 1; r->rs[23] = 4; r->rs[22] = 3; r->rs[19] = 2; r->rs[20] = 1; r->rs[25] = 8;   // ZENABLE, ZWRITE, ZFUNC LEQUAL, CULL CCW, SRC ONE, DST ZERO, AFUNC ALWAYS
    for (int st = 0; st < 16; st++) { r->tss[st][1] = st ? 1 : 4; r->tss[st][2] = 2; r->tss[st][3] = 1; r->tss[st][4] = st ? 1 : 2; r->tss[st][5] = 2; r->tss[st][6] = 1;
                                     r->tss[st][11] = st; r->tss[st][13] = r->tss[st][14] = 1; r->tss[st][16] = r->tss[st][17] = 2; r->tss[st][18] = 1; }

 for(int k=0;k<3;k++) for(int i=0;i<256;i++) r->ramps[k][i]=i*257;
 r->rs[53]=r->rs[54]=r->rs[55]=1; r->rs[56]=8; r->rs[58]=r->rs[59]=255; r->rs[168]=15;
 r->rs[171]=1; r->rs[193]=0xffffffffu; r->rs[186]=r->rs[187]=r->rs[188]=1; r->rs[189]=8; r->rs[207]=2; r->rs[208]=1; r->rs[209]=1;   /* D3D9 defaults */
 glGenSamplers(16,r->sampler);
 glGenVertexArrays(1,&r->svao); glGenBuffers(1,&r->pvbo); glGenBuffers(1,&r->pebo);
 glGenTextures(1,&r->dummy_cube); glBindTexture(GL_TEXTURE_CUBE_MAP,r->dummy_cube);
 for(int f=0;f<6;f++) glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+f,0,GL_RGBA8,1,1,0,GL_BGRA,GL_UNSIGNED_BYTE,&white);
 glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
 glGenTextures(1,&r->dummy_3d); glBindTexture(GL_TEXTURE_3D,r->dummy_3d);
 glTexImage3D(GL_TEXTURE_3D,0,GL_RGBA8,1,1,1,0,GL_BGRA,GL_UNSIGNED_BYTE,&white); glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
 glBindVertexArray(r->vao);
 ramp(r); glDisable(GL_DITHER); return r;
}
/* a multisampled surface's samples into its texture (before it is sampled, copied, read back or presented) */
static void resolve(DSRRenderer *r,Surf *s) {
 if(!s || !s->ms || !s->ms_dirty) return;
 if(!r->rfbo) { glGenFramebuffers(1,&r->rfbo); glGenFramebuffers(1,&r->dfbo); }
 GLenum att=s->fmt==3?GL_DEPTH_STENCIL_ATTACHMENT:GL_COLOR_ATTACHMENT0;
 glBindFramebuffer(GL_READ_FRAMEBUFFER,r->rfbo); glFramebufferTexture2D(GL_READ_FRAMEBUFFER,att,GL_TEXTURE_2D_MULTISAMPLE,s->ms,0);
 glBindFramebuffer(GL_DRAW_FRAMEBUFFER,r->dfbo); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,att,GL_TEXTURE_2D,s->tex,0);
 GLboolean sc=glIsEnabled(GL_SCISSOR_TEST); glDisable(GL_SCISSOR_TEST);    /* (called mid-draw too: the scissor is put back) */
 glBlitFramebuffer(0,0,s->w,s->h,0,0,s->w,s->h,s->fmt==3?GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT:GL_COLOR_BUFFER_BIT,GL_NEAREST);
 if(sc) glEnable(GL_SCISSOR_TEST);
 glBindFramebuffer(GL_FRAMEBUFFER,r->fbo); s->ms_dirty=0;
}
static int target_read(DSRRenderer *r,uint32_t id) {       /* for glReadPixels: resolved, single-sampled */
 Surf *s=surf(r,id); if(!s) return 0; resolve(r,s);
 glBindFramebuffer(GL_FRAMEBUFFER,r->fbo);
 glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,face_target(s),s->tex,s->level);
 glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
 return glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
}
static int target(DSRRenderer *r,uint32_t color,uint32_t depth) {
 Surf *c=surf(r,color), *z=surf(r,depth); if(!c) return 0;
 /* multisampled storage when color and depth agree on it (as Direct3D requires); else both single-sampled */
 int ms=c->ms && (!z || (z->ms && z->samples==c->samples));
 if(getenv("DSR_MSLOG") && ((c->ms!=0)!=(z && z->ms!=0)) && z) { static int n; if(n++<40) fprintf(stderr,"mslog: color %u %ux%u samples %u + depth %u %ux%u samples %u\n",color,c->w,c->h,c->samples,depth,z->w,z->h,z->samples); }
 if(!ms) { resolve(r,c); if(z && z->ms) resolve(r,z); }
 glBindFramebuffer(GL_FRAMEBUFFER,r->fbo);
 if(ms) { glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D_MULTISAMPLE,c->ms,0); c->ms_dirty=1; }
 else glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,face_target(c),c->tex,c->level);
 if(ms && z) { glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D_MULTISAMPLE,z->ms,0); z->ms_dirty=1; }
 else glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,z?face_target(z):GL_TEXTURE_2D,z?z->tex:0,z?z->level:0);
 return glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
}
/* multisampled storage for a surface (render targets and depth buffers; resolved into tex when read) */
static void make_ms(Surf *s,uint32_t samples) {
 if(samples<2 || s->ms || s->target!=GL_TEXTURE_2D || s->levels!=1 || s->level) return;
 GLint maxs=0; glGetIntegerv(GL_MAX_SAMPLES,&maxs); if((GLint)samples>maxs) samples=(uint32_t)maxs;
 GLint in; GLenum f,t; gl_format(s->fmt,&in,&f,&t); while(glGetError()) {}
 glGenTextures(1,&s->ms); glBindTexture(GL_TEXTURE_2D_MULTISAMPLE,s->ms);
 glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE,(GLsizei)samples,in,s->w,s->h,GL_TRUE); s->samples=samples; s->ms_dirty=0;
 if(glGetError()) { glDeleteTextures(1,&s->ms); s->ms=0; s->samples=0; }
}
/* DSR_MSAA=2/4/8: multisampling forced on the game's full-screen render targets and depth buffers, as a driver
 * override does (the game's own MSAA path leaves its scene without a depth buffer here); textures that are render
 * targets count when they are floating point (the HDR scene) */
static uint32_t forced_samples(DSRRenderer *r,const Surf *s) {
 static int n=-1; if(n<0) { const char *e=getenv("DSR_MSAA"); n=e?atoi(e):0; if(n!=2 && n!=4 && n!=8) n=0; }
 if(!n || !r->mode_w || s->w!=r->mode_w || s->h!=r->mode_h) return 0;
 if(s->kind==1 || s->kind==2 || (s->kind==3 && s->fmt>=4 && s->fmt<=9)) return (uint32_t)n;
 return 0;
}
static void create_surface(DSRRenderer *r,const uint32_t *p,uint32_t size) {
 uint32_t id=p[0],w=p[1],h=p[2],fmt=p[4],parent=p[5],level=p[6]; if(!id || id>=MAXSURF || !w || !h) return;
 uint32_t face=size>=40?p[8]:0; int cube=size>=40 && (p[9]&1); uint32_t samples=size>=40?(p[9]>>8)&255:0;
 Surf *s=&r->s[id]; if(s->used && s->root==id) { glDeleteTextures(1,&s->tex); if(s->ms) glDeleteTextures(1,&s->ms); }
 *s=(Surf){.w=w,.h=h,.fmt=fmt,.level=level,.root=id,.used=1,.levels=size>=32 && p[7]?p[7]:1,.face=face<6?face:0,.target=GL_TEXTURE_2D};
 Surf *root=surf(r,parent); if(root) { s->tex=root->tex; s->root=root->root; s->levels=root->levels; s->target=root->target; return; }
 GLint in; GLenum f,t; gl_format(fmt,&in,&f,&t);
 s->target=cube?GL_TEXTURE_CUBE_MAP:GL_TEXTURE_2D;
 glGenTextures(1,&s->tex); glActiveTexture(GL_TEXTURE0); glBindTexture(s->target,s->tex);
 glTexParameteri(s->target,GL_TEXTURE_MAX_LEVEL,s->levels-1);
 for(int fc=0;fc<(cube?6:1);fc++) for(uint32_t l=0;l<s->levels;l++)
  glTexImage2D(cube?GL_TEXTURE_CUBE_MAP_POSITIVE_X+fc:GL_TEXTURE_2D,l,in,w>>l?w>>l:1,h>>l?h>>l:1,0,f,t,NULL);
 if(fmt>=4 && fmt<=9) {                        /* D3D9 reads missing channels of these formats as 1 */
  int ch=(fmt-4)%3; if(ch<2) glTexParameteri(s->target,GL_TEXTURE_SWIZZLE_B,GL_ONE); if(ch<1) glTexParameteri(s->target,GL_TEXTURE_SWIZZLE_G,GL_ONE);
  if(ch<2) glTexParameteri(s->target,GL_TEXTURE_SWIZZLE_A,GL_ONE);
 }
 glTexParameteri(s->target,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(s->target,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
 glTexParameteri(s->target,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glTexParameteri(s->target,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
 s->kind=p[3];
 if(samples<2) samples=forced_samples(r,s);
 make_ms(s,samples);
}
static void quad(DSRRenderer *r,GLuint texture,const float *dst,const float *src,uint32_t color,int mode,int present) {
 glBindSampler(0,0); glBindSampler(1,0);
 glUseProgram(r->quad); glDisable(GL_STENCIL_TEST); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE); glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
 if(mode==2) { glEnable(GL_BLEND); glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA); } else glDisable(GL_BLEND);
 glUniform4fv(glGetUniformLocation(r->quad,"dst"),1,dst); glUniform4fv(glGetUniformLocation(r->quad,"src"),1,src);
 glUniform4f(glGetUniformLocation(r->quad,"color"),((color>>16)&255)/255.f,((color>>8)&255)/255.f,(color&255)/255.f,(color>>24)/255.f);
 glUniform1i(glGetUniformLocation(r->quad,"mode"),mode); glUniform1i(glGetUniformLocation(r->quad,"present"),present);
 { static int fx=-1; if(fx<0) { const char *e=getenv("DSR_FXAA"); fx=e && *e && *e!='0'; }   /* DSR_FXAA=1: FXAA on the presented image */
   glUniform1i(glGetUniformLocation(r->quad,"fxaa"),present && fx); }
 glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,texture?texture:r->dummy);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,present?GL_LINEAR:GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,present?GL_LINEAR:GL_NEAREST);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
 glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_1D,r->gamma); glActiveTexture(GL_TEXTURE0);
 glDrawArrays(GL_TRIANGLE_STRIP,0,4);
}
static void blit(DSRRenderer *r,uint32_t dst,const int32_t *dr,uint32_t src,const int32_t *sr,int mode,uint32_t color) {
 Surf *d=surf(r,dst),*s=surf(r,src); resolve(r,s); if(!d || !target(r,dst,0)) return;
 float rect[4]={dr[0]/(float)d->w,dr[1]/(float)d->h,dr[2]/(float)d->w,dr[3]/(float)d->h},uv[4]={0,0,1,1};
 if(s || mode==2) { float w=s?s->w:256,h=s?s->h:256; for(int k=0;k<4;k++) uv[k]=sr[k]/(k%2?h:w); }
 GLuint texture=mode==2?r->scratch:s?s->tex:r->dummy;
 /* Avoid sampling a texture while it is attached to this draw framebuffer. */
 GLuint copy=0;
 if(s && s->tex==d->tex) {
  glGenTextures(1,&copy); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,copy);
  glCopyTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,0,0,d->w,d->h,0); texture=copy;
 }
 glViewport(0,0,d->w,d->h); quad(r,texture,rect,uv,color,mode,0); if(copy) glDeleteTextures(1,&copy);
}
static void upload(DSRRenderer *r,const uint32_t *p,uint32_t size,int over) {
 Surf *s=surf(r,p[0]); uint32_t w=p[3],h=p[4],pitch=p[5];
 if(!s || s->fmt==3 || !w || !h || (uint64_t)pitch<(uint64_t)w*4 || 24ull+(uint64_t)pitch*h>size || (uint64_t)p[1]+w>s->w || (uint64_t)p[2]+h>s->h) return;
 over=over && w<=256 && h<=256;
 if(!over && s->fmt>=4) return;                 /* float render targets are never uploaded */
 glActiveTexture(GL_TEXTURE0); glBindTexture(over?GL_TEXTURE_2D:s->target,over?r->scratch:s->tex); glPixelStorei(GL_UNPACK_ROW_LENGTH,pitch/4);
 glTexSubImage2D(over?GL_TEXTURE_2D:face_target(s),over?0:s->level,over?0:p[1],over?0:p[2],w,h,GL_BGRA,GL_UNSIGNED_BYTE,p+6); glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
 if(over) { int32_t dr[4]={p[1],p[2],p[1]+w,p[2]+h},sr[4]={0,0,w,h}; blit(r,p[0],dr,0,sr,2,0); }
}
static GLenum blend(uint32_t b) {   /* D3DBLEND: 12/13 (BOTHSRCALPHA) are handled by the caller */
 static const GLenum v[]={GL_ONE,GL_ZERO,GL_ONE,GL_SRC_COLOR,GL_ONE_MINUS_SRC_COLOR,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA,GL_DST_COLOR,GL_ONE_MINUS_DST_COLOR,GL_SRC_ALPHA_SATURATE,
                          GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_CONSTANT_COLOR,GL_ONE_MINUS_CONSTANT_COLOR};
 return b<16?v[b]:GL_ONE;
}
static int flog(DSRRenderer *r); static void log_draw(DSRRenderer *r,const char *kind,uint32_t prog,uint32_t prim,uint32_t n);
static GLenum blend_op(uint32_t o) { return o==2?GL_FUNC_SUBTRACT:o==3?GL_FUNC_REVERSE_SUBTRACT:o==4?GL_MIN:o==5?GL_MAX:GL_FUNC_ADD; }
static GLenum compare(uint32_t f) { static const GLenum v[]={GL_ALWAYS,GL_NEVER,GL_LESS,GL_EQUAL,GL_LEQUAL,GL_GREATER,GL_NOTEQUAL,GL_GEQUAL,GL_ALWAYS}; return f<9?v[f]:GL_ALWAYS; }
static GLenum stencil_op(uint32_t op) {
 static const GLenum ops[]={GL_KEEP,GL_KEEP,GL_ZERO,GL_REPLACE,GL_INCR,GL_DECR,GL_INVERT,GL_INCR_WRAP,GL_DECR_WRAP};
 return op<9?ops[op]:GL_KEEP;
}
static GLenum address(uint32_t a) { return a==2?GL_MIRRORED_REPEAT:a==3?GL_CLAMP_TO_EDGE:a==4?GL_CLAMP_TO_BORDER:GL_REPEAT; }
static void bind_texture(DSRRenderer *r,int stage,Surf *s) {
 glActiveTexture(GL_TEXTURE0+stage);
 if(s && s->target==GL_TEXTURE_CUBE_MAP) s=NULL;  /* a 2D sampler: a cube texture cannot be bound there */
 if(s && s->ms_dirty) { resolve(r,s); glActiveTexture(GL_TEXTURE0+stage); }
 glBindTexture(GL_TEXTURE_2D,s?s->tex:r->dummy);
 GLuint sampler=r->sampler[stage]; glBindSampler(stage,sampler);
 uint32_t *t=r->tss[stage]; int linear=t[17]>=2,mip=s && s->levels>1 ? t[18]:0;
 GLenum min=mip==3?(linear?GL_LINEAR_MIPMAP_LINEAR:GL_NEAREST_MIPMAP_LINEAR):mip==2?(linear?GL_LINEAR_MIPMAP_NEAREST:GL_NEAREST_MIPMAP_NEAREST):(linear?GL_LINEAR:GL_NEAREST);
 glSamplerParameteri(sampler,GL_TEXTURE_MIN_FILTER,min); glSamplerParameteri(sampler,GL_TEXTURE_MAG_FILTER,t[16]>=2?GL_LINEAR:GL_NEAREST);
 glSamplerParameteri(sampler,GL_TEXTURE_WRAP_S,address(t[13])); glSamplerParameteri(sampler,GL_TEXTURE_WRAP_T,address(t[14]));
}
/* depth, stencil, colour mask, blending, culling and viewport from the D3D render states */
static float rsf(const DSRRenderer *r,int k) { float f; memcpy(&f,&r->rs[k],4); return f; }
static void apply_state(DSRRenderer *r,Surf *ds,float vpw,float vph) {
 uint32_t *rs=r->rs;
 if(ds && rs[7]) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
 glDepthMask(ds && rs[7] && rs[14]); glDepthFunc(compare(rs[23]));
 /* depth bias (195 DEPTHBIAS in depth-buffer units, 175 SLOPESCALEDEPTHBIAS): a 24-bit buffer's unit is 2^-24 */
 { float bias=rsf(r,195), slope=rsf(r,175);
   if(bias!=0 || slope!=0) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(slope,bias*16777216.0f); } else glDisable(GL_POLYGON_OFFSET_FILL); }
 if(ds && rs[52]) {
  glEnable(GL_STENCIL_TEST);
  if(rs[185]) {                                  /* TWOSIDEDSTENCILMODE: counter-clockwise faces use 186-189 */
   glStencilFuncSeparate(GL_FRONT,compare(rs[56]),rs[57]&255,rs[58]&255); glStencilOpSeparate(GL_FRONT,stencil_op(rs[53]),stencil_op(rs[54]),stencil_op(rs[55]));
   glStencilFuncSeparate(GL_BACK,compare(rs[189]),rs[57]&255,rs[58]&255); glStencilOpSeparate(GL_BACK,stencil_op(rs[186]),stencil_op(rs[187]),stencil_op(rs[188]));
  } else { glStencilFunc(compare(rs[56]),rs[57]&255,rs[58]&255); glStencilOp(stencil_op(rs[53]),stencil_op(rs[54]),stencil_op(rs[55])); }
  glStencilMask(rs[59]&255);
 } else glDisable(GL_STENCIL_TEST);
 glColorMask((rs[168]&1)!=0,(rs[168]&2)!=0,(rs[168]&4)!=0,(rs[168]&8)!=0);
 if(rs[27]) {                                    /* blend: op (171), separate alpha (206-209), factor (193) */
  glEnable(GL_BLEND);
  uint32_t src=rs[19],dst=rs[20],sa=rs[206]?rs[207]:src,da=rs[206]?rs[208]:dst,op=rs[171],opa=rs[206]?rs[209]:op;
  if(src==12) { src=5; dst=6; } if(src==13) { src=6; dst=5; } if(sa==12) { sa=5; da=6; } if(sa==13) { sa=6; da=5; }
  glBlendFuncSeparate(blend(src),blend(dst),blend(sa),blend(da)); glBlendEquationSeparate(blend_op(op),blend_op(opa));
  uint32_t f=rs[193]; glBlendColor(((f>>16)&255)/255.f,((f>>8)&255)/255.f,(f&255)/255.f,(f>>24)/255.f);
 } else glDisable(GL_BLEND);
 if(rs[174]) {                                   /* SCISSORTESTENABLE: the rectangle in render-target pixels (top row first) */
  Surf *rt=surf(r,r->rt); int h=rt?(int)rt->h:0;
  glEnable(GL_SCISSOR_TEST); glScissor(r->scissor[0],r->scissor[1],r->scissor[2]-r->scissor[0],r->scissor[3]-r->scissor[1]); (void)h;
 } else glDisable(GL_SCISSOR_TEST);
 glFrontFace(GL_CCW);
 if(rs[22]==2 || rs[22]==3) { glEnable(GL_CULL_FACE); glCullFace(rs[22]==2?GL_FRONT:GL_BACK); } else glDisable(GL_CULL_FACE);
 glViewport(r->vp[0],r->vp[1],vpw,vph); glDepthRange(r->vp[4],r->vp[5]?r->vp[5]:1);
}
static void draw(DSRRenderer *r,const uint32_t *p,uint32_t size) {
 uint32_t prim=p[0],fvf=p[1],nv=p[2],ni=p[3]; int rhw=(fvf&14)==4;
 uint32_t stride=rhw?16:12, coloroff=0,uvoff=0;
 if(fvf&16) stride+=12; if(fvf&64) { coloroff=stride; stride+=4; } if(fvf&128) stride+=4; uvoff=stride;
 for(uint32_t k=0;k<((fvf>>8)&15);k++) { uint32_t f=(fvf>>(16+2*k))&3; stride+=f==0?8:f==1?12:f==2?16:4; }
 dsr_stat[0]++; if(!nv || 16ull+(uint64_t)nv*stride+(uint64_t)ni*2>size) { dsr_stat[1]++; return; }
 Surf *rt=surf(r,r->rt), *ds=surf(r,r->ds); if(!rt || !target(r,r->rt,r->ds)) { dsr_stat[2]++; return; }
 glBindVertexArray(r->vao); if(flog(r)) log_draw(r,"ff",0,prim,nv);
    Uniforms u; memset(&u, 0, sizeof u);
    float wv[16]; mat_mul(wv, r->xf[1], r->xf[2]); mat_mul(u.wvp, wv, r->xf[3]); memcpy(u.wv, wv, 64); memcpy(u.tex0, r->xf[16], 64); memcpy(u.tex1, r->xf[17], 64);
    u.s1[0] = r->tss[1][11]; u.s1[1] = r->tss[1][24];
    u.vp[0] = r->vp[0]; u.vp[1] = r->vp[1]; u.vp[2] = r->vp[2] ? r->vp[2] : rt->w; u.vp[3] = r->vp[3] ? r->vp[3] : rt->h;
    { float fs, fe; memcpy(&fs, &r->rs[36], 4); memcpy(&fe, &r->rs[37], 4); u.fog[0] = fs; u.fog[1] = fe; u.fog[2] = r->rs[28] && r->rs[140] == 3 && fe != fs ? 1 : 0; }
    uint32_t fc = r->rs[34]; u.fogcolor[0] = ((fc >> 16) & 255) / 255.0f; u.fogcolor[1] = ((fc >> 8) & 255) / 255.0f; u.fogcolor[2] = (fc & 255) / 255.0f; u.fogcolor[3] = 1;
    uint32_t tf = r->rs[60]; u.tfactor[0] = ((tf >> 16) & 255) / 255.0f; u.tfactor[1] = ((tf >> 8) & 255) / 255.0f; u.tfactor[2] = (tf & 255) / 255.0f; u.tfactor[3] = (tf >> 24) / 255.0f;
    u.c0[0] = r->tss[0][1]; u.c0[1] = r->tss[0][2]; u.c0[2] = r->tss[0][3]; u.c0[3] = r->tss[0][4]; u.a0[0] = r->tss[0][5]; u.a0[1] = r->tss[0][6]; u.a0[2] = r->tss[0][11]; u.a0[3] = r->tss[0][24];
    u.c1[0] = r->tss[1][1]; u.c1[1] = r->tss[1][2]; u.c1[2] = r->tss[1][3]; u.c1[3] = r->tss[1][4]; u.a1[0] = r->tss[1][5]; u.a1[1] = r->tss[1][6];
    u.at[0] = (r->rs[24] & 255) / 255.0f; u.at[1] = r->rs[25]; u.at[2] = r->rs[15] ? 1 : 0;
    Surf *t0 = surf(r, r->tex[0]), *t1 = surf(r, r->tex[1]);
    u.misc[0] = (fvf & 0xe) == 4; u.misc[1] = (fvf & 0x40) != 0; u.misc[2] = t0 != NULL; u.misc[3] = t1 != NULL;
    u.xrgb[0] = t0 && t0->fmt == 2; u.xrgb[1] = t1 && t1->fmt == 2;

 glUseProgram(r->game); glBindBuffer(GL_UNIFORM_BUFFER,r->ubo); glBufferSubData(GL_UNIFORM_BUFFER,0,sizeof u,&u);
 apply_state(r,ds,u.vp[2],u.vp[3]);
 glBindBuffer(GL_ARRAY_BUFFER,r->vbo); glBufferData(GL_ARRAY_BUFFER,nv*stride,p+4,GL_STREAM_DRAW);
 glEnableVertexAttribArray(0); glVertexAttribPointer(0,rhw?4:3,GL_FLOAT,GL_FALSE,stride,0);
 if(fvf&64) { glEnableVertexAttribArray(1); glVertexAttribPointer(1,GL_BGRA,GL_UNSIGNED_BYTE,GL_TRUE,stride,(void *)(uintptr_t)coloroff); }
 else { glDisableVertexAttribArray(1); glVertexAttrib4f(1,1,1,1,1); }
 if((fvf>>8)&15) { glEnableVertexAttribArray(2); glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,stride,(void *)(uintptr_t)uvoff); }
 else { glDisableVertexAttribArray(2); glVertexAttrib2f(2,0,0); }
 bind_texture(r,0,t0); bind_texture(r,1,t1);
 GLenum type=prim==1?GL_POINTS:prim==2?GL_LINES:prim==3?GL_LINE_STRIP:prim==5?GL_TRIANGLE_STRIP:prim==6?GL_TRIANGLE_FAN:GL_TRIANGLES;
 if(ni) { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,r->ebo); glBufferData(GL_ELEMENT_ARRAY_BUFFER,ni*2,(const uint8_t *)(p+4)+nv*stride,GL_STREAM_DRAW); glDrawElements(type,ni,GL_UNSIGNED_SHORT,0); }
 else glDrawArrays(type,0,nv);
}
static void clear(DSRRenderer *r,const uint32_t *p,uint32_t size) {
 if(size<20 || (uint64_t)p[4]*16+20>size) return;
 Surf *rt=surf(r,r->rt); if(!rt || !target(r,r->rt,r->ds)) return;
 int32_t full[4]={(int32_t)r->vp[0],(int32_t)r->vp[1],(int32_t)(r->vp[0]+(r->vp[2]?r->vp[2]:rt->w)),(int32_t)(r->vp[1]+(r->vp[3]?r->vp[3]:rt->h))};
 if(full[0]<0) full[0]=0; if(full[1]<0) full[1]=0;
 if(full[2]>(int32_t)rt->w) full[2]=rt->w; if(full[3]>(int32_t)rt->h) full[3]=rt->h;
 uint32_t c=p[1]; float z; memcpy(&z,p+2,4);
 glClearColor(((c>>16)&255)/255.f,((c>>8)&255)/255.f,(c&255)/255.f,(c>>24)/255.f);
 glClearDepth(z); glClearStencil(p[3]&255); glDepthMask(GL_TRUE); glStencilMask(255); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
 GLbitfield mask=((p[0]&1)?GL_COLOR_BUFFER_BIT:0)|((p[0]&2)?GL_DEPTH_BUFFER_BIT:0)|((p[0]&4)?GL_STENCIL_BUFFER_BIT:0);
 glEnable(GL_SCISSOR_TEST);
 for(uint32_t k=0;k<(p[4]?p[4]:1);k++) {
  int32_t rc[4]; memcpy(rc,p[4]?p+5+4*k:(const uint32_t *)full,16);
  if(rc[0]<full[0]) rc[0]=full[0]; if(rc[1]<full[1]) rc[1]=full[1];
  if(rc[2]>full[2]) rc[2]=full[2]; if(rc[3]>full[3]) rc[3]=full[3];
  if(rc[2]<=rc[0] || rc[3]<=rc[1]) continue;
  glScissor(rc[0],rc[1],rc[2]-rc[0],rc[3]-rc[1]); glClear(mask);
 }
 glDisable(GL_SCISSOR_TEST);
}
void dsr_gl_queries(DSRRenderer *r,volatile uint32_t *results) { r->qres=results; }
static void query(DSRRenderer *r,const uint32_t *p) {             /* DSR_QUERY: slot, begin/end, serial */
 uint32_t k=p[0]; if(!k || k>=1024) return;
 if(r->framelog==1) fprintf(stderr,"frame: query %u %s serial %u (active %d)\n",k,p[1]?"begin":"end",p[2],r->qactive);
 if(p[1]) { if(r->qactive) glEndQuery(GL_SAMPLES_PASSED); if(!r->qobj[k]) glGenQueries(1,&r->qobj[k]); glBeginQuery(GL_SAMPLES_PASSED,r->qobj[k]); r->qactive=(int)k; }
 else if(r->qactive==(int)k) {
  glEndQuery(GL_SAMPLES_PASSED); r->qactive=0; r->qserial[k]=p[2];
  int dup=0; for(int i=0;i<r->nqpend;i++) if(r->qpend[i]==k) dup=1;
  if(!dup && r->nqpend<1024) r->qpend[r->nqpend++]=(uint16_t)k;
 } else if(r->qres) { r->qres[k]=1000; __atomic_store_n(&r->qres[1024+k],p[2],__ATOMIC_RELEASE); }   /* an End without a Begin */
}
void dsr_gl_poll_queries(DSRRenderer *r) {                       /* publish the counts that are ready */
 for(int i=0;i<r->nqpend;) {
  uint32_t k=r->qpend[i]; GLuint avail=0; glGetQueryObjectuiv(r->qobj[k],GL_QUERY_RESULT_AVAILABLE,&avail);
  if(!avail) { i++; continue; }
  GLuint n=0; glGetQueryObjectuiv(r->qobj[k],GL_QUERY_RESULT,&n);
  if(getenv("DSR_QUERYLOG")) fprintf(stderr,"query %u serial %u -> %u\n",k,r->qserial[k],n);
  if(r->qres) { r->qres[k]=n; __atomic_store_n(&r->qres[1024+k],r->qserial[k],__ATOMIC_RELEASE); }
  r->qpend[i]=r->qpend[--r->nqpend];
 }
}
void dsr_gl_readback(DSRRenderer *r,void *data,size_t size,volatile uint32_t *done) { r->readback=data; r->readback_size=size; r->readback_done=done; }
static void readback(DSRRenderer *r,const uint32_t *p) {
 Surf *s=surf(r,p[0]); int ok=0;
 if(s && s->fmt!=3 && r->readback && p[3] && p[4] && (uint64_t)p[3]*p[4]*4<=r->readback_size && (uint64_t)p[1]+p[3]<=s->w && (uint64_t)p[2]+p[4]<=s->h && target_read(r,p[0])) {
  glReadPixels(p[1],p[2],p[3],p[4],GL_BGRA,GL_UNSIGNED_BYTE,r->readback); ok=1;
 }
 if(r->readback_done) __atomic_store_n(r->readback_done,p[5]|(ok?0:0x80000000u),__ATOMIC_RELEASE);
}
void dsr_gl_size(DSRRenderer *r,uint32_t *w,uint32_t *h) { Surf *s=surf(r,r->presented); *w=s?s->w:0; *h=s?s->h:0; }
void dsr_gl_present(DSRRenderer *r,int width,int height) {
 Surf *s=surf(r,r->presented); if(!s) return; resolve(r,s);
 glBindFramebuffer(GL_FRAMEBUFFER,0); glDisable(GL_SCISSOR_TEST); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
 double scale=(double)width/s->w; if((double)height/s->h<scale) scale=(double)height/s->h;
 int w=(int)(s->w*scale),h=(int)(s->h*scale); glViewport((width-w)/2,(height-h)/2,w,h);
 const float rect[4]={0,0,1,1}; quad(r,s->tex,rect,rect,0,0,1); glFlush();
}
int dsr_gl_screenshot(DSRRenderer *r,const char *path) {
 Surf *s=surf(r,r->presented);
 if(!s || !target_read(r,r->presented)) { fprintf(stderr,"screenshot: presented surface %u %s (status %#x)\n",r->presented,s?"is not renderable":"is unknown",s?glCheckFramebufferStatus(GL_FRAMEBUFFER):0); return -1; }
 uint8_t *pixels=malloc((size_t)s->w*s->h*4); if(!pixels) return -1;
 glReadPixels(0,0,s->w,s->h,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
 png_image image={0}; image.version=PNG_IMAGE_VERSION; image.width=s->w; image.height=s->h; image.format=PNG_FORMAT_RGBA;
 int ok=png_image_write_to_file(&image,path,0,pixels,0,NULL); if(!ok) fprintf(stderr,"screenshot: %s\n",image.message);
 png_image_free(&image); free(pixels); return ok?0:-1;
}
int dsr_gl_display_screenshot(const char *path,int width,int height) {
 if(width<=0 || height<=0) return -1;
 size_t row=(size_t)width*4;
 uint8_t *pixels=malloc(row*height), *topdown=malloc(row*height);
 if(!pixels || !topdown) { free(pixels); free(topdown); return -1; }
 glBindFramebuffer(GL_FRAMEBUFFER,0); glReadBuffer(GL_BACK);
 glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
 for(int y=0;y<height;y++) memcpy(topdown+y*row,pixels+(height-1-y)*row,row);
 png_image image={0}; image.version=PNG_IMAGE_VERSION; image.width=width; image.height=height; image.format=PNG_FORMAT_RGBA;
 int ok=png_image_write_to_file(&image,path,0,topdown,0,NULL);
 png_image_free(&image); free(pixels); free(topdown); return ok?0:-1;
}

/* DSR_FRAMELOG_SEC=<seconds>: every draw of the first frame after that time, with its program, target, textures
 * and blending, to stderr; DSR_FRAMELOG_GLSL=<dir> also writes the programs' sources there */
static int flog(DSRRenderer *r) { return r->framelog == 1; }
static void log_draw(DSRRenderer *r,const char *kind,uint32_t prog,uint32_t prim,uint32_t n) {
 Surf *rt=surf(r,r->rt); uint32_t *rs=r->rs;
 fprintf(stderr,"frame: %s prog %08x prim %u n %u | rt %u %ux%u f%u ds %u | blend %u %u:%u op %u sep %u %u:%u | z %u w %u f %u | at %u ref %u func %u | cw %x | tex",
         kind,prog,prim,n,r->rt,rt?rt->w:0,rt?rt->h:0,rt?rt->fmt:0,r->ds,rs[27],rs[19],rs[20],rs[171],rs[206],rs[207],rs[208],rs[7],rs[14],rs[23],rs[15],rs[24],rs[25],rs[168]);
 for(int k=0;k<16;k++) if(r->tex[k]) { Surf *t=surf(r,r->tex[k]); fprintf(stderr," [%d]%u:%ux%u f%u%s",k,r->tex[k],t?t->w:0,t?t->h:0,t?t->fmt:0,t&&t->target==GL_TEXTURE_CUBE_MAP?"c":""); }
 fprintf(stderr,"\n");
}
/* ---- the game's own shaders ---- */
static GProg *gprog(DSRRenderer *r,uint32_t id) { for(uint32_t k=0;k<r->nprogs;k++) if(r->progs[k].id==id) return &r->progs[k]; return NULL; }
static GLuint compile(GLenum type,const char *src,uint32_t id) {
 GLuint sh=glCreateShader(type); glShaderSource(sh,1,&src,NULL); glCompileShader(sh); GLint ok; glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);
 if(!ok) { char log[4096]; glGetShaderInfoLog(sh,sizeof log,NULL,log); static int n; if(n++<10) fprintf(stderr,"renderer: program %u %s shader: %.600s\n",id,type==GL_VERTEX_SHADER?"vertex":"pixel",log); glDeleteShader(sh); return 0; }
 return sh;
}
static void glsl_create(DSRRenderer *r,const uint32_t *p,uint32_t size) {
 uint32_t id=p[0],na=p[1],vl=p[2],pl=p[3];
 if(na>16 || 16ull+32ull*na+((vl+4)&~3u)+((pl+4)&~3u)>size || gprog(r,id)) return;
 const char *vs=(const char *)(p+4+8*na), *ps=vs+((vl+4)&~3u);
 { const char *dir=getenv("DSR_FRAMELOG_GLSL"); if(dir) { char path[1024]; { uint32_t h=2166136261u; for(const char *c=vs;*c;c++) h=(h^(uint8_t)*c)*16777619u; for(const char *c=ps;*c;c++) h=(h^(uint8_t)*c)*16777619u; snprintf(path,sizeof path,"%s/%08x.glsl",dir,h); } FILE *o=fopen(path,"w"); if(o) { fprintf(o,"%s\n//---- pixel ----\n%s",vs,ps); fclose(o); } } }
 GLuint v=compile(GL_VERTEX_SHADER,vs,id), f=v?compile(GL_FRAGMENT_SHADER,ps,id):0;
 GProg g={0}; g.id=id;
 { uint32_t h=2166136261u; for(const char *c=vs;*c;c++) h=(h^(uint8_t)*c)*16777619u; for(const char *c=ps;*c;c++) h=(h^(uint8_t)*c)*16777619u; g.hash=h; if(getenv("DSR_PROGLOG")) fprintf(stderr,"renderer: program %u is %08x\n",g.id,h); }   /* stable across runs */
 if(v && f) {
  g.prog=glCreateProgram(); glAttachShader(g.prog,v); glAttachShader(g.prog,f);
  for(uint32_t k=0;k<na;k++) { char name[29]; memcpy(name,p+4+8*k+1,28); name[28]=0; glBindAttribLocation(g.prog,p[4+8*k],name); }
  glLinkProgram(g.prog); GLint ok; glGetProgramiv(g.prog,GL_LINK_STATUS,&ok);
  if(!ok) { char log[4096]; glGetProgramInfoLog(g.prog,sizeof log,NULL,log); static int n; if(n++<10) fprintf(stderr,"renderer: program %u link: %.600s\n",id,log); glDeleteProgram(g.prog); g.prog=0; }
 }
 if(v) glDeleteShader(v); if(f) glDeleteShader(f);
 if(g.prog) {
  g.vf=glGetUniformLocation(g.prog,"vs_uniforms_vec4"); g.pf=glGetUniformLocation(g.prog,"ps_uniforms_vec4");
  g.vi=glGetUniformLocation(g.prog,"vs_uniforms_ivec4"); g.pi=glGetUniformLocation(g.prog,"ps_uniforms_ivec4");
  g.vb=glGetUniformLocation(g.prog,"vs_uniforms_bool"); g.pb=glGetUniformLocation(g.prog,"ps_uniforms_bool");
  g.flip=glGetUniformLocation(g.prog,"vpFlip"); g.at=glGetUniformLocation(g.prog,"d3d_alphatest"); g.pixfix=glGetUniformLocation(g.prog,"d3d_pixfix");
  glUseProgram(g.prog);
  for(int k=0;k<16;k++) {                     /* sampler register k -> texture unit k; its type for the stand-in texture */
   char n[16]; snprintf(n,sizeof n,"ps_s%d",k); GLint loc=glGetUniformLocation(g.prog,n); if(loc<0) continue;
   glUniform1i(loc,k); g.nsamp=k+1;
   GLint cnt; glGetProgramiv(g.prog,GL_ACTIVE_UNIFORMS,&cnt);
   for(GLint u=0;u<cnt;u++) { char un[64]; GLint sz; GLenum ty; glGetActiveUniform(g.prog,u,sizeof un,NULL,&sz,&ty,un); if(!strcmp(un,n)) g.samptype[k]=ty; }
  }
 }
 r->progs=realloc(r->progs,(r->nprogs+1)*sizeof *r->progs); r->progs[r->nprogs++]=g;
}
static void shader_consts(DSRRenderer *r,const uint32_t *p,uint32_t size) {
 GProg *g=gprog(r,r->cur); if(!g || !g->prog || size<24) return;
 uint32_t vf=p[0],pf=p[1],vi=p[2],pi=p[3],vb=p[4],pb=p[5];
 if(24ull+16ull*(vf+pf+vi+pi)+4ull*(vb+pb)+16>size) return;
 const uint32_t *w=p+6; glUseProgram(g->prog);
 { static uint32_t want; static int init; static int left=40; if(!init) { init=1; const char *e=getenv("DSR_PEEK_PROG"); want=e?(uint32_t)strtoul(e,0,16):0; }
   if(want==g->hash && left>0) { left--; const float *f=(const float *)w; fprintf(stderr,"consts %u: vs",g->id);
     for(uint32_t k=0;k<vf;k++) fprintf(stderr," c%u(%g %g %g %g)",k,f[4*k],f[4*k+1],f[4*k+2],f[4*k+3]); f+=4*vf; fprintf(stderr," | ps");
     for(uint32_t k=0;k<pf;k++) fprintf(stderr," c%u(%g %g %g %g)",k,f[4*k],f[4*k+1],f[4*k+2],f[4*k+3]); fprintf(stderr,"\n"); } }
 if(vf && g->vf>=0) glUniform4fv(g->vf,vf,(const float *)w); w+=4*vf;
 if(pf && g->pf>=0) glUniform4fv(g->pf,pf,(const float *)w); w+=4*pf;
 if(vi && g->vi>=0) glUniform4iv(g->vi,vi,(const GLint *)w); w+=4*vi;
 if(pi && g->pi>=0) glUniform4iv(g->pi,pi,(const GLint *)w); w+=4*pi;
 if(vb && g->vb>=0) glUniform1iv(g->vb,vb,(const GLint *)w); w+=vb;
 if(pb && g->pb>=0) glUniform1iv(g->pb,pb,(const GLint *)w); w+=pb;
 if(g->at>=0) glUniform4fv(g->at,1,(const float *)w);
 if(getenv("DSR_ATLOG") && flog(r)) fprintf(stderr,"atlog %08x loc %d: %g %g %g %g\n",g->hash,g->at,((float *)w)[0],((float *)w)[1],((float *)w)[2],((float *)w)[3]);
 if(g->flip>=0) glUniform1f(g->flip,-1.0f);        /* render targets are stored top row first: y is inverted */
}
/* vertex and index buffers kept on the GPU (DSR_VBUF_DATA), by the D3D9 layer's buffer id */
static GLuint vbuf[DSR_VBUFS]; static uint32_t vbuf_size[DSR_VBUFS];
static void vbuf_data(const uint32_t *p,uint32_t size) {
 uint32_t id=p[0],total=p[1],off=p[2],n=p[3]; if(id>=DSR_VBUFS || 16ull+n>size || (uint64_t)off+n>total) return;
 if(!vbuf[id]) glGenBuffers(1,&vbuf[id]);
 glBindBuffer(GL_COPY_WRITE_BUFFER,vbuf[id]);
 if(vbuf_size[id]!=total) { glBufferData(GL_COPY_WRITE_BUFFER,total,NULL,GL_DYNAMIC_DRAW); vbuf_size[id]=total; }
 if(n) glBufferSubData(GL_COPY_WRITE_BUFFER,off,n,p+4);
}
static void vbuf_free(const uint32_t *p) { uint32_t id=p[0]; if(id<DSR_VBUFS && vbuf[id]) { glDeleteBuffers(1,&vbuf[id]); vbuf[id]=0; vbuf_size[id]=0; } }
static int vb_attrib(uint32_t sl,uint32_t type,uint32_t stride,uint32_t off) {   /* a D3DDECLTYPE as GL reads it (missing components 0, 0, 0, 1 in both) */
 static const struct { GLint n; GLenum t; GLboolean norm; } f[]={
  {1,GL_FLOAT,0},{2,GL_FLOAT,0},{3,GL_FLOAT,0},{4,GL_FLOAT,0},{GL_BGRA,GL_UNSIGNED_BYTE,1},{4,GL_UNSIGNED_BYTE,0},
  {2,GL_SHORT,0},{4,GL_SHORT,0},{4,GL_UNSIGNED_BYTE,1},{2,GL_SHORT,1},{4,GL_SHORT,1},{2,GL_UNSIGNED_SHORT,1},{4,GL_UNSIGNED_SHORT,1},
  {0,0,0},{0,0,0},{2,GL_HALF_FLOAT,0},{4,GL_HALF_FLOAT,0}};
 if(type>16 || !f[type].n) return 0;
 glVertexAttribPointer(sl,f[type].n,f[type].t,f[type].norm,(GLsizei)stride,(void *)(uintptr_t)off); return 1;
}
/* DSR_DRAW_PROG (vb 0: float4 vertices in the command) or DSR_DRAW_PROG_VB (vb 1: elements of kept buffers) */
static void draw_prog_x(DSRRenderer *r,const uint32_t *p,uint32_t size,int vb) {
 uint32_t prim=p[0],mask=vb?0:p[1],nv=vb?p[1]:p[2],ni=vb?p[2]:p[3],flags=vb?p[3]:p[4],ns=(uint32_t)__builtin_popcount(mask);
 GProg *g=gprog(r,r->cur); dsr_stat[0]++;
 if(!g || !g->prog || !nv || (vb ? 36ull+20ull*p[8]>size : 20ull+16ull*nv*ns+2ull*ni>size)) { dsr_stat[1]++; return; }
 Surf *rt=surf(r,r->rt), *ds=surf(r,r->ds); if(!rt || !target(r,r->rt,r->ds)) { dsr_stat[2]++; return; }
 if(flog(r)) log_draw(r,flags&1?"sh-pretransformed":"sh",g->hash,prim,ni?ni:nv);
 { static int ql=-1; if(ql<0) ql=getenv("DSR_QUERYLOG")!=0; if(ql && r->qactive) fprintf(stderr,"inquery %d prog %08x n %u\n",r->qactive,g->hash,ni?ni:nv); }
 { static const char *skip; static int init; if(!init) { init=1; skip=getenv("DSR_SKIP_PROG"); }   /* DSR_SKIP_PROG=hash,hash: those programs' draws left out */
   if(skip) { char h[16]; snprintf(h,sizeof h,"%08x",g->hash); if(strstr(skip,h)) return; } }
 if(flog(r)) { static const char *td; static int td_init, done; if(!td_init) { td_init=1; td=getenv("DSR_TEXDUMP_PROG"); }   /* DSR_TEXDUMP_PROG=hash: in the logged frame, that program's samplers 0-1 (level 0) as /tmp/dsr-tex-<hash>-<k>.pam */
   char h[16]; snprintf(h,sizeof h,"%08x",g->hash);
   if(td && !done && strstr(td,h)) { done=1; for(int k=0;k<2;k++) { Surf *t=surf(r,r->tex[k]); if(!t || t->target!=GL_TEXTURE_2D) continue;
       glBindTexture(GL_TEXTURE_2D,t->tex);
       for(int lv=1;lv<12;lv++) { GLint lw=0,lh=0; glGetTexLevelParameteriv(GL_TEXTURE_2D,lv,GL_TEXTURE_WIDTH,&lw); glGetTexLevelParameteriv(GL_TEXTURE_2D,lv,GL_TEXTURE_HEIGHT,&lh); if(!lw) break;
         uint8_t *q2=malloc((size_t)lw*lh*4); glGetTexImage(GL_TEXTURE_2D,lv,GL_RGBA,GL_UNSIGNED_BYTE,q2); long sa=0; for(long i=0;i<(long)lw*lh;i++) sa+=q2[4*i+3];
         fprintf(stderr,"texdump %s sampler %d level %d: %dx%d mean alpha %.1f first %u %u %u %u\n",h,k,lv,lw,lh,(double)sa/(lw*lh),q2[0],q2[1],q2[2],q2[3]); free(q2); }
       { GLint mn=0,mx=0,base=0,maxl=0; glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,&mn); glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,&base); glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,&maxl); (void)mx;
         fprintf(stderr,"texdump %s sampler %d: min filter %x base %d max level %d (surface levels %u)\n",h,k,mn,base,maxl,t->levels); }
       uint8_t *px=malloc((size_t)t->w*t->h*4); glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
       char path[64]; snprintf(path,sizeof path,"/tmp/dsr-tex-%s-%d.pam",h,k); FILE *f=fopen(path,"wb");
       if(f) { fprintf(f,"P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",t->w,t->h); fwrite(px,1,(size_t)t->w*t->h*4,f); fclose(f); }
       free(px); fprintf(stderr,"texdump %s sampler %d: surface %u %ux%u fmt %u\n",h,k,r->tex[k],t->w,t->h,t->fmt); } } }
 glUseProgram(g->prog); glBindVertexArray(r->svao);
 float vpw=r->vp[2]?r->vp[2]:rt->w, vph=r->vp[3]?r->vp[3]:rt->h;
 apply_state(r,ds,vpw,vph);
 if(g->pixfix>=0) glUniform4f(g->pixfix,1.0f/vpw,-1.0f/vph,0,0);   /* D3D9 pixel centres (d3d9_shader.c wrap_vs) */
 { static const char *red; static int init; if(!init) { init=1; red=getenv("DSR_RED_PROG"); }   /* DSR_RED_PROG=hash: those draws' surviving pixels painted red */
   if(red) { char h[16]; snprintf(h,sizeof h,"%08x",g->hash); if(strstr(red,h)) { glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_CONSTANT_COLOR,GL_ZERO); glBlendColor(1,0,0,1); } } }
 if(vb) {
  uint32_t used=0; const uint32_t *e=p+9;
  for(uint32_t k=0;k<p[8];k++,e+=5) {
   if(e[0]>=16 || e[1]>=DSR_VBUFS || !vbuf[e[1]]) continue;
   glBindBuffer(GL_ARRAY_BUFFER,vbuf[e[1]]);
   if(vb_attrib(e[0],e[4],e[3],e[2])) { glEnableVertexAttribArray(e[0]); used|=1u<<e[0]; }
  }
  for(uint32_t sl=0;sl<16;sl++) if(!(used>>sl&1)) { glDisableVertexAttribArray(sl); glVertexAttrib4f(sl,0,0,0,1); }
 } else {
  glBindBuffer(GL_ARRAY_BUFFER,r->pvbo); glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)nv*ns*16,p+5,GL_STREAM_DRAW);
  for(uint32_t sl=0,c=0;sl<16;sl++) {
   if(mask>>sl&1) { glEnableVertexAttribArray(sl); glVertexAttribPointer(sl,4,GL_FLOAT,GL_FALSE,ns*16,(void *)(uintptr_t)(c*16)); c++; }
   else { glDisableVertexAttribArray(sl); glVertexAttrib4f(sl,0,0,0,1); }
  }
 }
 for(int k=0;k<g->nsamp && k<16;k++) {
  if(!g->samptype[k]) continue;
  if(g->samptype[k]==GL_SAMPLER_2D || g->samptype[k]==GL_SAMPLER_2D_SHADOW) bind_texture(r,k,surf(r,r->tex[k]));
  else if(g->samptype[k]==GL_SAMPLER_CUBE) {
   Surf *t=surf(r,r->tex[k]); int ok=t && t->target==GL_TEXTURE_CUBE_MAP;
   glActiveTexture(GL_TEXTURE0+k); glBindTexture(GL_TEXTURE_CUBE_MAP,ok?t->tex:r->dummy_cube);
   GLuint sm=r->sampler[k]; glBindSampler(k,sm); uint32_t *ts=r->tss[k];
   int mip=ok && t->levels>1?ts[18]:0, lin=ts[17]>=2;
   glSamplerParameteri(sm,GL_TEXTURE_MIN_FILTER,mip==3?(lin?GL_LINEAR_MIPMAP_LINEAR:GL_NEAREST_MIPMAP_LINEAR):mip==2?(lin?GL_LINEAR_MIPMAP_NEAREST:GL_NEAREST_MIPMAP_NEAREST):(lin?GL_LINEAR:GL_NEAREST));
   glSamplerParameteri(sm,GL_TEXTURE_MAG_FILTER,ts[16]>=2?GL_LINEAR:GL_NEAREST);
   glSamplerParameteri(sm,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glSamplerParameteri(sm,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE); glSamplerParameteri(sm,GL_TEXTURE_WRAP_R,GL_CLAMP_TO_EDGE);
  }
  else { glActiveTexture(GL_TEXTURE0+k); glBindSampler(k,0); glBindTexture(GL_TEXTURE_3D,r->dummy_3d); }
 }
 GLenum type=prim==1?GL_POINTS:prim==2?GL_LINES:prim==3?GL_LINE_STRIP:prim==5?GL_TRIANGLE_STRIP:prim==6?GL_TRIANGLE_FAN:GL_TRIANGLES;
 { static const char *nd,*nz; static int init; if(!init) { init=1; nd=getenv("DSR_NODRAW_PROG"); nz=getenv("DSR_NOZW_PROG"); }   /* debugging: state set, draw left out / depth not written */
   char h[16]; snprintf(h,sizeof h,"%08x",g->hash);
   if(nd && strstr(nd,h)) { glActiveTexture(GL_TEXTURE0); return; }
   if(nz && strstr(nz,h)) glDepthMask(GL_FALSE); }
 if(vb) {
  if(ni) { if(p[4]<DSR_VBUFS && vbuf[p[4]]) { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,vbuf[p[4]]);
    glDrawElementsBaseVertex(type,(GLsizei)ni,flags&2?GL_UNSIGNED_INT:GL_UNSIGNED_SHORT,(void *)(uintptr_t)p[5],(GLint)p[6]); } }
  else glDrawArrays(type,(GLint)p[7],(GLsizei)nv);
 }
 else if(ni) { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,r->pebo); glBufferData(GL_ELEMENT_ARRAY_BUFFER,ni*2,(const uint8_t *)(p+5)+(size_t)nv*ns*16,GL_STREAM_DRAW); glDrawElements(type,ni,GL_UNSIGNED_SHORT,0); }
 else glDrawArrays(type,0,nv);
 glActiveTexture(GL_TEXTURE0);
 { static uint32_t want; static int init, left=40; if(!init) { init=1; const char *e=getenv("DSR_PEEK_PROG"); want=e?(uint32_t)strtoul(e,0,16):0; }
   static uint32_t cnt; if(want==g->hash && left>0 && cnt++%150==0) { left--; float v[4]={0}; glReadPixels(0,0,1,1,GL_RGBA,GL_FLOAT,v);
     fprintf(stderr,"after %u: rt %u %ux%u vp %g %g %g %g texel0 %g %g %g %g err %#x | s0 addr %u %u border %#x filter %u %u %u | s1 addr %u %u\n",g->id,r->rt,rt->w,rt->h,r->vp[0],r->vp[1],r->vp[2],r->vp[3],v[0],v[1],v[2],v[3],glGetError(),
             r->tss[0][13],r->tss[0][14],r->tss[0][15],r->tss[0][16],r->tss[0][17],r->tss[0][18],r->tss[1][13],r->tss[1][14]); } }
}
static void draw_prog(DSRRenderer *r,const uint32_t *p,uint32_t size) { draw_prog_x(r,p,size,0); }
int dsr_gl_exec(DSRRenderer *r,uint32_t op,const uint8_t *data,uint32_t size) {
 /* Explicit checks avoid assuming enum numbering for protocol extensions. */
 uint32_t need=0;
 switch(op) {
 case DSR_RENDER_STATE: case DSR_TEXTURE: case DSR_SET_RT: need=8; break;
 case DSR_TSS: need=12; break; case DSR_TRANSFORM: need=68; break;
 case DSR_VIEWPORT: case DSR_READBACK: case DSR_SURFACE_UPLOAD: case DSR_UPLOAD_OVER: need=24; break;
 case DSR_CLEAR: need=20; break; case DSR_DRAW: need=16; break;
 case DSR_SURFACE_CREATE: need=28; break; case DSR_SURFACE_DESTROY: case DSR_PRESENT: need=4; break;
 case DSR_BLT: need=48; break; case DSR_GLSL_CREATE: need=16; break; case DSR_PROG_USE: need=4; break;
 case DSR_SHADER_CONST: need=24; break; case DSR_DRAW_PROG: need=20; break; case DSR_QUERY: need=12; break; case DSR_SCISSOR: need=16; break; case DSR_VBUF_DATA: need=16; break; case DSR_VBUF_FREE: need=4; break; case DSR_DRAW_PROG_VB: need=36; break; default: break;
 }
 if(size<need) return 0;
 const uint32_t *p=(const uint32_t *)data;
 switch(op) {
 case DSR_RENDER_STATE: if(p[0]<256) r->rs[p[0]]=p[1]; break;
 case DSR_TSS: if(p[0]<16 && p[1]<32) { r->tss[p[0]][p[1]]=p[2]; if(p[1]==12) r->tss[p[0]][13]=r->tss[p[0]][14]=p[2]; } break;
 case DSR_TEXTURE: if(p[0]<16) r->tex[p[0]]=p[1]; break;
 case DSR_TRANSFORM: if(p[0]<32) memcpy(r->xf[p[0]],p+1,64); break;
 case DSR_VIEWPORT: for(int k=0;k<4;k++) r->vp[k]=p[k]; memcpy(r->vp+4,p+4,8); break;
 case DSR_CLEAR: clear(r,p,size); break;
 case DSR_DRAW: draw(r,p,size); break;
 case DSR_SET_RT: r->rt=p[0]; r->ds=p[1]; break;
 case DSR_SURFACE_CREATE: create_surface(r,p,size); break;
 case DSR_SURFACE_DESTROY: if(p[0]<MAXSURF) { Surf *s=&r->s[p[0]]; if(s->used && s->root==p[0]) { glDeleteTextures(1,&s->tex); if(s->ms) glDeleteTextures(1,&s->ms); } s->ms=0; s->used=0; } break;
 case DSR_SURFACE_UPLOAD: upload(r,p,size,0); break;
 case DSR_UPLOAD_OVER: upload(r,p,size,1); break;
 case DSR_READBACK: readback(r,p); break;
 case DSR_BLT: if(p[5]) blit(r,p[0],(const int32_t *)(p+1),p[5],(const int32_t *)(p+6),0,0); else if(!(p[10]&0x20000)) blit(r,p[0],(const int32_t *)(p+1),0,(const int32_t *)(p+1),1,p[11]); break;
 case DSR_GLSL_CREATE: glsl_create(r,p,size); break;
 case DSR_PROG_USE: r->cur=p[0]; break;
 case DSR_SHADER_CONST: shader_consts(r,p,size); break;
 case DSR_DRAW_PROG: draw_prog(r,p,size); break;
 case DSR_MODE: if(size>=8) { r->mode_w=p[0]; r->mode_h=p[1];      /* the game's screen size: surfaces made just before (the back buffer) too */
   for(uint32_t k=1;k<MAXSURF;k++) { Surf *t=&r->s[k]; if(t->used && t->root==k && !t->ms) make_ms(t,forced_samples(r,t)); } } break;
 case DSR_VBUF_DATA: vbuf_data(p,size); break;
 case DSR_VBUF_FREE: vbuf_free(p); break;
 case DSR_DRAW_PROG_VB: draw_prog_x(r,p,size,1); break;
 case DSR_QUERY: query(r,p); break;
 case DSR_SCISSOR: memcpy(r->scissor,p,16); break;
 case DSR_PRESENT: r->presented=p[0]; dsr_gl_poll_queries(r);
  { static const char *pk; static int pk_init; if(!pk_init) { pk_init=1; pk=getenv("DSR_PEEK"); }   /* DSR_PEEK=id,id: those surfaces' first texel each present */
    if(pk) { static uint32_t n; if(n++%30==0) { const char *q=pk; fprintf(stderr,"peek %u:",n);
      while(*q) { uint32_t id=(uint32_t)strtoul(q,(char **)&q,10); Surf *t=surf(r,id); float v[4]={0};
        if(t && target_read(r,id)) glReadPixels(0,0,1,1,GL_RGBA,GL_FLOAT,v); fprintf(stderr," [%u] %g %g %g %g",id,v[0],v[1],v[2],v[3]); if(*q==',') q++; else break; }
      fprintf(stderr,"\n"); } } }
  { static double at=-1; if(at<0) { const char *e=getenv("DSR_FRAMELOG_SEC"); at=e?atof(e):0; }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); double now=ts.tv_sec+ts.tv_nsec/1e9; if(!r->t0) r->t0=now;
    static int left=-1; if(left<0) { const char *e=getenv("DSR_FRAMELOG_N"); left=e?atoi(e):1; }
    if(r->framelog==1) { fprintf(stderr,"frame: end\n"); if(--left<=0) r->framelog=2; else fprintf(stderr,"frame: begin (%.3f s)\n",now-r->t0); }
    else if(at>0 && !r->framelog && now-r->t0>=at) { r->framelog=1; fprintf(stderr,"frame: begin (%.3f s)\n",now-r->t0); } }
  return 1;
 case DSR_GAMMA: if(size>=1536) { memcpy(r->ramps,p,1536); ramp(r);
  if(getenv("DSR_COLORLOG")) fprintf(stderr,"gamma: 64=%u/%u/%u 128=%u/%u/%u 192=%u/%u/%u\n",r->ramps[0][64],r->ramps[1][64],r->ramps[2][64],r->ramps[0][128],r->ramps[1][128],r->ramps[2][128],r->ramps[0][192],r->ramps[1][192],r->ramps[2][192]);
 } break;
 default: break;
 }
 return 0;
}
void dsr_gl_destroy(DSRRenderer *r) {
 if(!r) return; for(uint32_t i=1;i<MAXSURF;i++) if(r->s[i].used && r->s[i].root==i) glDeleteTextures(1,&r->s[i].tex);
 glDeleteProgram(r->game); glDeleteProgram(r->quad); glDeleteFramebuffers(1,&r->fbo); glDeleteVertexArrays(1,&r->vao);
 glDeleteSamplers(16,r->sampler);
 GLuint b[]={r->vbo,r->ebo,r->ubo}; glDeleteBuffers(3,b); GLuint t[]={r->dummy,r->scratch,r->gamma}; glDeleteTextures(3,t); free(r);
}
