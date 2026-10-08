/* The Direct3D front ends' shared producer side (implemented in ddraw.c): the command ring to the Metal renderer
 * (src/dsr/dsr_proto.h), renderer surface ids, COM-style interface objects, and presenting a frame. */
#ifndef DSR_PRODUCER_H
#define DSR_PRODUCER_H
#include "w32.h"
uint32_t *cmd_begin(uint32_t op, uint32_t payload);   /* a record of payload bytes; fill it, then cmd_end() */
void cmd_end(void);
int cmd_readback(uint32_t id, const int32_t *r, uint8_t *dst, uint32_t pitch, uint32_t bpp);   /* GPU -> CPU, synchronous */
uint32_t next_object_id(void);                         /* a renderer surface id */
void surface_id_free(uint32_t id);
uint32_t fvf_stride(uint32_t fvf);
extern uint32_t vt[64];                                /* guest vtables by interface type */
uint32_t make_iface(int type, void *p);                /* a guest interface pointer for a host object */
void *obj(uint32_t g, int type);                       /* the host object behind a guest interface (0 if not that type) */
void obj_drop(uint32_t g);
void dsr_present(Ctx *c, uint32_t id);
extern uint32_t dsr_d3d9_w, dsr_d3d9_h;
#endif
