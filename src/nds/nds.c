#include <stdlib.h>
#include "nds.h"

nds_t *nds_create(void)
{
    nds_t *nds = calloc(1, sizeof(nds_t));
    if (nds == NULL)
        return NULL;

    /* 总线是其它模块的依赖，先建；失败则整体失败 */
    nds->bus = bus_create();
    if (nds->bus == NULL) {
        free(nds);
        return NULL;
    }
    return nds;
}

void nds_destroy(nds_t *nds)
{
    if (nds == NULL)
        return;
    /* 逆序清理：后建的先释放（此处先清 bus） */
    bus_destroy(nds->bus);
    free(nds);
}
