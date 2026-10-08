/* Linux virtual display modes. The game renders offscreen, so a selected custom
 * size can differ from the desktop without changing the monitor's video mode. */
#ifndef DS_DISPLAY_MODES_H
#define DS_DISPLAY_MODES_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define DS_DISPLAY_MODE_CAP 64
static int ds_parse_resolution(const char *text, int *w, int *h)
{
    if (!text) return 0;
    char *end; long x = strtol(text, &end, 10);
    if (end == text || *end != 'x' || x < 640 || x > 16384) return 0;
    const char *next = end + 1; long y = strtol(next, &end, 10);
    if (end == next || *end || y < 480 || y > 16384) return 0;
    *w = (int)x; *h = (int)y; return 1;
}
static int ds_display_modes(uint32_t out[DS_DISPLAY_MODE_CAP][2], uint32_t width, uint32_t height, const char *selected)
{
    static const uint16_t sizes[][2] = {
        {640,480},{800,600},{1024,768},{1152,864},{1280,720},{1280,800},{1280,960},{1280,1024},{1366,768},
        {1440,900},{1600,900},{1600,1200},{1680,1050},{1920,1080},{1920,1200},{2048,1152},
        {2560,1080},{2560,1440},{2560,1600},{3440,1440},{3840,1080},{3840,1600},{3840,2160},
        {5120,1440},{5120,2160},{5120,2880},{7680,2160},{7680,4320}
    };
    int n = 0, w, h;
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++)
        if (sizes[i][0] <= width && sizes[i][1] <= height) {
            out[n][0] = sizes[i][0]; out[n++][1] = sizes[i][1];
        }
    out[n][0] = width; out[n++][1] = height;
    if (ds_parse_resolution(selected, &w, &h)) { out[n][0] = w; out[n++][1] = h; }
    for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++)
        if (out[b][0] * out[b][1] < out[a][0] * out[a][1] ||
            (out[b][0] * out[b][1] == out[a][0] * out[a][1] && out[b][0] < out[a][0])) {
            uint32_t x = out[a][0], y = out[a][1];
            out[a][0] = out[b][0]; out[a][1] = out[b][1]; out[b][0] = x; out[b][1] = y;
        }
    int count = 0;
    for (int i = 0; i < n; i++)
        if (!count || out[i][0] != out[count-1][0] || out[i][1] != out[count-1][1]) {
            out[count][0] = out[i][0]; out[count++][1] = out[i][1];
        }
    return count;
}
#endif
