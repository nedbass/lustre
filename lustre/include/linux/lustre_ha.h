/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */

#ifndef _LUSTRE_HA_H
#define _LUSTRE_HA_H

#include <linux/lustre_net.h>

#define MGR_STOPPING   1
#define MGR_RUNNING    2
#define MGR_STOPPED    4
#define MGR_KILLED     8
#define MGR_EVENT      16
#define MGR_WORKING    32
#define MGR_SIGNAL     64

#define LUSTRE_HA_NAME "ptlrpc"

#define CONNMGR_CONNECT 1

extern struct recovd_obd *ptlrpc_connmgr;

struct connmgr_thread {
        struct recovd_obd *mgr;
        char *name;
};

struct connmgr_body {
        __u64 conn;
        __u64 conn_token;
        __u32 generation;
};

int connmgr_connect(struct recovd_obd *mgr, struct ptlrpc_connection *conn);
int connmgr_handle(struct obd_device *dev, struct ptlrpc_service *svc,
                   struct ptlrpc_request *req);
void connmgr_cli_fail(struct ptlrpc_client *cli);
void connmgr_cli_manage(struct recovd_obd *mgr, struct ptlrpc_client *cli);
int recovd_setup(struct recovd_obd *mgr);
int recovd_cleanup(struct recovd_obd *mgr);

#endif
