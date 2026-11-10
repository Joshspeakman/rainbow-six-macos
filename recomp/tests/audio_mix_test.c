/* Resampler parity at format/rate/loop boundaries, including tiny rings. */
#include "../runtime/win32/dsound.c"
#include <assert.h>
#include <time.h>
uint8_t *G_MEM;
void w32_audio_out(const float *p,uint32_t n,int rate,int source){(void)p;(void)n;(void)rate;(void)source;}
static void test_render(float *samples, uint32_t frames) { render(NULL,samples,frames); }
static float reference_sample(Buf *b,unsigned frame,int ch) {
 unsigned at=(frame%(b->size/b->align))*b->align+(b->ch==2?ch:0)*(b->bits/8);
 return b->bits==16?(int16_t)rt_r16(G_MEM,b->mem+at)/32768.f:((int)G_MEM[b->mem+at]-128)/128.f;
}
int main(void) {
 unsetenv("W32_AUDIO_MUTE");G_MEM=calloc(1,1048576);assert(G_MEM);primary_playing=1;
 for(unsigned i=0;i<1048576;i++)G_MEM[i]=(i*131+17)&255;
 unsigned rates[]={11025,22050,44100,48000,96000,1000000},sizes[]={1,2,31,1024};
 for(unsigned fmt=0;fmt<4;fmt++)for(unsigned rate=0;rate<6;rate++)for(unsigned size=0;size<4;size++)for(unsigned loop=0;loop<2;loop++) {
  memset(bufs,0,sizeof bufs);Buf *b=&bufs[0];b->used=1;b->playing=1;b->looping=loop;b->mem=64;
  b->bits=fmt&1?16:8;b->ch=fmt&2?2:1;b->align=b->ch*b->bits/8;b->size=sizes[size]*b->align;
  b->rate=rates[rate];b->gain_l=.4;b->gain_r=.7;b->frac=.375;
  unsigned pos=0;double fr=b->frac,step=(double)b->rate/OUT_RATE;int playing=1;
  float expected[1026]={0},actual[1026];
  for(unsigned i=0;i<513;i++) {
   unsigned p0=pos,p1=pos+1;if(!loop && p1>=sizes[size])p1=p0;float t=(float)fr;
   float l=reference_sample(b,p0,0)*(1-t)+reference_sample(b,p1,0)*t;
   float r=reference_sample(b,p0,1)*(1-t)+reference_sample(b,p1,1)*t;
   expected[2*i]=(float)(l*b->gain_l);expected[2*i+1]=(float)(r*b->gain_r);
   fr+=step;while(fr>=1){fr-=1;pos++;}
   if(pos>=sizes[size]){if(loop)pos-=sizes[size];else{playing=0;pos=0;break;}}
  }
  test_render(actual,513);assert(!memcmp(expected,actual,sizeof actual));
  assert(b->play==pos*b->align && b->frac==fr && b->playing==playing);
 }
 memset(bufs,0,sizeof bufs);
 for(int k=0;k<32;k++) {
  Buf *b=&bufs[k];b->used=1;b->playing=1;b->looping=1;b->mem=64;b->bits=16;b->ch=2;b->align=4;
  b->size=4096;b->rate=48000;b->gain_l=.01;b->gain_r=.02;
 }
 float samples[1024];struct timespec a,z;clock_gettime(CLOCK_MONOTONIC,&a);
 for(int k=0;k<1000;k++)test_render(samples,512);
 clock_gettime(CLOCK_MONOTONIC,&z);
 printf("audio: exact PCM/cursor parity across 192 scenarios; 32 voices x 1000 blocks %.6f seconds\n",z.tv_sec-a.tv_sec+(z.tv_nsec-a.tv_nsec)*1e-9);
 free(G_MEM);
}
