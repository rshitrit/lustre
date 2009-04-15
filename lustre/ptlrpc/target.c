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
 * Copyright  2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * Lustre Common Target
 * These are common function for MDT and OST recovery-related functionality
 *
 *   Author: Mikhail Pershin <tappro@sun.com>
 */

#include <obd.h>
#include <lustre_fsfilt.h>
/**
 * Update client data in last_rcvd file. An obd API
 */
static int obt_client_data_update(struct obd_export *exp)
{
        struct lu_export_data *led = &exp->exp_target_data;
        struct obd_device_target *obt = &exp->exp_obd->u.obt;
        loff_t off = led->led_lr_off;
        int rc = 0;

        rc = fsfilt_write_record(exp->exp_obd, obt->obt_rcvd_filp,
                                 led->led_lcd, sizeof(*led->led_lcd), &off, 0);

        CDEBUG(D_INFO, "update client idx %u last_epoch %#x (%#x)\n",
               led->led_lr_idx, le32_to_cpu(led->led_lcd->lcd_last_epoch),
               le32_to_cpu(obt->obt_lsd->lsd_start_epoch));

        return rc;
}

/**
 * Update server data in last_rcvd file. An obd API
 */
int obt_server_data_update(struct obd_device *obd, int force_sync)
{
        struct obd_device_target *obt = &obd->u.obt;
        loff_t off = 0;
        int rc;
        ENTRY;

        CDEBUG(D_SUPER,
               "%s: mount_count is "LPU64", last_transno is "LPU64"\n",
               obt->obt_lsd->lsd_uuid,
               le64_to_cpu(obt->obt_lsd->lsd_mount_count),
               le64_to_cpu(obt->obt_lsd->lsd_last_transno));

        rc = fsfilt_write_record(obd, obt->obt_rcvd_filp, obt->obt_lsd,
                                 sizeof(*obt->obt_lsd), &off, force_sync);
        if (rc)
                CERROR("error writing lr_server_data: rc = %d\n", rc);

        RETURN(rc);
}

/**
 * Update client epoch with server's one
 */
void obt_client_epoch_update(struct obd_export *exp)
{
        struct lsd_client_data *lcd = exp->exp_target_data.led_lcd;
        struct obd_device_target *obt = &exp->exp_obd->u.obt;

        /** VBR: set client last_epoch to current epoch */
        if (le32_to_cpu(lcd->lcd_last_epoch) >=
            le32_to_cpu(obt->obt_lsd->lsd_start_epoch))
                return;
        lcd->lcd_last_epoch = obt->obt_lsd->lsd_start_epoch;
        obt_client_data_update(exp);
}

/**
 * Increment server epoch. An obd API
 */
static void obt_boot_epoch_update(struct obd_device *obd)
{
        __u32 start_epoch;
        struct obd_device_target *obt = &obd->u.obt;
        struct ptlrpc_request *req;
        struct list_head client_list;

        spin_lock(&obt->obt_translock);
        start_epoch = lr_epoch(le64_to_cpu(obt->obt_last_transno)) + 1;
        obt->obt_last_transno = cpu_to_le64((__u64)start_epoch <<
                                            LR_EPOCH_BITS);
        obt->obt_lsd->lsd_start_epoch = cpu_to_le32(start_epoch);
        spin_unlock(&obt->obt_translock);

        CFS_INIT_LIST_HEAD(&client_list);
        spin_lock_bh(&obd->obd_processing_task_lock);
        list_splice_init(&obd->obd_final_req_queue, &client_list);
        spin_unlock_bh(&obd->obd_processing_task_lock);

        /**
         * go through list of exports participated in recovery and
         * set new epoch for them
         */
        list_for_each_entry(req, &client_list, rq_list) {
                LASSERT(!req->rq_export->exp_delayed);
                obt_client_epoch_update(req->rq_export);
        }
        /** return list back at once */
        spin_lock_bh(&obd->obd_processing_task_lock);
        list_splice_init(&client_list, &obd->obd_final_req_queue);
        spin_unlock_bh(&obd->obd_processing_task_lock);
        obt_server_data_update(obd, 1);
}

/**
 * write data in last_rcvd file.
 */
static int lut_last_rcvd_write(const struct lu_env *env, struct lu_target *lut,
                               const struct lu_buf *buf, loff_t *off, int sync)
{
        struct thandle *th;
        struct txn_param p;
        int rc, credits;
        ENTRY;

        credits = lut->lut_bottom->dd_ops->dt_credit_get(env, lut->lut_bottom,
                                                         DTO_WRITE_BLOCK);
        txn_param_init(&p, credits);

        th = dt_trans_start(env, lut->lut_bottom, &p);
        if (IS_ERR(th))
                RETURN(PTR_ERR(th));

        rc = dt_record_write(env, lut->lut_last_rcvd, buf, off, th);
        dt_trans_stop(env, lut->lut_bottom, th);

        CDEBUG(D_INFO, "write last_rcvd header rc = %d:\n"
               "uuid = %s\nlast_transno = "LPU64"\n",
               rc, lut->lut_lsd.lsd_uuid, lut->lut_lsd.lsd_last_transno);

        RETURN(rc);
}

/**
 * Update client data in last_rcvd
 */
int lut_client_data_update(const struct lu_env *env, struct lu_target *lut,
                            struct obd_export *exp)
{
        struct lu_export_data *led = &exp->exp_target_data;
        struct lsd_client_data tmp_lcd;
        loff_t tmp_off = led->led_lr_off;
        struct lu_buf tmp_buf = {
                                        .lb_buf = &tmp_lcd,
                                        .lb_len = sizeof(tmp_lcd)
                                };
        int rc = 0;

        lcd_cpu_to_le(led->led_lcd, &tmp_lcd);
        LASSERT(lut->lut_last_rcvd);
        rc = lut_last_rcvd_write(env, lut, &tmp_buf, &tmp_off, 0);

        return rc;
}

/**
 * Update server data in last_rcvd
 */
static int lut_server_data_update(const struct lu_env *env,
                                  struct lu_target *lut, int sync)
{
        struct lr_server_data tmp_lsd;
        loff_t tmp_off = 0;
        struct lu_buf tmp_buf = {
                                        .lb_buf = &tmp_lsd,
                                        .lb_len = sizeof(tmp_lsd)
                                };
        int rc = 0;
        ENTRY;

        CDEBUG(D_SUPER,
               "%s: mount_count is "LPU64", last_transno is "LPU64"\n",
               lut->lut_lsd.lsd_uuid, lut->lut_mount_count,
               lut->lut_last_transno);

        spin_lock(&lut->lut_translock);
        lut->lut_lsd.lsd_last_transno = lut->lut_last_transno;
        spin_unlock(&lut->lut_translock);

        lsd_cpu_to_le(&lut->lut_lsd, &tmp_lsd);
        if (lut->lut_last_rcvd != NULL)
                rc = lut_last_rcvd_write(env, lut, &tmp_buf, &tmp_off, sync);
        RETURN(rc);
}

void lut_client_epoch_update(const struct lu_env *env, struct lu_target *lut,
                             struct obd_export *exp)
{
        struct lsd_client_data *lcd = exp->exp_target_data.led_lcd;

        LASSERT(lut->lut_bottom);
        /** VBR: set client last_epoch to current epoch */
        if (lcd->lcd_last_epoch >= lut->lut_lsd.lsd_start_epoch)
                return;
        lcd->lcd_last_epoch = lut->lut_lsd.lsd_start_epoch;
        lut_client_data_update(env, lut, exp);
}

/**
 * Update boot epoch when recovery ends
 */
void lut_boot_epoch_update(struct lu_target *lut)
{
        struct lu_env env;
        struct ptlrpc_request *req;
        __u32 start_epoch;
        struct list_head client_list;
        int rc;

        if (lut->lut_obd->obd_stopping)
                return;
        /** Increase server epoch after recovery */
        if (lut->lut_bottom == NULL)
                return obt_boot_epoch_update(lut->lut_obd);

        rc = lu_env_init(&env, LCT_DT_THREAD);
        if (rc) {
                CERROR("Can't initialize environment rc=%i\n", rc);
                return;
        }

        spin_lock(&lut->lut_translock);
        start_epoch = lr_epoch(lut->lut_last_transno) + 1;
        lut->lut_last_transno = (__u64)start_epoch << LR_EPOCH_BITS;
        lut->lut_lsd.lsd_start_epoch = start_epoch;
        spin_unlock(&lut->lut_translock);

        CFS_INIT_LIST_HEAD(&client_list);
        /**
         * The recovery is not yet finished and final queue can still be updated
         * with resend requests. Move final list to separate one for processing
         */
        spin_lock_bh(&lut->lut_obd->obd_processing_task_lock);
        list_splice_init(&lut->lut_obd->obd_final_req_queue, &client_list);
        spin_unlock_bh(&lut->lut_obd->obd_processing_task_lock);

        /**
         * go through list of exports participated in recovery and
         * set new epoch for them
         */
        list_for_each_entry(req, &client_list, rq_list) {
                LASSERT(!req->rq_export->exp_delayed);
                if (!req->rq_export->exp_vbr_failed)
                        lut_client_epoch_update(&env, lut, req->rq_export);
        }
        /** return list back at once */
        spin_lock_bh(&lut->lut_obd->obd_processing_task_lock);
        list_splice_init(&client_list, &lut->lut_obd->obd_final_req_queue);
        spin_unlock_bh(&lut->lut_obd->obd_processing_task_lock);
        /** update server epoch */
        lut_server_data_update(&env, lut, 1);
        lu_env_fini(&env);
}
EXPORT_SYMBOL(lut_boot_epoch_update);

/**
 * commit callback, need to update last_commited value
 */
void lut_cb_last_committed(struct lu_target *lut, __u64 transno,
                           void *data, int err)
{
        struct obd_export *exp = data;

        spin_lock(&lut->lut_translock);
        if (transno > lut->lut_obd->obd_last_committed)
                lut->lut_obd->obd_last_committed = transno;

        LASSERT(exp);
        if (!lut->lut_obd->obd_stopping &&
            transno > exp->exp_last_committed) {
                exp->exp_last_committed = transno;
                spin_unlock(&lut->lut_translock);
                ptlrpc_commit_replies(exp);
        } else {
                spin_unlock(&lut->lut_translock);
        }
        if (transno)
                CDEBUG(D_HA, "%s: transno "LPD64" is committed\n",
                       lut->lut_obd->obd_name, transno);
}
EXPORT_SYMBOL(lut_cb_last_committed);

void lut_cb_client(struct lu_target *lut, __u64 transno,
                       void *data, int err)
{
        LASSERT(lut->lut_obd);
        target_client_add_cb(lut->lut_obd, transno, data, err);
}
EXPORT_SYMBOL(lut_cb_client);

int lut_init(const struct lu_env *env, struct lu_target *lut,
             struct obd_device *obd, struct dt_device *dt)
{
        struct lu_fid fid;
        struct dt_object *o;
        int rc = 0;
        ENTRY;

        lut->lut_obd = obd;
        lut->lut_bottom = dt;
        lut->lut_last_rcvd = NULL;

        spin_lock_init(&lut->lut_translock);
        spin_lock_init(&lut->lut_client_bitmap_lock);
        spin_lock_init(&lut->lut_trans_table_lock);

        /** obdfilter has no lu_device stack yet */
        if (dt == NULL)
                RETURN(rc);
        o = dt_store_open(env, lut->lut_bottom, "", LAST_RCVD, &fid);
        if (!IS_ERR(o)) {
                lut->lut_last_rcvd = o;
        } else {
                rc = PTR_ERR(o);
                CERROR("cannot open %s: rc = %d\n", LAST_RCVD, rc);
        }

        RETURN(rc);
}
EXPORT_SYMBOL(lut_init);

void lut_fini(const struct lu_env *env, struct lu_target *lut)
{
        ENTRY;
        if (lut->lut_last_rcvd)
                lu_object_put(env, &lut->lut_last_rcvd->do_lu);
        lut->lut_last_rcvd = NULL;
        EXIT;
}
EXPORT_SYMBOL(lut_fini);