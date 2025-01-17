/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * Copyright (c) 2011 Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/lov/lov_ea.c
 *
 * Author: Wang Di <wangdi@clusterfs.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_LOV

#ifdef __KERNEL__
#include <asm/div64.h>
#include <libcfs/libcfs.h>
#else
#include <liblustre.h>
#endif

#include <obd_class.h>
#include <obd_lov.h>
#include <lustre/lustre_idl.h>
#include <lustre_log.h>

#include "lov_internal.h"

struct lovea_unpack_args {
        struct lov_stripe_md *lsm;
        int                   cursor;
};

static int lsm_lmm_verify_common(struct lov_mds_md *lmm, int lmm_bytes,
                                 int stripe_count)
{

        if (stripe_count == 0 || stripe_count > LOV_V1_INSANE_STRIPE_COUNT) {
                CERROR("bad stripe count %d\n", stripe_count);
                lov_dump_lmm(D_WARNING, lmm);
                return -EINVAL;
        }

        if (lmm->lmm_object_id == 0) {
                CERROR("zero object id\n");
                lov_dump_lmm(D_WARNING, lmm);
                return -EINVAL;
        }

        if (lmm->lmm_pattern != cpu_to_le32(LOV_PATTERN_RAID0)) {
                CERROR("bad striping pattern\n");
                lov_dump_lmm(D_WARNING, lmm);
                return -EINVAL;
        }

        if (lmm->lmm_stripe_size == 0 ||
             (le32_to_cpu(lmm->lmm_stripe_size)&(LOV_MIN_STRIPE_SIZE-1)) != 0) {
                CERROR("bad stripe size %u\n",
                       le32_to_cpu(lmm->lmm_stripe_size));
                lov_dump_lmm(D_WARNING, lmm);
                return -EINVAL;
        }
        return 0;
}

struct lov_stripe_md *lsm_alloc_plain(int stripe_count, int *size)
{
        struct lov_stripe_md *lsm;
        int i, oinfo_ptrs_size;
        struct lov_oinfo *loi;

        LASSERT(stripe_count > 0);

        oinfo_ptrs_size = sizeof(struct lov_oinfo *) * stripe_count;
        *size = sizeof(struct lov_stripe_md) + oinfo_ptrs_size;

        OBD_ALLOC_LARGE(lsm, *size);
        if (!lsm)
                return NULL;;

        for (i = 0; i < stripe_count; i++) {
                OBD_SLAB_ALLOC_PTR_GFP(loi, lov_oinfo_slab, CFS_ALLOC_IO);
                if (loi == NULL)
                        goto err;
                lsm->lsm_oinfo[i] = loi;
        }
        lsm->lsm_stripe_count = stripe_count;
        lsm->lsm_pool_name[0] = '\0';
        return lsm;

err:
        while (--i >= 0)
                OBD_SLAB_FREE(lsm->lsm_oinfo[i], lov_oinfo_slab, sizeof(*loi));
        OBD_FREE_LARGE(lsm, *size);
        return NULL;
}

void lsm_free_plain(struct lov_stripe_md *lsm)
{
        int stripe_count = lsm->lsm_stripe_count;
        int i;

        for (i = 0; i < stripe_count; i++)
                OBD_SLAB_FREE(lsm->lsm_oinfo[i], lov_oinfo_slab,
                              sizeof(struct lov_oinfo));
        OBD_FREE_LARGE(lsm, sizeof(struct lov_stripe_md) +
                       stripe_count * sizeof(struct lov_oinfo *));
}

static void lsm_unpackmd_common(struct lov_stripe_md *lsm,
                                struct lov_mds_md *lmm)
{
        /*
         * This supposes lov_mds_md_v1/v3 first fields are
         * are the same
         */
        lsm->lsm_object_id = le64_to_cpu(lmm->lmm_object_id);
        lsm->lsm_object_seq = le64_to_cpu(lmm->lmm_object_seq);
        lsm->lsm_stripe_size = le32_to_cpu(lmm->lmm_stripe_size);
        lsm->lsm_pattern = le32_to_cpu(lmm->lmm_pattern);
        lsm->lsm_pool_name[0] = '\0';
}

static void
lsm_stripe_by_index_plain(struct lov_stripe_md *lsm, int *stripeno,
                           obd_off *lov_off, obd_off *swidth)
{
        if (swidth)
                *swidth = (obd_off)lsm->lsm_stripe_size * lsm->lsm_stripe_count;
}

static void
lsm_stripe_by_offset_plain(struct lov_stripe_md *lsm, int *stripeno,
                           obd_off *lov_off, obd_off *swidth)
{
        if (swidth)
                *swidth = (obd_off)lsm->lsm_stripe_size * lsm->lsm_stripe_count;
}

static int lsm_destroy_plain(struct lov_stripe_md *lsm, struct obdo *oa,
                             struct obd_export *md_exp)
{
        return 0;
}

static int lsm_lmm_verify_v1(struct lov_mds_md_v1 *lmm, int lmm_bytes,
                             int *stripe_count)
{
        if (lmm_bytes < sizeof(*lmm)) {
                CERROR("lov_mds_md_v1 too small: %d, need at least %d\n",
                       lmm_bytes, (int)sizeof(*lmm));
                return -EINVAL;
        }

        *stripe_count = le32_to_cpu(lmm->lmm_stripe_count);

        if (lmm_bytes < lov_mds_md_size(*stripe_count, LOV_MAGIC_V1)) {
                CERROR("LOV EA V1 too small: %d, need %d\n",
                       lmm_bytes, lov_mds_md_size(*stripe_count, LOV_MAGIC_V1));
                lov_dump_lmm_v1(D_WARNING, lmm);
                return -EINVAL;
        }

        return lsm_lmm_verify_common(lmm, lmm_bytes, *stripe_count);
}

int lsm_unpackmd_v1(struct lov_obd *lov, struct lov_stripe_md *lsm,
                    struct lov_mds_md_v1 *lmm)
{
        struct lov_oinfo *loi;
        int i;
        __u64 stripe_maxbytes = OBD_OBJECT_EOF;

        lsm_unpackmd_common(lsm, lmm);

        for (i = 0; i < lsm->lsm_stripe_count; i++) {
                struct obd_import *imp;

                /* XXX LOV STACKING call down to osc_unpackmd() */
                loi = lsm->lsm_oinfo[i];
                loi->loi_id = le64_to_cpu(lmm->lmm_objects[i].l_object_id);
                loi->loi_seq = le64_to_cpu(lmm->lmm_objects[i].l_object_seq);
                loi->loi_ost_idx = le32_to_cpu(lmm->lmm_objects[i].l_ost_idx);
                loi->loi_ost_gen = le32_to_cpu(lmm->lmm_objects[i].l_ost_gen);
                if (loi->loi_ost_idx >= lov->desc.ld_tgt_count) {
                        CERROR("OST index %d more than OST count %d\n",
                               loi->loi_ost_idx, lov->desc.ld_tgt_count);
                        lov_dump_lmm_v1(D_WARNING, lmm);
                        return -EINVAL;
                }
                if (!lov->lov_tgts[loi->loi_ost_idx]) {
                        CERROR("OST index %d missing\n", loi->loi_ost_idx);
                        lov_dump_lmm_v1(D_WARNING, lmm);
                        return -EINVAL;
                }
                /* calculate the minimum stripe max bytes */
                imp = lov->lov_tgts[loi->loi_ost_idx]->ltd_obd->u.cli.cl_import;
                if (imp != NULL) {
                        if (!(imp->imp_connect_data.ocd_connect_flags &
                              OBD_CONNECT_MAXBYTES)) {
                                imp->imp_connect_data.ocd_maxbytes =
                                                         LUSTRE_STRIPE_MAXBYTES;
                        }
                        if (stripe_maxbytes>imp->imp_connect_data.ocd_maxbytes){
                                stripe_maxbytes =
                                             imp->imp_connect_data.ocd_maxbytes;
                        }
                }
        }

        /* no ost connected yet */
        if (stripe_maxbytes == OBD_OBJECT_EOF)
                stripe_maxbytes = LUSTRE_STRIPE_MAXBYTES;
        lsm->lsm_maxbytes = stripe_maxbytes * lsm->lsm_stripe_count;

        return 0;
}

const struct lsm_operations lsm_v1_ops = {
        .lsm_free            = lsm_free_plain,
        .lsm_destroy         = lsm_destroy_plain,
        .lsm_stripe_by_index    = lsm_stripe_by_index_plain,
        .lsm_stripe_by_offset   = lsm_stripe_by_offset_plain,
        .lsm_lmm_verify         = lsm_lmm_verify_v1,
        .lsm_unpackmd           = lsm_unpackmd_v1,
};

static int lsm_lmm_verify_v3(struct lov_mds_md *lmmv1, int lmm_bytes,
                             int *stripe_count)
{
        struct lov_mds_md_v3 *lmm;

        lmm = (struct lov_mds_md_v3 *)lmmv1;

        if (lmm_bytes < sizeof(*lmm)) {
                CERROR("lov_mds_md_v3 too small: %d, need at least %d\n",
                       lmm_bytes, (int)sizeof(*lmm));
                return -EINVAL;
        }

        *stripe_count = le32_to_cpu(lmm->lmm_stripe_count);

        if (lmm_bytes < lov_mds_md_size(*stripe_count, LOV_MAGIC_V3)) {
                CERROR("LOV EA V3 too small: %d, need %d\n",
                       lmm_bytes, lov_mds_md_size(*stripe_count, LOV_MAGIC_V3));
                lov_dump_lmm_v3(D_WARNING, lmm);
                return -EINVAL;
        }

        return lsm_lmm_verify_common((struct lov_mds_md_v1 *)lmm, lmm_bytes,
                                     *stripe_count);
}

int lsm_unpackmd_v3(struct lov_obd *lov, struct lov_stripe_md *lsm,
                    struct lov_mds_md *lmmv1)
{
        struct lov_mds_md_v3 *lmm;
        struct lov_oinfo *loi;
        int i;
        __u64 stripe_maxbytes = OBD_OBJECT_EOF;

        lmm = (struct lov_mds_md_v3 *)lmmv1;

        lsm_unpackmd_common(lsm, (struct lov_mds_md_v1 *)lmm);
        strncpy(lsm->lsm_pool_name, lmm->lmm_pool_name, LOV_MAXPOOLNAME);

        for (i = 0; i < lsm->lsm_stripe_count; i++) {
                struct obd_import *imp;

                /* XXX LOV STACKING call down to osc_unpackmd() */
                loi = lsm->lsm_oinfo[i];
                loi->loi_id = le64_to_cpu(lmm->lmm_objects[i].l_object_id);
                loi->loi_seq = le64_to_cpu(lmm->lmm_objects[i].l_object_seq);
                loi->loi_ost_idx = le32_to_cpu(lmm->lmm_objects[i].l_ost_idx);
                loi->loi_ost_gen = le32_to_cpu(lmm->lmm_objects[i].l_ost_gen);
                if (loi->loi_ost_idx >= lov->desc.ld_tgt_count) {
                        CERROR("OST index %d more than OST count %d\n",
                               loi->loi_ost_idx, lov->desc.ld_tgt_count);
                        lov_dump_lmm_v3(D_WARNING, lmm);
                        return -EINVAL;
                }
                if (!lov->lov_tgts[loi->loi_ost_idx]) {
                        CERROR("OST index %d missing\n", loi->loi_ost_idx);
                        lov_dump_lmm_v3(D_WARNING, lmm);
                        return -EINVAL;
                }
                /* calculate the minimum stripe max bytes */
                imp = lov->lov_tgts[loi->loi_ost_idx]->ltd_obd->u.cli.cl_import;
                if (imp != NULL) {
                        if (!(imp->imp_connect_data.ocd_connect_flags &
                              OBD_CONNECT_MAXBYTES)) {
                                imp->imp_connect_data.ocd_maxbytes =
                                                         LUSTRE_STRIPE_MAXBYTES;
                        }
                        if (stripe_maxbytes>imp->imp_connect_data.ocd_maxbytes){
                                stripe_maxbytes =
                                             imp->imp_connect_data.ocd_maxbytes;
                        }
                }
        }

        /* no ost connected yet */
        if (stripe_maxbytes == OBD_OBJECT_EOF)
                stripe_maxbytes = LUSTRE_STRIPE_MAXBYTES;
        lsm->lsm_maxbytes = stripe_maxbytes * lsm->lsm_stripe_count;

        return 0;
}

const struct lsm_operations lsm_v3_ops = {
        .lsm_free            = lsm_free_plain,
        .lsm_destroy         = lsm_destroy_plain,
        .lsm_stripe_by_index    = lsm_stripe_by_index_plain,
        .lsm_stripe_by_offset   = lsm_stripe_by_offset_plain,
        .lsm_lmm_verify         = lsm_lmm_verify_v3,
        .lsm_unpackmd           = lsm_unpackmd_v3,
};

