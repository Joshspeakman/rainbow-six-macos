#ifndef DS_LINUX_RENDERER_H
#define DS_LINUX_RENDERER_H
#include <stdint.h>
#include <stddef.h>
typedef struct DSRRenderer DSRRenderer;
DSRRenderer *dsr_gl_create(const char *shader_dir);
void dsr_gl_destroy(DSRRenderer *r);
int dsr_gl_exec(DSRRenderer *r, uint32_t op, const uint8_t *data, uint32_t size);
void dsr_gl_present(DSRRenderer *r, int width, int height);
void dsr_gl_size(DSRRenderer *r, uint32_t *w, uint32_t *h);
void dsr_gl_queries(DSRRenderer *r, volatile uint32_t *results);   /* the header's occlusion-query area (dsr_shm.h) */
void dsr_gl_poll_queries(DSRRenderer *r);
void dsr_gl_readback(DSRRenderer *r, void *data, size_t size, volatile uint32_t *done);
int dsr_gl_screenshot(DSRRenderer *r, const char *path);
int dsr_gl_display_screenshot(const char *path, int width, int height);
extern long dsr_stat[8];
#endif
