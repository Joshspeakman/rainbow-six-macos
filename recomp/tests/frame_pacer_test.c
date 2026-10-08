#include "../host/frame_pacer.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    DSFramePacer p; ds_pacer_init(&p,210,210,1); assert(!p.period);
    ds_pacer_init(&p,120,210,0); uint64_t start=1000000000,now=start;
    for(int i=0;i<100000;i++) {
        uint64_t deadline=ds_pacer_deadline(&p,now);
        assert(fabs((double)deadline-(start+i*(1e9/120)))<20);
        ds_pacer_advance(&p); now=deadline+2000000;
    }
    now+=50000000; assert(ds_pacer_deadline(&p,now)==now); ds_pacer_advance(&p);
    assert(ds_pacer_deadline(&p,now+1000000)>now+8000000); /* no catch-up burst */
    ds_pacer_init(&p,0,210,0); assert(ds_pacer_deadline(&p,now)==now);
    ds_pacer_init(&p,777,210,0); assert(p.period>1287000 && p.period<1288000);
    puts("frame pacing: fractional cadence, overload recovery, VSync and unlimited passed");
}
