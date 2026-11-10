/* Full 16-bit domain and padded palettized/32-bit textures vs scalar reference. */
#include "../runtime/win32/ddraw_pal.c"
#include <assert.h>
#include <time.h>
uint8_t *G_MEM;
static uint32_t reference(uint32_t v,uint32_t m) {
 if(!m)return 255;unsigned bits=__builtin_popcount(m),x=(v&m)>>__builtin_ctz(m);
 if(bits>=8)return x>>(bits-8);unsigned max=(1u<<bits)-1;return (x*255+max/2)/max;
}
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void) {
 G_MEM=calloc(1,1024*1024);assert(G_MEM);uint32_t out[65536];PPal pal={0};
 for(unsigned i=0;i<256;i++)pal.entries[i]=i*0x15379u;
 uint32_t formats[][4]={{0,0xf800,0x7e0,0x1f},{0x8000,0x7c00,0x3e0,31},{0xf000,0xf00,0xf0,15},
 {0,0x7c00,0x3e0,31},{0xff000000,0xff0000,0xff00,0xff},{0xc0000000,0x3ff00000,0xffc00,0x3ff},
 {0,0x10001,0x20202,0x40404},{0xffffffff,0,0,0}};
 for(unsigned fmt=0;fmt<8;fmt++)for(unsigned depth=0;depth<3;depth++) {
  unsigned bpp=depth==0?8:depth==1?16:32;
  PSurf s={.w=256,.h=256,.pitch=256*(bpp/8)+8,.bpp=bpp,.mem=64,.pal=&pal,.has_ck=1,.ck_lo=12,.ck_hi=33};
  s.pf[1]=1;for(int k=0;k<4;k++)s.pf[k?3+k:7]=formats[fmt][k];
  for(unsigned y=0;y<256;y++)for(unsigned x=0;x<256;x++) {
   uint32_t v=y*256+x;if(bpp==32)v=v*2654435761u;memcpy(G_MEM+s.mem+y*s.pitch+x*(bpp/8),&v,bpp/8);
  }
  surf_to_bgra(&s,out);
  for(unsigned y=0;y<256;y++)for(unsigned x=0;x<256;x++) {
   uint32_t v=0;memcpy(&v,G_MEM+s.mem+y*s.pitch+x*(bpp/8),bpp/8);uint32_t expected;
   if(bpp==8){uint32_t e=pal.entries[v];expected=0xff000000u|(e&255)<<16|(e&0xff00)|(e>>16&255);}
   else expected=reference(v,formats[fmt][0])<<24|reference(v,formats[fmt][1])<<16|reference(v,formats[fmt][2])<<8|reference(v,formats[fmt][3]);
   if(v>=12&&v<=33)expected&=0xffffff;assert(out[y*256+x]==expected);
  }
 }
 PSurf s={.w=256,.h=256,.pitch=512,.bpp=16,.mem=64};s.pf[1]=1;s.pf[4]=0xf00;s.pf[5]=0xf0;s.pf[6]=15;s.pf[7]=0xf000;
 double start=seconds();for(int i=0;i<500;i++)surf_to_bgra(&s,out);
 printf("texture conversion: parity passed; 32.8M pixels %.6f seconds (%x)\n",seconds()-start,out[123]);free(G_MEM);
}
