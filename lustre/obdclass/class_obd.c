/*
 *              An implementation of a loadable kernel mode driver providing
 *              multiple kernel/user space bidirectional communications links.
 *
 *              Author:         Alan Cox <alan@cymru.net>
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 * 
 *              Adapted to become the Linux 2.0 Coda pseudo device
 *              Peter  Braam  <braam@maths.ox.ac.uk> 
 *              Michael Callahan <mjc@emmy.smith.edu>           
 *
 *              Changes for Linux 2.1
 *              Copyright (c) 1997 Carnegie-Mellon University
 *
 *              Redone again for Intermezzo
 *              Copyright (c) 1998 Peter J. Braam
 *
 *              Hacked up again for simulated OBD
 *              Copyright (c) 1999 Stelias Computing, Inc.
 *                (authors {pschwan,braam}@stelias.com)
 *              Copyright (C) 1999 Seagate Technology, Inc.
 *              Copyright (C) 2001 Cluster File Systems, Inc.
 *
 * 
 */

#define EXPORT_SYMTAB

#include <linux/config.h> /* for CONFIG_PROC_FS */
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/kmod.h>   /* for request_module() */
#include <linux/sched.h>
#include <linux/lp.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/fcntl.h>
#include <linux/delay.h>
#include <linux/skbuff.h>
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/list.h>
#include <asm/io.h>
#include <asm/segment.h>
#include <asm/system.h>
#include <asm/poll.h>
#include <asm/uaccess.h>

#include <linux/obd_support.h>
#include <linux/obd_class.h>

static int obd_init_magic;
int obd_print_entry = 1;
int obd_debug_level = ~0;
long obd_memory = 0;
struct obd_device obd_dev[MAX_OBD_DEVICES];
struct list_head obd_types;

/* called when opening /dev/obdNNN */
static int obd_class_open(struct inode * inode, struct file * file)
{
        int dev;
        ENTRY;

        if (!inode)
                return -EINVAL;
        dev = MINOR(inode->i_rdev);
        if (dev >= MAX_OBD_DEVICES)
                return -ENODEV;
        obd_dev[dev].obd_refcnt++;
        CDEBUG(D_PSDEV, "Dev %d refcount now %d\n", dev,
               obd_dev[dev].obd_refcnt);

	obd_dev[dev].obd_proc_entry = 
		proc_lustre_register_obd_device(&obd_dev[dev]);

        MOD_INC_USE_COUNT;
        EXIT;
        return 0;
}

/* called when closing /dev/obdNNN */
static int obd_class_release(struct inode * inode, struct file * file)
{
        int dev;
        ENTRY;

        if (!inode)
                return -EINVAL;
        dev = MINOR(inode->i_rdev);
        if (dev >= MAX_OBD_DEVICES)
                return -ENODEV;
        fsync_dev(inode->i_rdev);
        if (obd_dev[dev].obd_refcnt <= 0)
                printk(KERN_ALERT __FUNCTION__ ": refcount(%d) <= 0\n",
                       obd_dev[dev].obd_refcnt);
        obd_dev[dev].obd_refcnt--;

	if (obd_dev[dev].obd_proc_entry && (obd_dev[dev].obd_refcnt==0))
		proc_lustre_release_obd_device(&obd_dev[dev]);

        CDEBUG(D_PSDEV, "Dev %d refcount now %d\n", dev,
               obd_dev[dev].obd_refcnt);
        MOD_DEC_USE_COUNT;

        EXIT;
        return 0;
}

/* 
 * support functions: we could use inter-module communication, but this 
 * is more portable to other OS's
 */
static struct obd_type *obd_search_type(char *nm)
{
        struct list_head *tmp;
        struct obd_type *type;
        CDEBUG(D_INFO, "SEARCH %s\n", nm);
        
        tmp = &obd_types;
        while ( (tmp = tmp->next) != &obd_types ) {
                type = list_entry(tmp, struct obd_type, typ_chain);
                CDEBUG(D_INFO, "TYP %s\n", type->typ_name);
                if (strlen(type->typ_name) == strlen(nm) &&
                    strcmp(type->typ_name, nm) == 0 ) {
                        return type;
                }
        }
	return NULL;
}

static struct obd_type *obd_nm_to_type(char *nm) 
{
        struct obd_type *type = obd_search_type(nm);

#ifdef CONFIG_KMOD
	if ( !type ) {
		if ( !request_module(nm) ) {
			CDEBUG(D_PSDEV, "Loaded module '%s'\n", nm);
			type = obd_search_type(nm);
		} else {
			CDEBUG(D_PSDEV, "Can't load module '%s'\n", nm);
		}
	}
#endif
        return type;
}


static int getdata(int len, void **data)
{
        void *tmp = NULL;

        if (!len) {
                *data = NULL;
                return 0;
        }

        CDEBUG(D_MALLOC, "len %d, add %p\n", len, *data);

        OBD_ALLOC(tmp, void *, len);
        if ( !tmp )
                return -ENOMEM;
        
        memset(tmp, 0, len);
        if ( copy_from_user(tmp, *data, len)) {
                OBD_FREE(tmp, len);
                return -EFAULT;
        }
        *data = tmp;

        return 0;
}


static int obd_devicename_from_path(obd_devicename* whoami, 
				    uint32_t klen,
				    char* kname, char *user_path)
{
	int err;
	struct nameidata nd;
	whoami->len = klen;
	whoami->name = kname;

#if (LINUX_VERSION_CODE <=  0x20403)

 	err = user_path_walk(user_path, &nd);
 	if (!err) {
		CDEBUG(D_INFO, "found dentry for %s\n", kname);
 		whoami->dentry = nd.dentry;
 		path_release(&nd);
 	}
 	return err;
#endif 
	return 0;
}



/* to control /dev/obdNNN */
static int obd_class_ioctl (struct inode * inode, struct file * filp, 
                            unsigned int cmd, unsigned long arg)
{
        struct obd_device *obddev;
        /* NOTE this must be larger than any of the ioctl data structs */
        char buf[1024];
        void *tmp_buf = buf;
        struct obd_conn conn;
        int err, dev;
        long int cli_id; /* connect, disconnect */

        if (!inode) {
                CDEBUG(D_IOCTL, "invalid inode\n");
                return -EINVAL;
        }

        dev = MINOR(inode->i_rdev);
        if (dev > MAX_OBD_DEVICES)
                return -ENODEV;
        obddev = &obd_dev[dev];
        conn.oc_dev = obddev;

        switch (cmd) {
        case TCGETS:
                return -EINVAL;
        case OBD_IOC_ATTACH: {
                struct obd_type *type;
                struct oic_generic *input = tmp_buf;
                char *nm;

                ENTRY;
                /* have we attached a type to this device */
                if ( obddev->obd_type || 
                     (obddev->obd_flags & OBD_ATTACHED) ){
                        CDEBUG(D_IOCTL,
                               "OBD Device %d already attached to type %s.\n",
                               dev, obddev->obd_type->typ_name);
                        EXIT;
                        return -EBUSY;
                }

                /* get data structures */
                err = copy_from_user(input, (void *)arg, sizeof(*input));
                if ( err ) {
                        EXIT;
                        return err;
                }

                err = getdata(input->att_typelen + 1, &input->att_type);
                if ( err ) {
                        EXIT;
                        return err;
                }

                /* find the type */
                nm = input->att_type;
                type = obd_nm_to_type(nm);

                OBD_FREE(input->att_type, input->att_typelen + 1);
                if ( !type ) {
                        printk(__FUNCTION__ ": unknown obd type dev %d\n",
                               dev);
                        EXIT;
                        return -EINVAL;
                }
                obddev->obd_type = type;
                
                /* get the attach data */
                err = getdata(input->att_datalen, &input->att_data);
                if ( err ) {
                        EXIT;
                        return err;
                }

                INIT_LIST_HEAD(&obddev->obd_gen_clients);
                obddev->obd_multi_count = 0;

                CDEBUG(D_IOCTL, "Attach %d, datalen %d, type %s\n", 
                       dev, input->att_datalen, obddev->obd_type->typ_name);
                /* maybe we are done */
                if ( !OBT(obddev) || !OBP(obddev, attach) ) {
                        obddev->obd_flags |= OBD_ATTACHED;
                        type->typ_refcnt++;
                        CDEBUG(D_PSDEV, "Dev %d refcount now %d\n", dev,
                               type->typ_refcnt);
                        if (input->att_data)
                                OBD_FREE(input->att_data, input->att_datalen);
                        MOD_INC_USE_COUNT;
                        EXIT;
                        return 0;
                }

                /* do the attach */
                err = OBP(obddev, attach)(obddev, input->att_datalen,
                                          input->att_data);
                if (input->att_data)
                        OBD_FREE(input->att_data, input->att_datalen);

                if ( err ) {
                        obddev->obd_flags &= ~OBD_ATTACHED;
                        obddev->obd_type = NULL;
                        EXIT;
                } else {
                        obddev->obd_flags |=  OBD_ATTACHED;
                        type->typ_refcnt++;
                        CDEBUG(D_PSDEV, "Dev %d refcount now %d\n", dev,
                               type->typ_refcnt);
                        MOD_INC_USE_COUNT;
                        EXIT;
                }
                return err;
        }

        case OBD_IOC_DETACH: {

                ENTRY;
                if (obddev->obd_flags & OBD_SET_UP) {
                        EXIT;
                        return -EBUSY;
                }
                if (! (obddev->obd_flags & OBD_ATTACHED) ) {
                        CDEBUG(D_IOCTL, "Device not attached\n");
                        EXIT;
                        return -ENODEV;
                }
                if ( !list_empty(&obddev->obd_gen_clients) ) {
                        CDEBUG(D_IOCTL, "Device has connected clients\n");
                        EXIT;
                        return -EBUSY;
                }

                CDEBUG(D_PSDEV, "Detach %d, type %s\n", dev,
                       obddev->obd_type->typ_name);
                obddev->obd_flags &= ~OBD_ATTACHED;
                obddev->obd_type->typ_refcnt--;
                CDEBUG(D_PSDEV, "Dev %d refcount now %d\n", dev,
                       obddev->obd_type->typ_refcnt);
                obddev->obd_type = NULL;
                MOD_DEC_USE_COUNT;
                EXIT;
                return 0;
        }

        case OBD_IOC_SETUP: {
                struct ioc_setup {
                        int setup_datalen;
                        void *setup_data;
                } *setup;
		char *user_path;

                setup = tmp_buf;


                ENTRY;
                /* have we attached a type to this device */
                if (!(obddev->obd_flags & OBD_ATTACHED)) {
                        CDEBUG(D_IOCTL, "Device not attached\n");
                        EXIT;
                        return -ENODEV;
                }

                /* has this been done already? */
                if ( obddev->obd_flags & OBD_SET_UP ) {
                        CDEBUG(D_IOCTL, "Device %d already setup (type %s)\n",
                               dev, obddev->obd_type->typ_name);
                        EXIT;
                        return -EBUSY;
                }



		/* get main structure */
		err = copy_from_user(setup, (void *) arg, sizeof(*setup));
		if (err) {
		  EXIT;
		  return err;
		}
		user_path = setup->setup_data;

                /* get the attach data */
                err = getdata(setup->setup_datalen, &setup->setup_data);
                if ( err ) {
                        EXIT;
                        return err;
                }

		err = obd_devicename_from_path(&(obddev->obd_fsname),
					       setup->setup_datalen,
					       (char*) setup->setup_data, 
					       user_path);
		if (err) {
		  memset(&(obddev->obd_fsname), 0, sizeof(obd_devicename));
		  EXIT;
		  return err;
		}
		
                /* do the setup */
                CDEBUG(D_PSDEV, "Setup %d, type %s\n", dev, 
                       obddev->obd_type->typ_name);
                if ( !OBT(obddev) || !OBP(obddev, setup) ) {
                        obddev->obd_type->typ_refcnt++;
                        CDEBUG(D_PSDEV, "Dev %d refcount now %d\n",
                               dev, obddev->obd_type->typ_refcnt);
                        if (setup->setup_data)
                                OBD_FREE(setup->setup_data,
                                         setup->setup_datalen);
                        obddev->obd_flags |= OBD_SET_UP;
                        EXIT;
                        return 0;
                }

                err = OBP(obddev, setup)(obddev, 0, NULL);
                          
                if ( err )  {
                        obddev->obd_flags &= ~OBD_SET_UP;
                        EXIT;
                } else {
                        obddev->obd_type->typ_refcnt++;
                        CDEBUG(D_PSDEV, "Dev %d refcount now %d\n",
                               dev, obddev->obd_type->typ_refcnt);
                        obddev->obd_flags |= OBD_SET_UP;
                        EXIT;
                }
		OBD_FREE(setup->setup_data, setup->setup_datalen);
                return err;
        }
        case OBD_IOC_CLEANUP: {
                ENTRY;
                /* has this minor been registered? */
                if (!obddev->obd_type) {
                        CDEBUG(D_IOCTL, "OBD Device %d has no type.\n", dev);
                        EXIT;
                        return -ENODEV;
                }

                if ( !obddev->obd_type->typ_refcnt ) {
                        CDEBUG(D_IOCTL, "dev %d has refcount (%d)!\n",
                               dev, obddev->obd_type->typ_refcnt);
                        EXIT;
                        return -EBUSY;
                }

                if ( (!(obddev->obd_flags & OBD_SET_UP)) ||
                     (!(obddev->obd_flags & OBD_ATTACHED))) {
                        CDEBUG(D_IOCTL, "device not attached or set up\n");
                        EXIT;
                        return -ENODEV;
                }

                if ( !OBT(obddev) || !OBP(obddev, cleanup) )
                        goto cleanup_out;

                /* cleanup has no argument */
                err = OBP(obddev, cleanup)(obddev);
                if ( err ) {
                        EXIT;
                        return err;
                }

        cleanup_out: 
                obddev->obd_flags &= ~OBD_SET_UP;
                obddev->obd_type->typ_refcnt--;
                CDEBUG(D_PSDEV, "Dev %d refcount now %d\n", dev,
                       obddev->obd_type->typ_refcnt);
                EXIT;
                return 0;
        }
        case OBD_IOC_CONNECT:
        {
                if ( (!(obddev->obd_flags & OBD_SET_UP)) ||
                     (!(obddev->obd_flags & OBD_ATTACHED))) {
                        CDEBUG(D_IOCTL, "Device not attached or set up\n");
                        return -ENODEV;
                }

                if ( !OBT(obddev) || !OBP(obddev, connect) )
                        return -EOPNOTSUPP;
                
                err = OBP(obddev, connect)(&conn);
                if ( err )
                        return err;

                return copy_to_user((int *)arg, &conn.oc_id,
                                    sizeof(uint32_t));
        }
        case OBD_IOC_DISCONNECT:
                /* frees data structures */
                /* has this minor been registered? */
                if (!obddev->obd_type)
                        return -ENODEV;

                get_user(cli_id, (int *) arg);
                conn.oc_id = cli_id;

                if ( !OBT(obddev) || !OBP(obddev, disconnect))
                        return -EOPNOTSUPP;
                
                OBP(obddev, disconnect)(&conn);
                return 0;

        case OBD_IOC_SYNC: {
                struct oic_range_s *range = tmp_buf;

                if (!obddev->obd_type)
                        return -ENODEV;

                err = copy_from_user(range, (const void *)arg,  sizeof(*range));

                if ( err ) {
                        EXIT;
                        return err;
                }
                        
                if ( !OBT(obddev) || !OBP(obddev, sync) ) {
                        err = -EOPNOTSUPP;
                        EXIT;
                        return err;
                }

                /* XXX sync needs to be tested/verified */
                err = OBP(obddev, sync)(&conn, &range->obdo, range->count,
                                        range->offset);

                if ( err ) {
                        EXIT;
                        return err;
                }
                        
                return put_user(err, (int *) arg);
        }
        case OBD_IOC_CREATE: {
                struct oic_attr_s *attr = tmp_buf;

                err = copy_from_user(attr, (const void *)arg,  sizeof(*attr));
                if (err) {
                        EXIT;
                        return err;
                }

                /* has this minor been registered? */
                if ( !(obddev->obd_flags & OBD_ATTACHED) ||
                     !(obddev->obd_flags & OBD_SET_UP)) {
                        CDEBUG(D_IOCTL, "Device not attached or set up\n");
                        return -ENODEV;
                }
                conn.oc_id = attr->conn_id;

                if ( !OBT(obddev) || !OBP(obddev, create) )
                        return -EOPNOTSUPP;

                err = OBP(obddev, create)(&conn, &attr->obdo);
                if (err) {
                        EXIT;
                        return err;
                }

                err = copy_to_user((int *)arg, attr, sizeof(*attr));
                EXIT;
                return err;
        }

        case OBD_IOC_DESTROY: {
                struct oic_attr_s *attr = tmp_buf;
                
                /* has this minor been registered? */
                if (!obddev->obd_type)
                        return -ENODEV;

                err = copy_from_user(attr, (int *)arg, sizeof(*attr));
                if ( err ) {
                        EXIT;
                        return err;
                }

                if ( !OBT(obddev) || !OBP(obddev, destroy) )
                        return -EOPNOTSUPP;

                conn.oc_id = attr->conn_id;
                err = OBP(obddev, destroy)(&conn, &attr->obdo);
                EXIT;
                return err;
        }

        case OBD_IOC_SETATTR: {
                struct oic_attr_s *attr = tmp_buf;

                /* has this minor been registered? */
                if (!obddev->obd_type)
                        return -ENODEV;

                err = copy_from_user(attr, (int *)arg, sizeof(*attr));
                if (err)
                        return err;

                if ( !OBT(obddev) || !OBP(obddev, setattr) )
                        return -EOPNOTSUPP;
                
                conn.oc_id = attr->conn_id;
                err = OBP(obddev, setattr)(&conn, &attr->obdo);
                EXIT;
                return err;
        }

        case OBD_IOC_GETATTR: {
                struct oic_attr_s *attr = tmp_buf;

                err = copy_from_user(attr, (int *)arg, sizeof(*attr));
                if (err)
                        return err;

                conn.oc_id = attr->conn_id;
                ODEBUG(&attr->obdo);
                err = OBP(obddev, getattr)(&conn, &attr->obdo);
                if ( err ) {
                        EXIT;
                        return err;
                }

                err = copy_to_user((int *)arg, attr, sizeof(*attr));
                EXIT;
                return err;
        }

        case OBD_IOC_READ: {
                int err;
                struct oic_rw_s *rw_s = tmp_buf;  /* read, write ioctl str */

                err = copy_from_user(rw_s, (int *)arg, sizeof(*rw_s));
                if ( err ) {
                        EXIT;
                        return err;
                }

                conn.oc_id = rw_s->conn_id;

                if ( !OBT(obddev) || !OBP(obddev, read) ) {
                        err = -EOPNOTSUPP;
                        EXIT;
                        return err;
                }


                err = OBP(obddev, read)(&conn, &rw_s->obdo, rw_s->buf, 
                                        &rw_s->count, rw_s->offset);
                
                ODEBUG(&rw_s->obdo);
                CDEBUG(D_INFO, "READ: conn %d, count %Ld, offset %Ld, '%s'\n",
                       rw_s->conn_id, rw_s->count, rw_s->offset, rw_s->buf);
                if ( err ) {
                        EXIT;
                        return err;
                }
                        
                err = copy_to_user((int*)arg, &rw_s->count, sizeof(rw_s->count));
                EXIT;
                return err;
        }

        case OBD_IOC_WRITE: {
                struct oic_rw_s *rw_s = tmp_buf;  /* read, write ioctl str */

                err = copy_from_user(rw_s, (int *)arg, sizeof(*rw_s));
                if ( err ) {
                        EXIT;
                        return err;
                }

                conn.oc_id = rw_s->conn_id;

                if ( !OBT(obddev) || !OBP(obddev, write) ) {
                        err = -EOPNOTSUPP;
                        return err;
                }

                CDEBUG(D_INFO, "WRITE: conn %d, count %Ld, offset %Ld, '%s'\n",
                       rw_s->conn_id, rw_s->count, rw_s->offset, rw_s->buf);

                err = OBP(obddev, write)(&conn, &rw_s->obdo, rw_s->buf, 
                                         &rw_s->count, rw_s->offset);
                ODEBUG(&rw_s->obdo);
                if ( err ) {
                        EXIT;
                        return err;
                }

                err = copy_to_user((int *)arg, &rw_s->count,
                                   sizeof(rw_s->count));
                EXIT;
                return err;
        }
        case OBD_IOC_PREALLOCATE: {
                struct oic_prealloc_s *prealloc = tmp_buf;

                /* has this minor been registered? */
                if (!obddev->obd_type)
                        return -ENODEV;

                err = copy_from_user(prealloc, (int *)arg, sizeof(*prealloc));
                if (err) 
                        return -EFAULT;

                if ( !(obddev->obd_flags & OBD_ATTACHED) ||
                     !(obddev->obd_flags & OBD_SET_UP)) {
                        CDEBUG(D_IOCTL, "Device not attached or set up\n");
                        return -ENODEV;
                }

                if ( !OBT(obddev) || !OBP(obddev, preallocate) )
                        return -EOPNOTSUPP;

                conn.oc_id = prealloc->conn_id;
                err = OBP(obddev, preallocate)(&conn, &prealloc->alloc,
                                               prealloc->ids);
                if ( err ) {
                        EXIT;
                        return err;
                }

                err =copy_to_user((int *)arg, prealloc, sizeof(*prealloc));
                EXIT;
                return err;
        }
        case OBD_IOC_STATFS: {
                struct statfs *tmp;
                unsigned int conn_id;
                struct statfs buf;

                /* has this minor been registered? */
                if (!obddev->obd_type)
                        return -ENODEV;

                tmp = (void *)arg + sizeof(unsigned int);
                get_user(conn_id, (int *) arg);

                if ( !OBT(obddev) || !OBP(obddev, statfs) )
                        return -EOPNOTSUPP;

                conn.oc_id = conn_id;
                err = OBP(obddev, statfs)(&conn, &buf);
                if ( err ) {
                        EXIT;
                        return err;
                }
                err = copy_to_user(tmp, &buf, sizeof(buf));
                EXIT;
                return err;
                
        }
        case OBD_IOC_COPY: {
                struct ioc_mv_s *mvdata = tmp_buf;

                if ( (!(obddev->obd_flags & OBD_SET_UP)) ||
                     (!(obddev->obd_flags & OBD_ATTACHED))) {
                        CDEBUG(D_IOCTL, "Device not attached or set up\n");
                        return -ENODEV;
                }

                /* get main structure */
                err = copy_from_user(mvdata, (void *) arg, sizeof(*mvdata));
                if (err) {
                        EXIT;
                        return err;
                }

                if ( !OBT(obddev) || !OBP(obddev, copy) )
                        return -EOPNOTSUPP;

                /* do the partition */
                CDEBUG(D_INFO, "Copy %d, type %s dst %Ld src %Ld\n", dev, 
                       obddev->obd_type->typ_name, mvdata->dst.o_id, 
                       mvdata->src.o_id);

                conn.oc_id = mvdata->src_conn_id;

                err = OBP(obddev, copy)(&conn, &mvdata->dst, 
                                        &conn, &mvdata->src, 
                                        mvdata->src.o_size, 0);
                return err;
        }

        case OBD_IOC_MIGR: {
                struct ioc_mv_s *mvdata = tmp_buf;

                if ( (!(obddev->obd_flags & OBD_SET_UP)) ||
                     (!(obddev->obd_flags & OBD_ATTACHED))) {
                        CDEBUG(D_IOCTL, "Device not attached or set up\n");
                        return -ENODEV;
                }

                err = copy_from_user(mvdata, (void *) arg, sizeof(*mvdata));
                if (err) {
                        EXIT;
                        return err;
                }

                CDEBUG(D_INFO, "Migrate copying %d bytes\n", sizeof(*mvdata));

                if ( !OBT(obddev) || !OBP(obddev, migrate) )
                        return -EOPNOTSUPP;

                /* do the partition */
                CDEBUG(D_INFO, "Migrate %d, type %s conn %d src %Ld dst %Ld\n",
                       dev, obddev->obd_type->typ_name, mvdata->src_conn_id,
                       mvdata->src.o_id, mvdata->dst.o_id);

                conn.oc_id = mvdata->src_conn_id;
                err = OBP(obddev, migrate)(&conn, &mvdata->dst, &mvdata->src, 
                                           mvdata->src.o_size, 0);

                return err;
        }
        case OBD_IOC_PUNCH: {
                struct oic_rw_s *rw_s = tmp_buf;  /* read, write ioctl str */

                err = copy_from_user(rw_s, (int *)arg, sizeof(*rw_s));
                if ( err ) {
                        EXIT;
                        return err;
                }

                conn.oc_id = rw_s->conn_id;

                if ( !OBT(obddev) || !OBP(obddev, punch) ) {
                        err = -EOPNOTSUPP;
                        return err;
                }

                CDEBUG(D_INFO, "PUNCH: conn %d, count %Ld, offset %Ld\n",
                       rw_s->conn_id, rw_s->count, rw_s->offset);
                err = OBP(obddev, punch)(&conn, &rw_s->obdo, rw_s->count,
                                         rw_s->offset);
                ODEBUG(&rw_s->obdo);
                if ( err ) {
                        EXIT;
                        return err;
                }
                EXIT;
                return err;
        }

        default: {
                struct obd_type *type;
                struct oic_generic input;
                char *nm;
                void *karg;

                /* get data structures */
                err = copy_from_user(&input, (void *)arg, sizeof(input));
                if ( err ) {
                        EXIT;
                        return err;
                }

                err = getdata(input.att_typelen + 1, &input.att_type);
                if ( err ) {
                        EXIT;
                        return err;
                }

                /* find the type */
                nm = input.att_type;
                type = obd_nm_to_type(nm);
#ifdef CONFIG_KMOD
                if ( !type ) {
                        if ( !request_module(nm) ) {
                                CDEBUG(D_PSDEV, "Loaded module '%s'\n", nm);
                                type = obd_nm_to_type(nm);
                        } else {
                                CDEBUG(D_PSDEV, "Can't load module '%s'\n", nm);
                        }
                }
#endif
                OBD_FREE(input.att_type, input.att_typelen + 1);
                if ( !type ) {
                        printk(__FUNCTION__ ": unknown obd type dev %d\n", dev);
                        EXIT;
                        return -EINVAL;
                }
                
                if ( !type->typ_ops || !type->typ_ops->o_iocontrol ) {
                        EXIT;
                        return -EOPNOTSUPP;
                }
                conn.oc_id = input.att_connid;
                
                CDEBUG(D_INFO, "Calling ioctl %x for type %s, len %d\n",
                       cmd, type->typ_name, input.att_datalen);

                /* get the generic data */
                karg = input.att_data;
                err = getdata(input.att_datalen, &karg);
                if ( err ) {
                        EXIT;
                        return err;
                }

                err = type->typ_ops->o_iocontrol(cmd, &conn, input.att_datalen, 
                                                 karg, input.att_data);
                OBD_FREE(karg, input.att_datalen);

                EXIT;
                return err;
        }
        }
} /* obd_class_ioctl */

/* Driver interface done, utility functions follow */

int obd_register_type(struct obd_ops *ops, char *nm)
{
        struct obd_type *type;


        if (obd_init_magic != 0x11223344) {
                printk(__FUNCTION__ ": bad magic for type\n");
                EXIT;
                return -EINVAL;
        }

        if  ( obd_nm_to_type(nm) ) {
                CDEBUG(D_IOCTL, "Type %s already registered\n", nm);
                EXIT;
                return -EEXIST;
        }
        
        OBD_ALLOC(type, struct obd_type * , sizeof(*type));
        if ( !type ) {
                EXIT;
                return -ENOMEM;
        }
        memset(type, 0, sizeof(*type));
        INIT_LIST_HEAD(&type->typ_chain);
        MOD_INC_USE_COUNT;
        list_add(&type->typ_chain, obd_types.next);
        type->typ_ops = ops;
        type->typ_name = nm;
        EXIT;
        return 0;
}
        
int obd_unregister_type(char *nm)
{
        struct obd_type *type = obd_nm_to_type(nm);

        if ( !type ) {
                MOD_DEC_USE_COUNT;
                printk(KERN_INFO __FUNCTION__ ": unknown obd type\n");
                EXIT;
                return -EINVAL;
        }

        if ( type->typ_refcnt ) {
                MOD_DEC_USE_COUNT;
                printk(KERN_ALERT __FUNCTION__ ":type %s has refcount "
                       "(%d)\n", nm, type->typ_refcnt);
                EXIT;
                return -EBUSY;
        }

        list_del(&type->typ_chain);
        OBD_FREE(type, sizeof(*type));
        MOD_DEC_USE_COUNT;
        return 0;
} /* obd_unregister_type */

/* declare character device */
static struct file_operations obd_psdev_fops = {
        ioctl: obd_class_ioctl,       /* ioctl */
        open: obd_class_open,        /* open */
        release: obd_class_release,     /* release */
};


/* modules setup */

int init_obd(void)
{
        int err;
        int i;

        printk(KERN_INFO "OBD class driver  v0.01, braam@stelias.com\n");
        
        INIT_LIST_HEAD(&obd_types);
        
        if (register_chrdev(OBD_PSDEV_MAJOR,"obd_psdev", 
                            &obd_psdev_fops)) {
                printk(KERN_ERR __FUNCTION__ ": unable to get major %d\n", 
                       OBD_PSDEV_MAJOR);
                return -EIO;
        }

        for (i = 0; i < MAX_OBD_DEVICES; i++) {
                memset(&(obd_dev[i]), 0, sizeof(obd_dev[i]));
                obd_dev[i].obd_minor = i;
                INIT_LIST_HEAD(&obd_dev[i].obd_gen_clients);
        }

        err = obd_init_obdo_cache();
        if (err)
                return err;
        obd_sysctl_init();
        obd_init_magic = 0x11223344;
        return 0;
}

EXPORT_SYMBOL(obd_register_type);
EXPORT_SYMBOL(obd_unregister_type);

EXPORT_SYMBOL(obd_print_entry);
EXPORT_SYMBOL(obd_debug_level);
EXPORT_SYMBOL(obd_dev);

EXPORT_SYMBOL(gen_connect);
EXPORT_SYMBOL(gen_client);
EXPORT_SYMBOL(gen_cleanup);
EXPORT_SYMBOL(gen_disconnect);
EXPORT_SYMBOL(gen_copy_data); 
EXPORT_SYMBOL(obdo_cachep);

/* EXPORT_SYMBOL(gen_multi_attach); */
EXPORT_SYMBOL(gen_multi_setup);
EXPORT_SYMBOL(gen_multi_cleanup);


#ifdef MODULE
int init_module(void)
{
        return init_obd();
}

void cleanup_module(void)
{
        int i;
        ENTRY;

        unregister_chrdev(OBD_PSDEV_MAJOR, "obd_psdev");
        for (i = 0; i < MAX_OBD_DEVICES; i++) {
                struct obd_device *obddev = &obd_dev[i];
                if ( obddev->obd_type && 
                     (obddev->obd_flags & OBD_SET_UP) &&
                     OBT(obddev) && OBP(obddev, detach) ) {
                        /* XXX should this call generic detach otherwise? */
                        OBP(obddev, detach)(obddev);
                } 
        }

        obd_cleanup_obdo_cache();
        obd_sysctl_clean();
        CDEBUG(D_MALLOC, "CLASS mem used %ld\n", obd_memory);
        obd_init_magic = 0;
        EXIT;
}
#endif
