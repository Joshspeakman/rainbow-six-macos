/* Native access to Dungeon Siege Tank archives and .gas text (runtime/win32/tank.c). */
#ifndef TANK_H
#define TANK_H
#include <stdint.h>
#include <stddef.h>
int tank_read(const char *archive, const char *path, uint8_t **out, size_t *len);   /* malloc'd; 0 = found */

typedef struct GasBlock {
    char name[64]; int nkey, nchild, capkey, capchild;
    char (*key)[48]; char (*val)[256];          /* grown as needed */
    struct GasBlock **child;
} GasBlock;
GasBlock *gas_parse(const char *text);
void gas_free(GasBlock *b);
GasBlock *gas_child(GasBlock *b, const char *name);
const char *gas_get(GasBlock *b, const char *key, const char *dflt);
#endif
