/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2001 Cluster File Systems, Inc. <info@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Generic infrastructure for managing a collection of logs.
 *
 * These logs are used for:
 *
 * - orphan recovery: OST adds record on create
 * - mtime/size consistency: the OST adds a record on first write
 * - open/unlinked objects: OST adds a record on destroy
 *
 * - mds unlink log: the MDS adds an entry upon delete
 *
 * - raid1 replication log between OST's
 * - MDS replication logs
 */

#ifndef _LUSTRE_LOG_H
#define _LUSTRE_LOG_H

#include <linux/obd.h>
#include <linux/lustre_idl.h>

#define LOG_NAME_LIMIT(logname, name)                   \
        snprintf(logname, sizeof(logname), "LOGS/%s", name)
#define LLOG_EEMPTY 4711

struct plain_handle_data {
        struct list_head    phd_entry;
        struct llog_handle *phd_cat_handle;
        struct llog_cookie  phd_cookie; /* cookie of this log in its cat */
        int                 phd_last_idx;
};

struct cat_handle_data {
        struct list_head        chd_head;
        struct llog_handle     *chd_current_log; /* currently open log */
};

/* In-memory descriptor for a log object or log catalog */
struct llog_handle {
        struct rw_semaphore     lgh_lock;
        struct llog_logid       lgh_id;              /* id of this log */
        struct llog_log_hdr    *lgh_hdr;
        struct file            *lgh_file;
        int                     lgh_last_idx;
        struct llog_ctxt       *lgh_ctxt;
        union {
                struct plain_handle_data phd;
                struct cat_handle_data   chd;
        } u;
};

/* got from mds_update_record.
 * FIXME: maybe some attribute in reint_record and update_record will be
 * changed later. */
/* XXX BUG 3188 -- must return to one set of structures. */
/* XXX use fixed-sized fields (__u32) instead of dev_t and iattr->gid_t, etc */

struct update_record {
        __u32 ur_opcode;
        __u32 ur_fsuid;
        __u32 ur_fsgid;
        dev_t ur_rdev;
        struct iattr ur_iattr;
        struct iattr ur_pattr;
        __u32 ur_flags;
        __u32 ur_len;
};

struct reint_record {
       struct update_record u_rec;
       char *rec_data1;
       int rec1_size;
       char *rec_data2;
       int rec2_size;
};

struct llog_smfs_rec {
        struct llog_rec_hdr     lsr_hdr;
        struct update_record    lsr_rec;
        struct llog_rec_tail    lsr_tail;
};

/* llog.c  -  general API */
typedef int (*llog_cb_t)(struct llog_handle *, struct llog_rec_hdr *, void *);
struct llog_handle *llog_alloc_handle(void);
void llog_free_handle(struct llog_handle *handle);
int llog_cancel_rec(struct llog_handle *loghandle, int index);
int llog_init_handle(struct llog_handle *handle, int flags,
                     struct obd_uuid *uuid);
int llog_close(struct llog_handle *cathandle);
int llog_process(struct llog_handle *loghandle, llog_cb_t cb,
                 void *data, void *catdata);
int llog_reverse_process(struct llog_handle *loghandle, llog_cb_t cb,
                         void *data, void *catdata);

/* llog_cat.c   -  catalog api */
struct llog_process_data {
        void *lpd_data;
        llog_cb_t lpd_cb;
};

struct llog_process_cat_data {
        int     first_idx;
        int     last_idx;
        /* to process catalog across zero record */
};

int llog_cat_id2handle(struct llog_handle *cathandle, struct llog_handle **res,
                       struct llog_logid *logid);
int llog_cat_put(struct llog_handle *cathandle);
int llog_cat_add_rec(struct llog_handle *cathandle, struct llog_rec_hdr *rec,
                     struct llog_cookie *reccookie, void *buf);
int llog_cat_cancel_records(struct llog_handle *cathandle, int count,
                            struct llog_cookie *cookies);
int llog_cat_process(struct llog_handle *cat_llh, llog_cb_t cb, void *data);
int llog_cat_reverse_process(struct llog_handle *cat_llh, llog_cb_t cb, void *data);
int llog_cat_set_first_idx(struct llog_handle *cathandle, int index);

int llog_catalog_add(struct llog_ctxt *ctxt, struct llog_rec_hdr *rec,
                     void *buf, struct llog_cookie *reccookie, int, void *data);
int llog_catalog_cancel(struct llog_ctxt *ctxt, int count, struct llog_cookie *,
                        int flags, void *data);
int llog_catalog_setup(struct llog_ctxt **res, char *name, struct lvfs_run_ctxt *,
                       struct fsfilt_operations *fsops, struct dentry *logs_de,
                       struct dentry *objects_de);
int llog_catalog_cleanup(struct llog_ctxt *ctxt);
int llog_cat_half_bottom(struct llog_cookie *, struct llog_handle *);

/* llog_lvfs.c */
int llog_get_cat_list(struct lvfs_run_ctxt *, struct fsfilt_operations *,
                      char *name, int count, struct llog_catid *idarray);
int llog_put_cat_list(struct lvfs_run_ctxt *, struct fsfilt_operations *, 
                      char *name, int count, struct llog_catid *idarray);
extern struct llog_operations llog_lvfs_ops;

/* llog_obd.c - obd llog api */
int obd_llog_setup(struct obd_device *obd, int index, struct obd_device *disk_obd,
                   int count,  struct llog_logid *logid,struct llog_operations *op);
int obd_llog_cleanup(struct llog_ctxt *);
int llog_obd_origin_setup(struct obd_device *obd, int index,
                          struct obd_device *disk_obd, int count,
                          struct llog_logid *logid);
int obd_llog_cat_initialize(struct obd_device *obd, int count, char *name);
int obd_llog_init(struct obd_device *obd, struct obd_device *disk_obd,
                  int count, struct llog_catid *logid);

int obd_llog_finish(struct obd_device *obd, int count);

/* llog_ioctl.c */
int llog_ioctl(struct llog_ctxt *ctxt, int cmd, struct obd_ioctl_data *data);
int llog_catalog_list(struct obd_device *obd, int count,
                     struct obd_ioctl_data *data);

/* llog_net.c */
int llog_initiator_connect(struct llog_ctxt *ctxt);
int llog_receptor_accept(struct llog_ctxt *ctxt, struct obd_import *imp);
int llog_origin_connect(struct llog_ctxt *ctxt, int count,
                        struct llog_logid *logid, struct llog_gen *gen,
                        struct obd_uuid *uuid);
int llog_handle_connect(struct ptlrpc_request *req);

/* recov_thread.c */
int llog_obd_repl_cancel(struct llog_ctxt *ctxt, int count,
                         struct llog_cookie *cookies, int flags, void *data);
                         
int llog_obd_repl_sync(struct llog_ctxt *ctxt, struct obd_export *exp);
int llog_repl_connect(struct llog_ctxt *ctxt, int count,
                      struct llog_logid *logid, struct llog_gen *gen,
                      struct obd_uuid *uuid);

struct llog_operations {
        int (*lop_setup)(struct obd_device *obd, int ctxt_idx,
                         struct obd_device *disk_obd, int count,
                         struct llog_logid *logid);
        int (*lop_cleanup)(struct llog_ctxt *ctxt);
        int (*lop_create)(struct llog_ctxt *ctxt, struct llog_handle **,
                          struct llog_logid *logid, char *name);
        int (*lop_destroy)(struct llog_handle *handle);
        int (*lop_close)(struct llog_handle *handle);

        int (*lop_read_header)(struct llog_handle *handle);
        int (*lop_add)(struct llog_ctxt *ctxt, struct llog_rec_hdr *rec,
                       void *buf, struct llog_cookie *logcookies,
                       int numcookies, void *data);
        int (*lop_cancel)(struct llog_ctxt *ctxt, int count,
                          struct llog_cookie *cookies, int flags, void *data);
        int (*lop_write_rec)(struct llog_handle *loghandle,
                             struct llog_rec_hdr *rec,
                             struct llog_cookie *logcookies, int numcookies,
                             void *, int idx);
        int (*lop_next_block)(struct llog_handle *h, int *curr_idx,
                              int next_idx, __u64 *offset, void *buf, int len);
        int (*lop_prev_block)(struct llog_handle *h,
                              int prev_idx, void *buf, int len);

        /* XXX add 2 more: commit callbacks and llog recovery functions */
        int (*lop_sync)(struct llog_ctxt *ctxt, struct obd_export *exp);
        int (*lop_connect)(struct llog_ctxt *ctxt, int count,
                           struct llog_logid *logid, struct llog_gen *gen,
                           struct obd_uuid *uuid);
};

struct llog_ctxt {
        /* needed for lvfs based log */
        struct llog_handle      *loc_handle;
        struct llog_operations  *loc_logops;
        struct fsfilt_operations *loc_fsops;
        struct dentry           *loc_logs_dir;
        struct dentry           *loc_objects_dir;
        struct lvfs_run_ctxt    *loc_lvfs_ctxt;

        struct obd_device       *loc_obd; /* points back to the containing obd*/
        struct llog_gen          loc_gen;
        int                      loc_idx; /* my index the obd array of ctxt's */

        struct obd_export       *loc_exp;
        struct obd_import       *loc_imp; /* to use in RPC's: can be backward
                                             pointing import */
        struct llog_canceld_ctxt *loc_llcd;
        struct semaphore         loc_sem; /* protects loc_llcd */
        void                    *loc_proc_cb; /* cb for recovery */
};

static inline void llog_gen_init(struct llog_ctxt *ctxt)
{
        struct obd_device *obd = ctxt->loc_exp->exp_obd;

        if (!strcmp(obd->obd_type->typ_name, "mds"))
                ctxt->loc_gen.mnt_cnt = obd->u.mds.mds_mount_count;
        else if (!strstr(obd->obd_type->typ_name, "filter"))
                ctxt->loc_gen.mnt_cnt = obd->u.filter.fo_mount_count;
        else
                ctxt->loc_gen.mnt_cnt = 0;
}

static inline int llog_gen_lt(struct llog_gen a, struct llog_gen b)
{
        if (a.mnt_cnt < b.mnt_cnt)
                return 1;
        if (a.mnt_cnt > b.mnt_cnt)
                return 0;
        return(a.conn_cnt < b.conn_cnt ? 1 : 0);
}

#define LLOG_GEN_INC(gen)  ((gen).conn_cnt) ++
#define LLOG_PROC_BREAK 0x0001

static inline int llog_ctxt2ops(struct llog_ctxt *ctxt,
                               struct llog_operations **lop)
{
        if (ctxt == NULL)
                return -ENOTCONN;

        *lop = ctxt->loc_logops;
        if (*lop == NULL)
                return -EOPNOTSUPP;

        return 0;
}

static inline int llog_handle2ops(struct llog_handle *loghandle,
                                  struct llog_operations **lop)
{
        if (loghandle == NULL)
                return -EINVAL;

        return llog_ctxt2ops(loghandle->lgh_ctxt, lop);
}

static inline int llog_data_len(int len)
{
        return size_round(len);
}

static inline struct llog_ctxt *llog_get_context(struct obd_device *obd,
                                                 int index)
{
        if (index < 0 || index >= LLOG_MAX_CTXTS)
                return NULL;

        return obd->obd_llog_ctxt[index];
}

static inline int llog_create(struct llog_ctxt *ctxt, struct llog_handle **res,
                              struct llog_logid *logid, char *name)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_ctxt2ops(ctxt, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_create == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_create(ctxt, res, logid, name);
        RETURN(rc);
}

static inline int llog_destroy(struct llog_handle *handle)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_handle2ops(handle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_destroy == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_destroy(handle);
        RETURN(rc);
}

static inline int llog_read_header(struct llog_handle *handle)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_handle2ops(handle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_read_header == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_read_header(handle);
        RETURN(rc);
}

static inline int llog_add(struct llog_ctxt *ctxt, struct llog_rec_hdr *rec,
                           void *buf, struct llog_cookie *logcookies,
                           int numcookies, void *data)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_ctxt2ops(ctxt, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_add == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_add(ctxt, rec, buf, logcookies, numcookies, data);
        RETURN(rc);
}

static inline int llog_cancel(struct llog_ctxt *ctxt, int count,
                              struct llog_cookie *cookies, int flags, void *data)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_ctxt2ops(ctxt, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_cancel == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_cancel(ctxt, count, cookies, flags, data);
        RETURN(rc);
}

static inline int llog_write_rec(struct llog_handle *handle,
                                 struct llog_rec_hdr *rec,
                                 struct llog_cookie *logcookies,
                                 int numcookies, void *buf, int idx)
{
        struct llog_operations *lop;
        int rc, buflen;
        ENTRY;

        rc = llog_handle2ops(handle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_write_rec == NULL)
                RETURN(-EOPNOTSUPP);

        if (buf)
                buflen = le32_to_cpu(rec->lrh_len) + sizeof(struct llog_rec_hdr)
                                + sizeof(struct llog_rec_tail);
        else
                buflen = le32_to_cpu(rec->lrh_len);
        LASSERT(size_round(buflen) == buflen);

        rc = lop->lop_write_rec(handle, rec, logcookies, numcookies, buf, idx);
        RETURN(rc);
}

static inline int llog_next_block(struct llog_handle *loghandle, int *curr_idx,
                                  int next_idx, __u64 *curr_offset, void *buf,
                                  int len)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_handle2ops(loghandle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_next_block == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_next_block(loghandle, curr_idx, next_idx, curr_offset, buf,
                                 len);
        RETURN(rc);
}

static inline int llog_prev_block(struct llog_handle *loghandle,
                                  int prev_idx, void *buf, int len)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_handle2ops(loghandle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_prev_block == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_prev_block(loghandle, prev_idx, buf, len);
        RETURN(rc);
}

static inline int llog_connect(struct llog_ctxt *ctxt, int count,
                               struct llog_logid *logid, struct llog_gen *gen,
                               struct obd_uuid *uuid)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_ctxt2ops(ctxt, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_connect == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_connect(ctxt, count, logid, gen, uuid);
        RETURN(rc);
}

static inline int llog_sync(struct llog_ctxt *ctxt, struct obd_export *exp)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_ctxt2ops(ctxt, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_sync == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_sync(ctxt, exp);
        RETURN(rc);
}

#endif
