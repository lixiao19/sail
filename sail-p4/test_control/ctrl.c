#include "headers.h"

static void setup_switchd(bf_switchd_context_t *ctx)
{
    char *conf_file;
    const char *install_dir = getenv("SDE_INSTALL");

    assert(install_dir != NULL);
    ctx->install_dir = strdup(install_dir);
    assert(ctx->install_dir != NULL);
    assert(asprintf(&conf_file, "%s/share/p4/targets/tofino/%s.conf",
                    install_dir, P4_PROG_NAME) >= 0);
    ctx->conf_file = conf_file;
    ctx->running_in_background = true;
    ctx->dev_sts_thread = true;
    ctx->dev_sts_port = 7777;
    assert(bf_switchd_lib_init(ctx) == BF_SUCCESS);
}

static bf_pal_front_port_handle_t front_port(bf_dev_id_t dev_id,
                                              const char *name)
{
    bf_pal_front_port_handle_t handle;

    assert(bf_pm_port_str_to_hdl_get(dev_id, name, &handle) == BF_SUCCESS);
    return handle;
}

static void setup_25g_ports(bf_dev_id_t dev_id)
{
    for (size_t i = 0; i < ARRLEN(DATA_PORTS); i++) {
        bf_pal_front_port_handle_t handle = front_port(dev_id,
                                                       DATA_PORTS[i].fp_port);
        assert(bf_pm_port_add(dev_id, &handle, BF_SPEED_25G,
                              BF_FEC_TYP_NONE) == BF_SUCCESS);
        assert(bf_pm_port_enable(dev_id, &handle) == BF_SUCCESS);
        printf("enabled 25G port %s\n", DATA_PORTS[i].fp_port);
    }
}

static void setup_loopback_ports(bf_dev_id_t dev_id)
{
    for (size_t i = 0; i < ARRLEN(LOOP_PORTS); i++) {
        bf_pal_front_port_handle_t handle = front_port(dev_id,
                                                       LOOP_PORTS[i].fp_port);
        assert(bf_pm_port_loopback_mode_set(dev_id, &handle,
                                            BF_LPBK_MAC_NEAR) == BF_SUCCESS);
        printf("enabled MAC-near loopback on %s\n", LOOP_PORTS[i].fp_port);
    }
}

static void setup_multicast(bf_dev_id_t dev_id)
{
    bf_mc_session_hdl_t session;
    bf_mc_mgrp_hdl_t group;
    bf_mc_node_hdl_t node;
    bf_mc_port_map_t ports;
    bf_mc_lag_map_t lags;

    assert(bf_mc_create_session(&session) == BF_SUCCESS);
    assert(bf_mc_mgrp_create(session, dev_id, DUMMY_MC_GID,
                             &group) == BF_SUCCESS);
    BF_MC_PORT_MAP_INIT(ports);
    BF_MC_LAG_MAP_INIT(lags);
    for (size_t i = 0; i < ARRLEN(DUMMY_MC_PORTS); i++) {
        bf_dev_port_t dev_port;
        assert(bf_pm_port_str_to_dev_port_get(
                   dev_id, (char *)DUMMY_MC_PORTS[i].fp_port,
                   &dev_port) == BF_SUCCESS);
        BF_MC_PORT_MAP_SET(ports, dev_port);
    }
    assert(bf_mc_node_create(session, dev_id, DUMMY_MC_RID, ports, lags,
                             &node) == BF_SUCCESS);
    assert(bf_mc_associate_node(session, dev_id, group, node, false,
                                0) == BF_SUCCESS);
    assert(bf_mc_complete_operations(session) == BF_SUCCESS);
    assert(bf_mc_destroy_session(session) == BF_SUCCESS);
    printf("configured dummy multicast group %u\n", DUMMY_MC_GID);
}

static void setup_notify_mirror(bf_dev_id_t dev_id)
{
    p4_pd_sess_hdl_t session;
    p4_pd_dev_target_t target = {dev_id, ALL_PIPES};
    p4_pd_mirror_session_info_t info = {
        PD_MIRROR_TYPE_NORM,
        PD_DIR_INGRESS,
        NOTIFY_MIRROR_SID,
        NOTIFY_MIRROR_PORT,
        true,
        0,
        PD_COLOR_GREEN,
        0,
        false,
        0,
        false,
        NOTIFY_MIRROR_MAX_PKT_LEN,
        0,
        0,
        0,
        0,
        0,
        0,
        false,
        0,
        0,
        NULL,
        0,
    };

    assert(p4_pd_client_init(&session) == BF_SUCCESS);
    assert(p4_pd_mirror_session_create(session, target, &info) == BF_SUCCESS);
    assert(p4_pd_complete_operations(session) == BF_SUCCESS);
    printf("configured notify mirror session %u -> port %u\n",
           NOTIFY_MIRROR_SID, NOTIFY_MIRROR_PORT);
}

int main(void)
{
    bf_switchd_context_t *ctx = calloc(1, sizeof(*ctx));

    assert(ctx != NULL);
    setup_switchd(ctx);
    setup_25g_ports(0);
    setup_loopback_ports(0);
    setup_multicast(0);
    setup_notify_mirror(0);
    printf("SAIL control plane initialized; press Ctrl-C to stop\n");
    for (;;)
        pause();
}
