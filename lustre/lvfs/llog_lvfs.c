/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2001-2003 Cluster File Systems, Inc.
 *   Author: Andreas Dilger <adilger@clusterfs.com>
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
 * OST<->MDS recovery logging infrastructure.
 *
 * Invariants in implementation:
 * - we do not share logs among different OST<->MDS connections, so that
 *   if an OST or MDS fails it need only look at log(s) relevant to itself
 */

#define DEBUG_SUBSYSTEM S_LOG

#ifndef EXPORT_SYMTAB
#define EXPORT_SYMTAB
#endif

#ifdef __KERNEL__
#include <linux/fs.h>
#else
#include <liblustre.h>
#endif

#include <linux/lvfs.h>
#include <linux/lustre_fsfilt.h>
#include <linux/lustre_log.h>

#ifdef __KERNEL__

static int llog_lvfs_pad(struct llog_ctxt *ctxt, struct l_file *file,
                         int len, int index)
{
        struct llog_rec_hdr rec;
        struct llog_rec_tail tail;
        int rc;
        ENTRY;

        LASSERT(len >= LLOG_MIN_REC_SIZE && (len & 0x7) == 0);

        tail.lrt_len = rec.lrh_len = cpu_to_le32(len);
        tail.lrt_index = rec.lrh_index = cpu_to_le32(index);
        rec.lrh_type = 0;

        rc = llog_fsfilt_write_record(ctxt, file, &rec, sizeof(rec),
                                      &file->f_pos, 0);
        if (rc) {
                CERROR("error writing padding record: rc %d\n", rc);
                goto out;
        }

        file->f_pos += len - sizeof(rec) - sizeof(tail);
        rc = llog_fsfilt_write_record(ctxt, file, &tail, sizeof(tail),
                                      &file->f_pos, 0);
        if (rc) {
                CERROR("error writing padding record: rc %d\n", rc);
                goto out;
        }

 out:
        RETURN(rc);
}

static int llog_lvfs_write_blob(struct llog_ctxt *ctxt, struct l_file *file,
                                struct llog_rec_hdr *rec, void *buf, loff_t off)
{
        int rc;
        struct llog_rec_tail end;
        loff_t saved_off = file->f_pos;
        int buflen = le32_to_cpu(rec->lrh_len);

        ENTRY;
        file->f_pos = off;

        if (!buf) {
                rc = llog_fsfilt_write_record(ctxt, file, rec, buflen,
                                              &file->f_pos, 0);
                if (rc) {
                        CERROR("error writing log record: rc %d\n", rc);
                        goto out;
                }
                GOTO(out, rc = 0);
        }

        /* the buf case */
        rec->lrh_len = cpu_to_le32(sizeof(*rec) + buflen + sizeof(end));
        rc = llog_fsfilt_write_record(ctxt, file, rec, sizeof(*rec),
                                      &file->f_pos, 0);
        if (rc) {
                CERROR("error writing log hdr: rc %d\n", rc);
                goto out;
        }

        rc = llog_fsfilt_write_record(ctxt, file, buf, buflen,
                                      &file->f_pos, 0);
        if (rc) {
                CERROR("error writing log buffer: rc %d\n", rc);
                goto out;
        }

        end.lrt_len = rec->lrh_len;
        end.lrt_index = rec->lrh_index;
        rc = llog_fsfilt_write_record(ctxt, file, &end, sizeof(end),
                                      &file->f_pos, 0);
        if (rc) {
                CERROR("error writing log tail: rc %d\n", rc);
                goto out;
        }

        rc = 0;
 out:
        if (saved_off > file->f_pos)
                file->f_pos = saved_off;
        LASSERT(rc <= 0);
        RETURN(rc);
}

static int llog_lvfs_read_blob(struct llog_ctxt *ctxt, struct l_file *file,
                               void *buf, int size, loff_t off)
{
        loff_t offset = off;
        int rc;
        ENTRY;

        rc = llog_fsfilt_read_record(ctxt, file, buf, size, &offset);
        if (rc) {
                CERROR("error reading log record: rc %d\n", rc);
                RETURN(rc);
        }
        RETURN(0);
}

static int llog_lvfs_read_header(struct llog_handle *handle)
{
        struct llog_ctxt *ctxt = handle->lgh_ctxt;
        int rc;
        ENTRY;

        LASSERT(sizeof(*handle->lgh_hdr) == LLOG_CHUNK_SIZE);
        LASSERT(ctxt != NULL);

        if (handle->lgh_file->f_dentry->d_inode->i_size == 0) {
                CDEBUG(D_HA, "not reading header from 0-byte log\n");
                RETURN(LLOG_EEMPTY);
        }

        rc = llog_lvfs_read_blob(ctxt, handle->lgh_file, handle->lgh_hdr,
                                 LLOG_CHUNK_SIZE, 0);
        if (rc)
                CERROR("error reading log header\n");

        handle->lgh_last_idx = le32_to_cpu(handle->lgh_hdr->llh_tail.lrt_index);
        handle->lgh_file->f_pos = handle->lgh_file->f_dentry->d_inode->i_size;

        RETURN(rc);
}

/* returns negative in on error; 0 if success && reccookie == 0; 1 otherwise */
/* appends if idx == -1, otherwise overwrites record idx. */
static int llog_lvfs_write_rec(struct llog_handle *loghandle,
                               struct llog_rec_hdr *rec,
                               struct llog_cookie *reccookie,
                               int cookiecount,
                               void *buf, int idx)
{
        struct llog_log_hdr *llh;
        int reclen = le32_to_cpu(rec->lrh_len), index, rc;
        struct llog_rec_tail *lrt;
        struct llog_ctxt *ctxt = loghandle->lgh_ctxt;
        struct file *file;
        loff_t offset;
        size_t left;
        ENTRY;

        llh = loghandle->lgh_hdr;
        file = loghandle->lgh_file;

        /* record length should not bigger than LLOG_CHUNK_SIZE */
        if (buf)
                rc = (reclen > LLOG_CHUNK_SIZE - sizeof(struct llog_rec_hdr)
                      - sizeof(struct llog_rec_tail)) ? -E2BIG : 0;
        else
                rc = (reclen > LLOG_CHUNK_SIZE) ? -E2BIG : 0;
        if (rc)
                RETURN(rc);

        if (idx != -1) {
                loff_t saved_offset;

                /* no header: only allowed to insert record 1 */
                if (idx != 1 && !file->f_dentry->d_inode->i_size) {
                        CERROR("idx != -1 in empty log\n");
                        LBUG();
                }

                if (idx && llh->llh_size && llh->llh_size != reclen)
                        RETURN(-EINVAL);

                rc = llog_lvfs_write_blob(ctxt, file, &llh->llh_hdr, NULL, 0);
                /* we are done if we only write the header or on error */
                if (rc || idx == 0)
                        RETURN(rc);

                saved_offset = sizeof(*llh) + (idx-1)*le32_to_cpu(rec->lrh_len);
                rc = llog_lvfs_write_blob(ctxt, file, rec, buf, saved_offset);
                if (rc == 0 && reccookie) {
                        reccookie->lgc_lgl = loghandle->lgh_id;
                        reccookie->lgc_index = idx;
                        rc = 1;
                }
                RETURN(rc);
        }

        /* Make sure that records don't cross a chunk boundary, so we can
         * process them page-at-a-time if needed.  If it will cross a chunk
         * boundary, write in a fake (but referenced) entry to pad the chunk.
         *
         * We know that llog_current_log() will return a loghandle that is
         * big enough to hold reclen, so all we care about is padding here.
         */
        left = LLOG_CHUNK_SIZE - (file->f_pos & (LLOG_CHUNK_SIZE - 1));
        if (buf)
                reclen = sizeof(*rec) + le32_to_cpu(rec->lrh_len) +
                         sizeof(struct llog_rec_tail);

        /* NOTE: padding is a record, but no bit is set */
        if (left != 0 && left != reclen &&
            left < (reclen + LLOG_MIN_REC_SIZE)) {
                loghandle->lgh_last_idx++;
                rc = llog_lvfs_pad(ctxt, file, left, loghandle->lgh_last_idx);
                if (rc)
                        RETURN(rc);
        }

        loghandle->lgh_last_idx++;
        index = loghandle->lgh_last_idx;
        rec->lrh_index = cpu_to_le32(index);
        if (buf == NULL) {
                lrt = (void *)rec + le32_to_cpu(rec->lrh_len) - sizeof(*lrt);
                lrt->lrt_len = rec->lrh_len;
                lrt->lrt_index = rec->lrh_index;
        }
        if (ext2_set_bit(index, llh->llh_bitmap)) {
                CERROR("argh, index %u already set in log bitmap?\n", index);
                LBUG(); /* should never happen */
        }
        llh->llh_count = cpu_to_le32(le32_to_cpu(llh->llh_count) + 1);
        llh->llh_tail.lrt_index = cpu_to_le32(index);

        offset = 0;
        rc = llog_lvfs_write_blob(ctxt, file, &llh->llh_hdr, NULL, 0);
        if (rc)
                RETURN(rc);

        rc = llog_lvfs_write_blob(ctxt, file, rec, buf, file->f_pos);
        if (rc)
                RETURN(rc);

        CDEBUG(D_HA, "added record "LPX64": idx: %u, %u bytes\n",
               loghandle->lgh_id.lgl_oid, index, le32_to_cpu(rec->lrh_len));
        if (rc == 0 && reccookie) {
                reccookie->lgc_lgl = loghandle->lgh_id;
                reccookie->lgc_index = index;
                if (le32_to_cpu(rec->lrh_type) == MDS_UNLINK_REC)
                        reccookie->lgc_subsys = LLOG_UNLINK_ORIG_CTXT;
                else if (le32_to_cpu(rec->lrh_type) == OST_SZ_REC)
                        reccookie->lgc_subsys = LLOG_SIZE_ORIG_CTXT;
                else if (le32_to_cpu(rec->lrh_type) == OST_RAID1_REC)
                        reccookie->lgc_subsys = LLOG_RD1_ORIG_CTXT;
                else
                        reccookie->lgc_subsys = -1;
                rc = 1;
        }
        if (rc == 0 && (le32_to_cpu(rec->lrh_type) == LLOG_GEN_REC ||
            le32_to_cpu(rec->lrh_type) == SMFS_UPDATE_REC))
                rc = 1;

        RETURN(rc);
}

/* We can skip reading at least as many log blocks as the number of
* minimum sized log records we are skipping.  If it turns out
* that we are not far enough along the log (because the
* actual records are larger than minimum size) we just skip
* some more records. */

static void llog_skip_over(__u64 *off, int curr, int goal)
{
        if (goal <= curr)
                return;
        *off = (*off + (goal-curr-1) * LLOG_MIN_REC_SIZE) &
                ~(LLOG_CHUNK_SIZE - 1);
}

/* sets:
 *  - curr_offset to the furthest point read in the log file
 *  - curr_idx to the log index preceeding curr_offset
 * returns -EIO/-EINVAL on error
 */
static int llog_lvfs_next_block(struct llog_handle *loghandle, int *curr_idx,
                                int next_idx, __u64 *curr_offset, void *buf,
                                int len)
{
        struct llog_ctxt *ctxt = loghandle->lgh_ctxt;
        int rc;
        ENTRY;

        if (len == 0 || len & (LLOG_CHUNK_SIZE - 1))
                RETURN(-EINVAL);

        CDEBUG(D_OTHER, "looking for log index %u (cur idx %u off "LPU64")\n",
               next_idx, *curr_idx, *curr_offset);

        while (*curr_offset < loghandle->lgh_file->f_dentry->d_inode->i_size) {
                struct llog_rec_hdr *rec;
                struct llog_rec_tail *tail;
                loff_t ppos;

                llog_skip_over(curr_offset, *curr_idx, next_idx);

                ppos = *curr_offset;
                rc = llog_fsfilt_read_record(ctxt, loghandle->lgh_file,
                                             buf, len, &ppos);

                if (rc) {
                        CERROR("Cant read llog block at log id "LPU64
                               "/%u offset "LPU64"\n",
                               loghandle->lgh_id.lgl_oid,
                               loghandle->lgh_id.lgl_ogen,
                               *curr_offset);
                        RETURN(rc);
                }

                /* put number of bytes read into rc to make code simpler */
                rc = ppos - *curr_offset;
                *curr_offset = ppos;

                if (rc == 0) /* end of file, nothing to do */
                        RETURN(0);

                if (rc < sizeof(*tail)) {
                        CERROR("Invalid llog block at log id "LPU64"/%u offset "
                               LPU64"\n", loghandle->lgh_id.lgl_oid,
                               loghandle->lgh_id.lgl_ogen, *curr_offset);
                        RETURN(-EINVAL);
                }

                tail = buf + rc - sizeof(struct llog_rec_tail);
                *curr_idx = le32_to_cpu(tail->lrt_index);

                /* this shouldn't happen */
                if (tail->lrt_index == 0) {
                        CERROR("Invalid llog tail at log id "LPU64"/%u offset "
                               LPU64"\n", loghandle->lgh_id.lgl_oid,
                               loghandle->lgh_id.lgl_ogen, *curr_offset);
                        RETURN(-EINVAL);
                }
                if (le32_to_cpu(tail->lrt_index) < next_idx)
                        continue;

                /* sanity check that the start of the new buffer is no farther
                 * than the record that we wanted.  This shouldn't happen. */
                rec = buf;
                if (le32_to_cpu(rec->lrh_index) > next_idx) {
                        CERROR("missed desired record? %u > %u\n",
                               le32_to_cpu(rec->lrh_index), next_idx);
                        RETURN(-ENOENT);
                }
                RETURN(0);
        }
        RETURN(-EIO);
}

static int llog_lvfs_prev_block(struct llog_handle *loghandle,
                                int prev_idx, void *buf, int len)
{
        struct llog_ctxt *ctxt = loghandle->lgh_ctxt;
        __u64 curr_offset;
        int rc;
        ENTRY;

        if (len == 0 || len & (LLOG_CHUNK_SIZE - 1))
                RETURN(-EINVAL);

        CDEBUG(D_OTHER, "looking for log index %u n", prev_idx);

        curr_offset = LLOG_CHUNK_SIZE;
        llog_skip_over(&curr_offset, 0, prev_idx);

        while (curr_offset < loghandle->lgh_file->f_dentry->d_inode->i_size) {
                struct llog_rec_hdr *rec;
                struct llog_rec_tail *tail;
                loff_t ppos;

                ppos = curr_offset;
                rc = llog_fsfilt_read_record(ctxt, loghandle->lgh_file,
                                             buf, len, &ppos);

                if (rc) {
                        CERROR("Cant read llog block at log id "LPU64
                               "/%u offset "LPU64"\n",
                               loghandle->lgh_id.lgl_oid,
                               loghandle->lgh_id.lgl_ogen,
                               curr_offset);
                        RETURN(rc);
                }

                /* put number of bytes read into rc to make code simpler */
                rc = ppos - curr_offset;
                curr_offset = ppos;

                if (rc == 0) /* end of file, nothing to do */
                        RETURN(0);

                if (rc < sizeof(*tail)) {
                        CERROR("Invalid llog block at log id "LPU64"/%u offset "
                               LPU64"\n", loghandle->lgh_id.lgl_oid,
                               loghandle->lgh_id.lgl_ogen, curr_offset);
                        RETURN(-EINVAL);
                }

                tail = buf + rc - sizeof(struct llog_rec_tail);

                /* this shouldn't happen */
                if (tail->lrt_index == 0) {
                        CERROR("Invalid llog tail at log id "LPU64"/%u offset "
                               LPU64"\n", loghandle->lgh_id.lgl_oid,
                               loghandle->lgh_id.lgl_ogen, curr_offset);
                        RETURN(-EINVAL);
                }
                if (le32_to_cpu(tail->lrt_index) < prev_idx)
                        continue;

                /* sanity check that the start of the new buffer is no farther
                 * than the record that we wanted.  This shouldn't happen. */
                rec = buf;
                if (le32_to_cpu(rec->lrh_index) > prev_idx) {
                        CERROR("missed desired record? %u > %u\n",
                               le32_to_cpu(rec->lrh_index), prev_idx);
                        RETURN(-ENOENT);
                }
                RETURN(0);
        }
        RETURN(-EIO);
}

static struct file *llog_filp_open(char *name, int flags, int mode)
{
        char *logname;
        struct file *filp;
        int len;

        OBD_ALLOC(logname, PATH_MAX);
        if (logname == NULL)
                return ERR_PTR(-ENOMEM);

        len = snprintf(logname, PATH_MAX, "LOGS/%s", name);
        if (len >= PATH_MAX - 1) {
                filp = ERR_PTR(-ENAMETOOLONG);
        } else {
                filp = l_filp_open(logname, flags, mode);
                if (IS_ERR(filp)) {
                        CERROR("logfile creation %s: %ld\n", logname,
                               PTR_ERR(filp));
                }
        }

        OBD_FREE(logname, PATH_MAX);
        return filp;
}

static struct file *llog_object_create(struct llog_ctxt *ctxt)
{
        unsigned int tmpname = ll_insecure_random_int();
        char fidname[LL_FID_NAMELEN];
        struct file *filp;
        struct dentry *new_child, *parent;
        void *handle;
        int rc = 0, err, namelen;
        ENTRY;

        sprintf(fidname, "OBJECTS/%u", tmpname);
        filp = filp_open(fidname, O_CREAT | O_EXCL, 0644);
        if (IS_ERR(filp)) {
                rc = PTR_ERR(filp);
                if (rc == -EEXIST) {
                        CERROR("impossible object name collision %u\n",
                               tmpname);
                        LBUG();
                }
                CERROR("error creating tmp object %u: rc %d\n", tmpname, rc);
                RETURN(filp);
        }

        namelen = ll_fid2str(fidname, filp->f_dentry->d_inode->i_ino,
                             filp->f_dentry->d_inode->i_generation);
        parent = filp->f_dentry->d_parent;
        down(&parent->d_inode->i_sem);
        new_child = lookup_one_len(fidname, parent, namelen);
        if (IS_ERR(new_child)) {
                CERROR("getting neg dentry for obj rename: %d\n", rc);
                GOTO(out_close, rc = PTR_ERR(new_child));
        }
        if (new_child->d_inode != NULL) {
                CERROR("impossible non-negative obj dentry %lu:%u!\n",
                       filp->f_dentry->d_inode->i_ino,
                       filp->f_dentry->d_inode->i_generation);
                LBUG();
        }

        handle = llog_fsfilt_start(ctxt, parent->d_inode, FSFILT_OP_RENAME, NULL);
        if (IS_ERR(handle))
                GOTO(out_dput, rc = PTR_ERR(handle));

        lock_kernel();
        rc = vfs_rename(parent->d_inode, filp->f_dentry,
                        parent->d_inode, new_child);
        unlock_kernel();
        if (rc)
                CERROR("error renaming new object %lu:%u: rc %d\n",
                       filp->f_dentry->d_inode->i_ino,
                       filp->f_dentry->d_inode->i_generation, rc);

        err = llog_fsfilt_commit(ctxt, parent->d_inode, handle, 0);
        if (!rc)
                rc = err;
out_dput:
        dput(new_child);
out_close:
        up(&parent->d_inode->i_sem);
        if (rc) {
                filp_close(filp, 0);
                filp = (struct file *)rc;
        }

        RETURN(filp);
}

static int llog_add_link_object(struct llog_ctxt *ctxt, struct llog_logid logid,
                                struct dentry *dentry)
{
        struct dentry *new_child;
        char fidname[LL_FID_NAMELEN];
        void *handle;
        int namelen, rc = 0, err;
        ENTRY;

        namelen = ll_fid2str(fidname, logid.lgl_oid, logid.lgl_ogen);
        down(&ctxt->loc_objects_dir->d_inode->i_sem);
        new_child = lookup_one_len(fidname, ctxt->loc_objects_dir, namelen);
        if (IS_ERR(new_child)) {
                CERROR("getting neg dentry for obj rename: %d\n", rc);
                GOTO(out, rc = PTR_ERR(new_child));
        }
        if (new_child->d_inode == dentry->d_inode)
                GOTO(out_dput, rc);
        if (new_child->d_inode != NULL) {
                CERROR("impossible non-negative obj dentry "LPX64":%u!\n",
                       logid.lgl_oid, logid.lgl_ogen);
                LBUG();
        }
        handle = llog_fsfilt_start(ctxt, ctxt->loc_objects_dir->d_inode,
                                   FSFILT_OP_LINK, NULL);
        if (IS_ERR(handle))
                GOTO(out_dput, rc = PTR_ERR(handle));

        lock_kernel();
        rc = vfs_link(dentry, ctxt->loc_objects_dir->d_inode, new_child);
        unlock_kernel();
        if (rc)
                CERROR("error link new object "LPX64":%u: rc %d\n",
                       logid.lgl_oid, logid.lgl_ogen, rc);
        err = llog_fsfilt_commit(ctxt, ctxt->loc_objects_dir->d_inode, handle, 0);
out_dput:
        l_dput(new_child);
out:
        up(&ctxt->loc_objects_dir->d_inode->i_sem);
        RETURN(rc);
}

/* This is a callback from the llog_* functions.
 * Assumes caller has already pushed us into the kernel context. */
static int llog_lvfs_create(struct llog_ctxt *ctxt, struct llog_handle **res,
                            struct llog_logid *logid, char *name)
{
        struct llog_handle *handle;
        struct lvfs_run_ctxt saved;
        int rc = 0;
        int open_flags = O_RDWR | O_CREAT | O_LARGEFILE;
        ENTRY;

        handle = llog_alloc_handle();
        if (handle == NULL)
                RETURN(-ENOMEM);
        *res = handle;

        LASSERT(ctxt);
        if (ctxt->loc_lvfs_ctxt)
                push_ctxt(&saved, ctxt->loc_lvfs_ctxt, NULL);

        if (logid != NULL) {
                char logname[LL_FID_NAMELEN + 10] = "OBJECTS/";
                char fidname[LL_FID_NAMELEN];
                ll_fid2str(fidname, logid->lgl_oid, logid->lgl_ogen);
                strcat(logname, fidname);

                handle->lgh_file = filp_open(logname, O_RDWR | O_LARGEFILE, 0644);
                if (IS_ERR(handle->lgh_file)) {
                        CERROR("cannot open %s file\n", logname);
                        GOTO(cleanup, rc = PTR_ERR(handle->lgh_file));
                }
                if (!S_ISREG(handle->lgh_file->f_dentry->d_inode->i_mode)) {
                        CERROR("%s is not a regular file!: mode = %o\n", logname,
                               handle->lgh_file->f_dentry->d_inode->i_mode);
                        GOTO(cleanup, rc = -ENOENT);
                }
                LASSERT(handle->lgh_file->f_dentry->d_parent == ctxt->loc_objects_dir);
                handle->lgh_id = *logid;
        } else if (name) {
                handle->lgh_file = llog_filp_open(name, open_flags, 0644);
                if (IS_ERR(handle->lgh_file))
                        GOTO(cleanup, rc = PTR_ERR(handle->lgh_file));
                LASSERT(handle->lgh_file->f_dentry->d_parent == ctxt->loc_logs_dir);

                handle->lgh_id.lgl_oid = handle->lgh_file->f_dentry->d_inode->i_ino;
                handle->lgh_id.lgl_ogen = handle->lgh_file->f_dentry->d_inode->i_generation;
                rc = llog_add_link_object(ctxt, handle->lgh_id, handle->lgh_file->f_dentry);
                if (rc)
                        GOTO(cleanup, rc);
        } else {
                handle->lgh_file = llog_object_create(ctxt);
                if (IS_ERR(handle->lgh_file))
                        GOTO(cleanup, rc = PTR_ERR(handle->lgh_file));
                LASSERT(handle->lgh_file->f_dentry->d_parent == ctxt->loc_objects_dir);
                handle->lgh_id.lgl_oid = handle->lgh_file->f_dentry->d_inode->i_ino;
                handle->lgh_id.lgl_ogen = handle->lgh_file->f_dentry->d_inode->i_generation;
        }

        handle->lgh_id.lgl_ogr = 1;
        handle->lgh_ctxt = ctxt;
 finish:
        if (ctxt->loc_lvfs_ctxt)
                pop_ctxt(&saved, ctxt->loc_lvfs_ctxt, NULL);
        RETURN(rc);
cleanup:
        llog_free_handle(handle);
        goto finish;
}

static int llog_lvfs_close(struct llog_handle *handle)
{
        int rc;
        ENTRY;

        rc = filp_close(handle->lgh_file, 0);
        if (rc)
                CERROR("error closing log: rc %d\n", rc);
        RETURN(rc);
}

static int llog_lvfs_destroy(struct llog_handle *loghandle)
{
        struct llog_ctxt *ctxt = loghandle->lgh_ctxt;
        struct lvfs_run_ctxt saved;
        struct dentry *fdentry;
        struct inode *parent_inode;
        char fidname[LL_FID_NAMELEN];
        void *handle;
        int rc = -EINVAL, err, namelen;
        ENTRY;

        if (ctxt->loc_lvfs_ctxt)
                push_ctxt(&saved, ctxt->loc_lvfs_ctxt, NULL);

        fdentry = loghandle->lgh_file->f_dentry;
        parent_inode = fdentry->d_parent->d_inode;

        if (!strcmp(fdentry->d_parent->d_name.name, "LOGS")) {
                LASSERT(parent_inode == ctxt->loc_logs_dir->d_inode);

                namelen = ll_fid2str(fidname, fdentry->d_inode->i_ino,
                                     fdentry->d_inode->i_generation);
                dget(fdentry);
                rc = llog_lvfs_close(loghandle);
                if (rc) {
                        dput(fdentry);
                        GOTO(out, rc);
                }

                handle = llog_fsfilt_start(ctxt, parent_inode,
                                           FSFILT_OP_UNLINK, NULL);
                if (IS_ERR(handle)) {
                        dput(fdentry);
                        GOTO(out, rc = PTR_ERR(handle));
                }

                down(&parent_inode->i_sem);
                rc = vfs_unlink(parent_inode, fdentry);
                up(&parent_inode->i_sem);
                dput(fdentry);

                if (!rc) {
                        down(&ctxt->loc_objects_dir->d_inode->i_sem);
                        fdentry = lookup_one_len(fidname, ctxt->loc_objects_dir,
                                                 namelen);
                        if (fdentry == NULL || fdentry->d_inode == NULL) {
                                CERROR("destroy non_existent object %s\n", fidname);
                                GOTO(out_err, rc = IS_ERR(fdentry) ?
                                     PTR_ERR(fdentry) : -ENOENT);
                        }
                        rc = vfs_unlink(ctxt->loc_objects_dir->d_inode, fdentry);
                        l_dput(fdentry);
out_err:
                        up(&ctxt->loc_objects_dir->d_inode->i_sem);
                }
                err = llog_fsfilt_commit(ctxt, parent_inode, handle, 0);
                if (err && !rc)
                        err = rc;

                GOTO(out, rc);
        }

        if (!strcmp(fdentry->d_parent->d_name.name, "OBJECTS")) {
                LASSERT(parent_inode == ctxt->loc_objects_dir->d_inode);

                dget(fdentry);
                rc = llog_lvfs_close(loghandle);
                if (rc == 0) {
                        down(&parent_inode->i_sem);
                        rc = vfs_unlink(parent_inode, fdentry);
                        up(&parent_inode->i_sem);
                }
                dput(fdentry);
        }
out:
        if (ctxt->loc_lvfs_ctxt)
                pop_ctxt(&saved, ctxt->loc_lvfs_ctxt, NULL);
        RETURN(rc);
}

/* reads the catalog list */
int llog_get_cat_list(struct lvfs_run_ctxt *ctxt,
                      struct fsfilt_operations *fsops, char *name,
                      int count, struct llog_catid *idarray)
{
        struct lvfs_run_ctxt saved;
        struct l_file *file;
        int size = sizeof(*idarray) * count;
        loff_t off = 0;
        int rc;

        LASSERT(count);

        if (ctxt)
                push_ctxt(&saved, ctxt, NULL);
        file = l_filp_open(name, O_RDWR | O_CREAT | O_LARGEFILE, 0700);
        if (!file || IS_ERR(file)) {
                rc = PTR_ERR(file);
                CERROR("OBD filter: cannot open/create %s: rc = %d\n",
                       name, rc);
                GOTO(out, rc);
        }

        if (!S_ISREG(file->f_dentry->d_inode->i_mode)) {
                CERROR("%s is not a regular file!: mode = %o\n", name,
                       file->f_dentry->d_inode->i_mode);
                GOTO(out, rc = -ENOENT);
        }

        rc = fsops->fs_read_record(file, idarray, size, &off);
        if (rc) {
                CDEBUG(D_INODE,"OBD filter: error reading %s: rc %d\n",
                       name, rc);
                GOTO(out, rc);
        }

 out:
        if (file && !IS_ERR(file))
                rc = filp_close(file, 0);
        if (ctxt)
                pop_ctxt(&saved, ctxt, NULL);
        RETURN(rc);
}
EXPORT_SYMBOL(llog_get_cat_list);

/* writes the cat list */
int llog_put_cat_list(struct lvfs_run_ctxt *ctxt,
                      struct fsfilt_operations *fsops, char *name,
                      int count, struct llog_catid *idarray)
{
        struct lvfs_run_ctxt saved;
        struct l_file *file;
        int size = sizeof(*idarray) * count;
        loff_t off = 0;
        int rc;

        LASSERT(count);

        if (ctxt)
                push_ctxt(&saved, ctxt, NULL);
        file = filp_open(name, O_RDWR | O_CREAT | O_LARGEFILE, 0700);
        if (!file || IS_ERR(file)) {
                rc = PTR_ERR(file);
                CERROR("OBD filter: cannot open/create %s: rc = %d\n",
                       name, rc);
                GOTO(out, rc);
        }

        if (!S_ISREG(file->f_dentry->d_inode->i_mode)) {
                CERROR("%s is not a regular file!: mode = %o\n", name,
                       file->f_dentry->d_inode->i_mode);
                GOTO(out, rc = -ENOENT);
        }

        rc = fsops->fs_write_record(file, idarray, size, &off, 1);
        if (rc) {
                CDEBUG(D_INODE,"OBD filter: error reading %s: rc %d\n",
                       name, rc);
                GOTO(out, rc);
        }

 out:
        if (file && !IS_ERR(file))
                rc = filp_close(file, 0);
        if (ctxt)
                pop_ctxt(&saved, ctxt, NULL);
        RETURN(rc);
}
EXPORT_SYMBOL(llog_put_cat_list);

struct llog_operations llog_lvfs_ops = {
        lop_create:      llog_lvfs_create,
        lop_destroy:     llog_lvfs_destroy,
        lop_close:       llog_lvfs_close,
        lop_read_header: llog_lvfs_read_header,
        lop_write_rec:   llog_lvfs_write_rec,
        lop_next_block:  llog_lvfs_next_block,
        lop_prev_block:  llog_lvfs_prev_block,
};
EXPORT_SYMBOL(llog_lvfs_ops);

#else /* !__KERNEL__ */

static int llog_lvfs_read_header(struct llog_handle *handle)
{
        LBUG();
        return 0;
}

static int llog_lvfs_write_rec(struct llog_handle *loghandle,
                               struct llog_rec_hdr *rec,
                               struct llog_cookie *reccookie, int cookiecount,
                               void *buf, int idx)
{
        LBUG();
        return 0;
}

static int llog_lvfs_create(struct llog_ctxt *ctxt, struct llog_handle **res,
                            struct llog_logid *logid, char *name)
{
        LBUG();
        return 0;
}

static int llog_lvfs_close(struct llog_handle *handle)
{
        LBUG();
        return 0;
}

static int llog_lvfs_destroy(struct llog_handle *handle)
{
        LBUG();
        return 0;
}

int llog_get_cat_list(struct lvfs_run_ctxt *ctxt,
                      struct fsfilt_operations *fsops, char *name,
                      int count, struct llog_catid *idarray)
{
        LBUG();
        return 0;
}

int llog_put_cat_list(struct lvfs_run_ctxt *ctxt,
                      struct fsfilt_operations *fsops, char *name,
                      int count, struct llog_catid *idarray)
{
        LBUG();
        return 0;
}

int llog_lvfs_prev_block(struct llog_handle *loghandle,
                         int prev_idx, void *buf, int len)
{
        LBUG();
        return 0;
}

int llog_lvfs_next_block(struct llog_handle *loghandle, int *curr_idx,
                         int next_idx, __u64 *offset, void *buf, int len)
{
        LBUG();
        return 0;
}

struct llog_operations llog_lvfs_ops = {
        lop_create:      llog_lvfs_create,
        lop_destroy:     llog_lvfs_destroy,
        lop_close:       llog_lvfs_close,
        lop_read_header: llog_lvfs_read_header,
        lop_write_rec:   llog_lvfs_write_rec,
        lop_next_block:  llog_lvfs_next_block,
        lop_prev_block:  llog_lvfs_prev_block,
};
#endif
