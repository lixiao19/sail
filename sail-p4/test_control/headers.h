#ifndef SAIL_CONTROL_HEADERS_H
#define SAIL_CONTROL_HEADERS_H

#define _GNU_SOURCE

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bf_pm/bf_pm_intf.h>
#include <bf_switchd/bf_switchd.h>
#include <mc_mgr/mc_mgr_intf.h>
#include <tofino/pdfixed/pd_conn_mgr.h>
#include <tofino/pdfixed/pd_mirror.h>

#include "../config.h"

#define ARRLEN(a) (sizeof(a) / sizeof((a)[0]))

typedef struct switch_port_s {
    const char *fp_port;
} switch_port_t;

static const switch_port_t DATA_PORTS[] = {
    {"8/0"}, {"10/0"}, {"14/0"}, {"16/0"}, {"1/0"}, {"2/0"},
};

static const switch_port_t LOOP_PORTS[] = {
    {"1/0"}, {"2/0"},
};

static const switch_port_t DUMMY_MC_PORTS[] = {
    {"2/0"},
};

#endif
