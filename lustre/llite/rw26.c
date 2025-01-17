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
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011 Whamcloud, Inc.
 *
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/lustre/llite/rw26.c
 *
 * Lustre Lite I/O page cache routines for the 2.5/2.6 kernel version
 */

#ifndef AUTOCONF_INCLUDED
#include <linux/config.h>
#endif
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/smp_lock.h>
#include <linux/unistd.h>
#include <linux/version.h>
#include <asm/system.h>
#include <asm/uaccess.h>

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/stat.h>
#include <asm/uaccess.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/smp_lock.h>

#define DEBUG_SUBSYSTEM S_LLITE

//#include <lustre_mdc.h>
#include <lustre_lite.h>
#include "llite_internal.h"
#include <linux/lustre_compat25.h>

/**
 * Implements Linux VM address_space::invalidatepage() method. This method is
 * called when the page is truncate from a file, either as a result of
 * explicit truncate, or when inode is removed from memory (as a result of
 * final iput(), umount, or memory pressure induced icache shrinking).
 *
 * [0, offset] bytes of the page remain valid (this is for a case of not-page
 * aligned truncate). Lustre leaves partially truncated page in the cache,
 * relying on struct inode::i_size to limit further accesses.
 */
static int cl_invalidatepage(struct page *vmpage, unsigned long offset)
{
        struct inode     *inode;
        struct lu_env    *env;
        struct cl_page   *page;
        struct cl_object *obj;

        int result;
        int refcheck;

        LASSERT(PageLocked(vmpage));
        LASSERT(!PageWriteback(vmpage));

        /*
         * It is safe to not check anything in invalidatepage/releasepage
         * below because they are run with page locked and all our io is
         * happening with locked page too
         */
        result = 0;
        if (offset == 0) {
                env = cl_env_get(&refcheck);
                if (!IS_ERR(env)) {
                        inode = vmpage->mapping->host;
                        obj = ll_i2info(inode)->lli_clob;
                        if (obj != NULL) {
                                page = cl_vmpage_page(vmpage, obj);
                                if (page != NULL) {
                                        lu_ref_add(&page->cp_reference,
                                                   "delete", vmpage);
                                        cl_page_delete(env, page);
                                        result = 1;
                                        lu_ref_del(&page->cp_reference,
                                                   "delete", vmpage);
                                        cl_page_put(env, page);
                                }
                        } else
                                LASSERT(vmpage->private == 0);
                        cl_env_put(env, &refcheck);
                }
        }
        return result;
}

#ifdef HAVE_INVALIDATEPAGE_RETURN_INT
static int ll_invalidatepage(struct page *page, unsigned long offset)
{
        return cl_invalidatepage(page, offset);
}
#else /* !HAVE_INVALIDATEPAGE_RETURN_INT */
static void ll_invalidatepage(struct page *page, unsigned long offset)
{
        cl_invalidatepage(page, offset);
}
#endif

#ifdef HAVE_RELEASEPAGE_WITH_INT
#define RELEASEPAGE_ARG_TYPE int
#else
#define RELEASEPAGE_ARG_TYPE gfp_t
#endif
static int ll_releasepage(struct page *page, RELEASEPAGE_ARG_TYPE gfp_mask)
{
        void *cookie;

        cookie = cl_env_reenter();
        ll_invalidatepage(page, 0);
        cl_env_reexit(cookie);
        return 1;
}

static int ll_set_page_dirty(struct page *vmpage)
{
#if 0
        struct cl_page    *page = vvp_vmpage_page_transient(vmpage);
        struct vvp_object *obj  = cl_inode2vvp(vmpage->mapping->host);
        struct vvp_page   *cpg;

        /*
         * XXX should page method be called here?
         */
        LASSERT(&obj->co_cl == page->cp_obj);
        cpg = cl2vvp_page(cl_page_at(page, &vvp_device_type));
        /*
         * XXX cannot do much here, because page is possibly not locked:
         * sys_munmap()->...
         *     ->unmap_page_range()->zap_pte_range()->set_page_dirty().
         */
        vvp_write_pending(obj, cpg);
#endif
        RETURN(__set_page_dirty_nobuffers(vmpage));
}

#define MAX_DIRECTIO_SIZE 2*1024*1024*1024UL

static inline int ll_get_user_pages(int rw, unsigned long user_addr,
                                    size_t size, struct page ***pages,
                                    int *max_pages)
{
        int result = -ENOMEM;

        /* set an arbitrary limit to prevent arithmetic overflow */
        if (size > MAX_DIRECTIO_SIZE) {
                *pages = NULL;
                return -EFBIG;
        }

        *max_pages = (user_addr + size + CFS_PAGE_SIZE - 1) >> CFS_PAGE_SHIFT;
        *max_pages -= user_addr >> CFS_PAGE_SHIFT;

        OBD_ALLOC_LARGE(*pages, *max_pages * sizeof(**pages));
        if (*pages) {
                down_read(&current->mm->mmap_sem);
                result = get_user_pages(current, current->mm, user_addr,
                                        *max_pages, (rw == READ), 0, *pages,
                                        NULL);
                up_read(&current->mm->mmap_sem);
                if (unlikely(result <= 0))
                        OBD_FREE_LARGE(*pages, *max_pages * sizeof(**pages));
        }

        return result;
}

/*  ll_free_user_pages - tear down page struct array
 *  @pages: array of page struct pointers underlying target buffer */
static void ll_free_user_pages(struct page **pages, int npages, int do_dirty)
{
        int i;

        for (i = 0; i < npages; i++) {
                if (pages[i] == NULL)
                        break;
                if (do_dirty)
                        set_page_dirty_lock(pages[i]);
                page_cache_release(pages[i]);
        }

        OBD_FREE_LARGE(pages, npages * sizeof(*pages));
}

ssize_t ll_direct_rw_pages(const struct lu_env *env, struct cl_io *io,
                           int rw, struct inode *inode,
                           struct ll_dio_pages *pv)
{
        struct cl_page    *clp;
        struct cl_2queue  *queue;
        struct cl_object  *obj = io->ci_obj;
        int i;
        ssize_t rc = 0;
        loff_t file_offset  = pv->ldp_start_offset;
        long size           = pv->ldp_size;
        int page_count      = pv->ldp_nr;
        struct page **pages = pv->ldp_pages;
        long page_size      = cl_page_size(obj);
        bool do_io;
        int  io_pages       = 0;
        ENTRY;

        queue = &io->ci_queue;
        cl_2queue_init(queue);
        for (i = 0; i < page_count; i++) {
                if (pv->ldp_offsets)
                    file_offset = pv->ldp_offsets[i];

                LASSERT(!(file_offset & (page_size - 1)));
                clp = cl_page_find(env, obj, cl_index(obj, file_offset),
                                   pv->ldp_pages[i], CPT_TRANSIENT);
                if (IS_ERR(clp)) {
                        rc = PTR_ERR(clp);
                        break;
                }

                rc = cl_page_own(env, io, clp);
                if (rc) {
                        LASSERT(clp->cp_state == CPS_FREEING);
                        cl_page_put(env, clp);
                        break;
                }

                do_io = true;

                /* check the page type: if the page is a host page, then do
                 * write directly */
                if (clp->cp_type == CPT_CACHEABLE) {
                        cfs_page_t *vmpage = cl_page_vmpage(env, clp);
                        cfs_page_t *src_page;
                        cfs_page_t *dst_page;
                        void       *src;
                        void       *dst;

                        src_page = (rw == WRITE) ? pages[i] : vmpage;
                        dst_page = (rw == WRITE) ? vmpage : pages[i];

                        src = kmap_atomic(src_page, KM_USER0);
                        dst = kmap_atomic(dst_page, KM_USER1);
                        memcpy(dst, src, min(page_size, size));
                        kunmap_atomic(dst, KM_USER1);
                        kunmap_atomic(src, KM_USER0);

                        /* make sure page will be added to the transfer by
                         * cl_io_submit()->...->vvp_page_prep_write(). */
                        if (rw == WRITE)
                                set_page_dirty(vmpage);

                        if (rw == READ) {
                                /* do not issue the page for read, since it
                                 * may reread a ra page which has NOT uptodate
                                 * bit set. */
                                cl_page_disown(env, io, clp);
                                do_io = false;
                        }
                }

                if (likely(do_io)) {
                        cl_2queue_add(queue, clp);

                        /*
                         * Set page clip to tell transfer formation engine
                         * that page has to be sent even if it is beyond KMS.
                         */
                        cl_page_clip(env, clp, 0, min(size, page_size));

                        ++io_pages;
                }

                /* drop the reference count for cl_page_find */
                cl_page_put(env, clp);
                size -= page_size;
                file_offset += page_size;
        }

        if (rc == 0 && io_pages) {
                rc = cl_io_submit_sync(env, io,
                                       rw == READ ? CRT_READ : CRT_WRITE,
                                       queue, CRP_NORMAL, 0);
        }
        if (rc == 0)
                rc = pv->ldp_size;

        cl_2queue_discard(env, io, queue);
        cl_2queue_disown(env, io, queue);
        cl_2queue_fini(env, queue);
        RETURN(rc);
}
EXPORT_SYMBOL(ll_direct_rw_pages);

static ssize_t ll_direct_IO_26_seg(const struct lu_env *env, struct cl_io *io,
                                   int rw, struct inode *inode,
                                   struct address_space *mapping,
                                   size_t size, loff_t file_offset,
                                   struct page **pages, int page_count)
{
    struct ll_dio_pages pvec = { .ldp_pages        = pages,
                                 .ldp_nr           = page_count,
                                 .ldp_size         = size,
                                 .ldp_offsets      = NULL,
                                 .ldp_start_offset = file_offset
                               };

    return ll_direct_rw_pages(env, io, rw, inode, &pvec);
}

/* This is the maximum size of a single O_DIRECT request, based on a 128kB
 * kmalloc limit.  We need to fit all of the brw_page structs, each one
 * representing PAGE_SIZE worth of user data, into a single buffer, and
 * then truncate this to be a full-sized RPC.  This is 22MB for 4kB pages. */
#define MAX_DIO_SIZE ((128 * 1024 / sizeof(struct brw_page) * CFS_PAGE_SIZE) & \
                      ~(PTLRPC_MAX_BRW_SIZE - 1))
static ssize_t ll_direct_IO_26(int rw, struct kiocb *iocb,
                               const struct iovec *iov, loff_t file_offset,
                               unsigned long nr_segs)
{
        struct lu_env *env;
        struct cl_io *io;
        struct file *file = iocb->ki_filp;
        struct inode *inode = file->f_mapping->host;
        struct ccc_object *obj = cl_inode2ccc(inode);
        long count = iov_length(iov, nr_segs);
        long tot_bytes = 0, result = 0;
        struct ll_inode_info *lli = ll_i2info(inode);
        struct lov_stripe_md *lsm = lli->lli_smd;
        unsigned long seg = 0;
        long size = MAX_DIO_SIZE;
        int refcheck;
        ENTRY;

        if (!lli->lli_smd || !lli->lli_smd->lsm_object_id)
                RETURN(-EBADF);

        /* FIXME: io smaller than PAGE_SIZE is broken on ia64 ??? */
        if ((file_offset & ~CFS_PAGE_MASK) || (count & ~CFS_PAGE_MASK))
                RETURN(-EINVAL);

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p), size=%lu (max %lu), "
               "offset=%lld=%llx, pages %lu (max %lu)\n",
               inode->i_ino, inode->i_generation, inode, count, MAX_DIO_SIZE,
               file_offset, file_offset, count >> CFS_PAGE_SHIFT,
               MAX_DIO_SIZE >> CFS_PAGE_SHIFT);

        /* Check that all user buffers are aligned as well */
        for (seg = 0; seg < nr_segs; seg++) {
                if (((unsigned long)iov[seg].iov_base & ~CFS_PAGE_MASK) ||
                    (iov[seg].iov_len & ~CFS_PAGE_MASK))
                        RETURN(-EINVAL);
        }

        env = cl_env_get(&refcheck);
        LASSERT(!IS_ERR(env));
        io = ccc_env_io(env)->cui_cl.cis_io;
        LASSERT(io != NULL);

        /* 0. Need locking between buffered and direct access. and race with
         *size changing by concurrent truncates and writes.
         * 1. Need inode sem to operate transient pages. */
        if (rw == READ)
                LOCK_INODE_MUTEX(inode);

        LASSERT(obj->cob_transient_pages == 0);
        for (seg = 0; seg < nr_segs; seg++) {
                long iov_left = iov[seg].iov_len;
                unsigned long user_addr = (unsigned long)iov[seg].iov_base;

                if (rw == READ) {
                        if (file_offset >= inode->i_size)
                                break;
                        if (file_offset + iov_left > inode->i_size)
                                iov_left = inode->i_size - file_offset;
                }

                while (iov_left > 0) {
                        struct page **pages;
                        int page_count, max_pages = 0;
                        long bytes;

                        bytes = min(size,iov_left);
                        page_count = ll_get_user_pages(rw, user_addr, bytes,
                                                       &pages, &max_pages);
                        if (likely(page_count > 0)) {
                                if (unlikely(page_count <  max_pages))
                                        bytes = page_count << CFS_PAGE_SHIFT;
                                result = ll_direct_IO_26_seg(env, io, rw, inode,
                                                             file->f_mapping,
                                                             bytes,
                                                             file_offset, pages,
                                                             page_count);
                                ll_free_user_pages(pages, max_pages, rw==READ);
                        } else if (page_count == 0) {
                                GOTO(out, result = -EFAULT);
                        } else {
                                result = page_count;
                        }
                        if (unlikely(result <= 0)) {
                                /* If we can't allocate a large enough buffer
                                 * for the request, shrink it to a smaller
                                 * PAGE_SIZE multiple and try again.
                                 * We should always be able to kmalloc for a
                                 * page worth of page pointers = 4MB on i386. */
                                if (result == -ENOMEM &&
                                    size > (CFS_PAGE_SIZE / sizeof(*pages)) *
                                           CFS_PAGE_SIZE) {
                                        size = ((((size / 2) - 1) |
                                                 ~CFS_PAGE_MASK) + 1) &
                                                CFS_PAGE_MASK;
                                        CDEBUG(D_VFSTRACE,"DIO size now %lu\n",
                                               size);
                                        continue;
                                }

                                GOTO(out, result);
                        }

                        tot_bytes += result;
                        file_offset += result;
                        iov_left -= result;
                        user_addr += result;
                }
        }
out:
        LASSERT(obj->cob_transient_pages == 0);
        if (rw == READ)
                UNLOCK_INODE_MUTEX(inode);

        if (tot_bytes > 0) {
                if (rw == WRITE) {
                        lov_stripe_lock(lsm);
                        obd_adjust_kms(ll_i2dtexp(inode), lsm, file_offset, 0);
                        lov_stripe_unlock(lsm);
                }
        }

        cl_env_put(env, &refcheck);
        RETURN(tot_bytes ? : result);
}

#if defined(HAVE_KERNEL_WRITE_BEGIN_END) || defined(MS_HAS_NEW_AOPS)
static int ll_write_begin(struct file *file, struct address_space *mapping,
                         loff_t pos, unsigned len, unsigned flags,
                         struct page **pagep, void **fsdata)
{
        pgoff_t index = pos >> PAGE_CACHE_SHIFT;
        struct page *page;
        int rc;
        unsigned from = pos & (PAGE_CACHE_SIZE - 1);
        ENTRY;

        page = grab_cache_page_write_begin(mapping, index, flags);
        if (!page)
                RETURN(-ENOMEM);

        *pagep = page;

        rc = ll_prepare_write(file, page, from, from + len);
        if (rc) {
                unlock_page(page);
                page_cache_release(page);
        }
        RETURN(rc);
}

static int ll_write_end(struct file *file, struct address_space *mapping,
                        loff_t pos, unsigned len, unsigned copied,
                        struct page *page, void *fsdata)
{
        unsigned from = pos & (PAGE_CACHE_SIZE - 1);
        int rc;
        rc = ll_commit_write(file, page, from, from + copied);

        unlock_page(page);
        page_cache_release(page);
        return rc?rc:copied;
}
#endif

#ifdef CONFIG_MIGRATION
int ll_migratepage(struct address_space *mapping,
                   struct page *newpage, struct page *page)
{
        /* Always fail page migration until we have a proper implementation */
        return -EIO;
}
#endif

#ifndef MS_HAS_NEW_AOPS
struct address_space_operations ll_aops = {
        .readpage       = ll_readpage,
//        .readpages      = ll_readpages,
        .direct_IO      = ll_direct_IO_26,
        .writepage      = ll_writepage,
        .writepages     = generic_writepages,
        .set_page_dirty = ll_set_page_dirty,
        .sync_page      = NULL,
#ifdef HAVE_KERNEL_WRITE_BEGIN_END
        .write_begin    = ll_write_begin,
        .write_end      = ll_write_end,
#else
        .prepare_write  = ll_prepare_write,
        .commit_write   = ll_commit_write,
#endif
        .invalidatepage = ll_invalidatepage,
        .releasepage    = (void *)ll_releasepage,
#ifdef CONFIG_MIGRATION
        .migratepage    = ll_migratepage,
#endif
        .bmap           = NULL
};
#else
struct address_space_operations_ext ll_aops = {
        .orig_aops.readpage       = ll_readpage,
//        .orig_aops.readpages      = ll_readpages,
        .orig_aops.direct_IO      = ll_direct_IO_26,
        .orig_aops.writepage      = ll_writepage,
        .orig_aops.writepages     = generic_writepages,
        .orig_aops.set_page_dirty = ll_set_page_dirty,
        .orig_aops.sync_page      = NULL,
        .orig_aops.prepare_write  = ll_prepare_write,
        .orig_aops.commit_write   = ll_commit_write,
        .orig_aops.invalidatepage = ll_invalidatepage,
        .orig_aops.releasepage    = ll_releasepage,
#ifdef CONFIG_MIGRATION
        .orig_aops.migratepage    = ll_migratepage,
#endif
        .orig_aops.bmap           = NULL,
        .write_begin    = ll_write_begin,
        .write_end      = ll_write_end
};
#endif
