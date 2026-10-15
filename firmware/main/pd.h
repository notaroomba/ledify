#pragma once
#include <stdbool.h>
#include "driver/i2c_master.h"

typedef struct {
    int cc;              // 1 or 2 = cable orientation, 0 = nothing attached
    bool vbus;
    bool ready;          // a contract (or a reject) has been received
    int contract_mv, contract_ma;
    int n_pdos;
    int pdo_mv[7], pdo_ma[7];
} pd_state_t;

void pd_init(i2c_master_bus_handle_t bus);   // also starts the PD task
const pd_state_t *pd_get(void);
bool pd_request(int min_mv);                  // renegotiate: most power at min_mv or above; false if nothing offered
