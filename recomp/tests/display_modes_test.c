#include "../runtime/win32/display_modes.h"
#include <assert.h>
static int has(uint32_t modes[][2], int n, int w, int h) {
    int count=0; for(int i=0;i<n;i++) count+=modes[i][0]==(unsigned)w && modes[i][1]==(unsigned)h;
    return count;
}
int main(void) {
    uint32_t m[DS_DISPLAY_MODE_CAP][2]; int n=ds_display_modes(m,5120,1440,"3440x1440");
    assert(has(m,n,5120,1440)==1 && has(m,n,3440,1440)==1 && has(m,n,2560,1080)==1);
    assert(!has(m,n,3840,2160));
    n=ds_display_modes(m,2560,1440,"5120x1440");
    assert(has(m,n,5120,1440)==1); /* custom must be enumerated even on a smaller desktop */
    n=ds_display_modes(m,7680,4320,"7680x4320");
    assert(n<DS_DISPLAY_MODE_CAP && has(m,n,7680,4320)==1 && has(m,n,7680,2160)==1);
    int w,h;
    assert(!ds_parse_resolution("4294970736x1440",&w,&h));
    assert(!ds_parse_resolution("3440x1440junk",&w,&h));
    assert(!ds_parse_resolution("3440x0",&w,&h));
    puts("ultrawide/native/custom mode enumeration and bounds: passed");
}
