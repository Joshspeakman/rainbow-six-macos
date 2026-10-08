// DSR native renderer: replays the command stream (dsr_proto.h) with Metal.
#import <Metal/Metal.h>
#include <stdint.h>
typedef struct DSRRenderer DSRRenderer;
DSRRenderer *dsr_renderer_create(id<MTLDevice> dev);
// Consume one command; returns 1 when it was a PRESENT (frame boundary).
int dsr_renderer_exec(DSRRenderer *r, uint32_t op, const uint8_t *payload, uint32_t size);
// The texture last presented (gamma not yet applied), and the command buffer that produced it.
id<MTLTexture> dsr_renderer_presented(DSRRenderer *r);
void dsr_renderer_flush(DSRRenderer *r, int wait);
id<MTLCommandBuffer> dsr_renderer_present_to(DSRRenderer *r, id<MTLTexture> dst);
void dsr_renderer_presented_size(DSRRenderer *r, uint32_t *w, uint32_t *h);
void dsr_renderer_set_readback(DSRRenderer *r, void *area, size_t size, volatile uint32_t *done);
double dsr_renderer_present_time(DSRRenderer *r);   // game-side seconds (QPC) of the last PRESENT, or -1
void dsr_renderer_set_graph(DSRRenderer *r, const float *a, int ha, const float *b, int hb);
extern int dsr_verbose;
