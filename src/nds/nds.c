#include <stdlib.h>
#include "nds.h"

nds_t *nds_create(void)
{
    return calloc(1, sizeof(nds_t));
}

void nds_destroy(nds_t *nds)
{
    free(nds);
}
