#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <asm/uaccess.h>
#include <drm/drmP.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <linux/namei.h>
#include <asm/compat.h>
#include <asm/string.h>
#include <linux/mm.h>
#include <asm/syscalls.h>
#include <linux/syscalls.h>
#include <asm/unistd.h>
#include <linux/random.h>
#include <asm/delay.h>

#define PTR_TYPE uint32_t

MODULE_LICENSE("GPL");

ssize_t drmtest_write(struct file *filp, const char __user *user_buf, size_t count, loff_t *f_pos);

struct file_operations drmtest_fops = {
	write: drmtest_write
};

const int drmtest_major = 199;


//***************************************************

#define DRM_DEV_PATH "/dev/dri/card0"
#define ANIM_LENGTH 300

struct file *filp;
mm_segment_t old_fs;

void *fb_base[10];
long fb_w[10];
long fb_h[10];

int connectors_count = 0;

static int exit_drm(void) {
	int err;
	// stop being master
	err = drm_ioctl(filp, DRM_IOCTL_DROP_MASTER, 0);
	if(err) {
		printk(KERN_ERR "drmtest: error in drm_ioctl, DRM_IOCTL_DROP_MASTER (%d)", err);
		return err;
	}

	// default memory model
	set_fs(old_fs);

	// close device
	filp_close(filp, (void *)0);

	return 0;
}

static int init_drm(void) {
	int err;
	int i;

	struct drm_file *file_priv;

	struct drm_mode_card_res res = {0};

	PTR_TYPE res_fb_buf[10] = {0};
	PTR_TYPE res_crtc_buf[10] = {0};
	PTR_TYPE res_conn_buf[10] = {0};
	PTR_TYPE res_enc_buf[10] = {0};
	
	struct drm_mode_modeinfo conn_mode_buf[20];
	PTR_TYPE conn_prop_buf[20];
	PTR_TYPE conn_propval_buf[20];
	PTR_TYPE conn_enc_buf[20];

	struct drm_mode_get_connector conn;

	struct drm_mode_create_dumb create_dumb;
	struct drm_mode_map_dumb map_dumb;
	struct drm_mode_fb_cmd cmd_dumb;

	struct drm_mode_get_encoder enc;
	struct drm_mode_crtc crtc;


	// make kernel and user memory in common space
	old_fs = get_fs();
	set_fs(KERNEL_DS);

	// open device file
	filp = filp_open(DRM_DEV_PATH, O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(filp)) {
		err = PTR_ERR(filp);
		printk(KERN_ERR "drmtest: unable to open file: %s (%d)", DRM_DEV_PATH, err);
		return err;
	}
 
 	// set master
  	err = drm_ioctl(filp, DRM_IOCTL_SET_MASTER, 0);
	if(err) {
		printk(KERN_ERR "drmtest: error in drm_ioctl, DRM_IOCTL_SET_MASTER (%d)", err);
		goto master_release;
	}

	// check if master permission is set
	file_priv = filp->private_data;
	err = drm_ioctl_permit(DRM_MASTER, file_priv);
	if(err) {
		printk(KERN_ERR "drmtest: cannot set MASTER permissions (%d)", err);
		goto master_release;
	}

	// get resources count
	err = drm_ioctl(filp, DRM_IOCTL_MODE_GETRESOURCES, (long unsigned int)&res);
	if(err) {
		printk(KERN_ERR "drmtest: error in drm_ioctl, DRM_IOCTL_MODE_GETRESOURCES (%d)", err);
		goto master_release;
	}

	// set pointers
	res.fb_id_ptr=(PTR_TYPE)res_fb_buf;
	res.crtc_id_ptr=(PTR_TYPE)res_crtc_buf;
	res.connector_id_ptr=(PTR_TYPE)res_conn_buf;
	res.encoder_id_ptr=(PTR_TYPE)res_enc_buf;

	// get resources data
	err = drm_ioctl(filp, DRM_IOCTL_MODE_GETRESOURCES, (long unsigned int)&res);
	if(err) {
		printk(KERN_ERR "drmtest: error in drm_ioctl, DRM_IOCTL_MODE_GETRESOURCES (%d)", err);
		goto master_release;
	}

	// print resources info
	printk("fb: %d, crtc: %d, conn: %d, enc: %d\n",res.count_fbs,res.count_crtcs,res.count_connectors,res.count_encoders);

	//Loop though all available connectors
	for (i=0; i<res.count_connectors; i++)
	{
		// clear
		memset(conn_mode_buf, 0, sizeof(struct drm_mode_modeinfo)*20);
		memset(conn_prop_buf, 0, sizeof(PTR_TYPE)*20);
		memset(conn_propval_buf, 0, sizeof(PTR_TYPE)*20);
		memset(conn_enc_buf, 0, sizeof(PTR_TYPE)*20);
		memset(&conn, 0, sizeof(struct drm_mode_get_connector));

		conn.connector_id = res_conn_buf[i];

		//get connector resource counts
		err = drm_ioctl(filp, DRM_IOCTL_MODE_GETCONNECTOR, (long unsigned int)&conn);	
		if(err) {
			printk(KERN_ERR "drmtest: error in drm_ioctl, DRM_IOCTL_MODE_GETCONNECTOR (%d)", err);
			goto master_release;
		}

		// set pointers
		conn.modes_ptr=(PTR_TYPE)conn_mode_buf;
		conn.props_ptr=(PTR_TYPE)conn_prop_buf;
		conn.prop_values_ptr=(PTR_TYPE)conn_propval_buf;
		conn.encoders_ptr=(PTR_TYPE)conn_enc_buf;

		// get connector resources
		err = drm_ioctl(filp, DRM_IOCTL_MODE_GETCONNECTOR, (long unsigned int)&conn);
		if(err) {
			printk(KERN_ERR "drmtest: error in drm_ioctl, DRM_IOCTL_MODE_GETCONNECTOR (%d)", err);
			goto master_release;
		}

		// check if the connector is connected
		if (conn.count_encoders<1 || conn.count_modes<1 || !conn.encoder_id || !conn.connection) {
			printk("Not connected\n");
			continue;
		}


		// *****************************
		//      create dumb buffer
		// *****************************

		memset(&create_dumb, 0, sizeof(struct drm_mode_create_dumb));
		memset(&map_dumb, 0, sizeof(struct drm_mode_map_dumb));
		memset(&cmd_dumb, 0, sizeof(struct drm_mode_fb_cmd));

		// set screen params
		create_dumb.width = conn_mode_buf[0].hdisplay;
		create_dumb.height = conn_mode_buf[0].vdisplay;
		create_dumb.bpp = 32;
		create_dumb.flags = 0;
		create_dumb.pitch = 0;
		create_dumb.size = 0;
		create_dumb.handle = 0;

		// create dumb buffer
		err = drm_ioctl(filp, DRM_IOCTL_MODE_CREATE_DUMB, (long unsigned int)&create_dumb);
		if(err) {
			printk(KERN_ERR "drmtest: error in drm_ioctl, DRM_IOCTL_MODE_CREATE_DUMB (%d)", err);
			goto master_release;
		}

		cmd_dumb.width=create_dumb.width;
		cmd_dumb.height=create_dumb.height;
		cmd_dumb.bpp=create_dumb.bpp;
		cmd_dumb.pitch=create_dumb.pitch;
		cmd_dumb.depth=24;
		cmd_dumb.handle=create_dumb.handle;

		// add framebuffer
		err = drm_ioctl(filp, DRM_IOCTL_MODE_ADDFB, (long unsigned int)&cmd_dumb);
		if(err) {
			printk(KERN_ERR "drmtest: error in drm_ioctl, DRM_IOCTL_MODE_ADDFB (%d)", err);
			goto master_release;
		}

		// prepare dumb buffer to mmap
		map_dumb.handle=create_dumb.handle;
		err = drm_ioctl(filp, DRM_IOCTL_MODE_MAP_DUMB, (long unsigned int)&map_dumb);
		if(err) {
			printk(KERN_ERR "drmtest: error in drm_ioctl, DRM_IOCTL_MODE_MAP_DUMB (%d)", err);
			goto master_release;
		}

		
		// map buffer to memory
		fb_base[i] = (void *)vm_mmap(filp, 0, create_dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, map_dumb.offset);

		fb_w[i]=create_dumb.width;
		fb_h[i]=create_dumb.height;


		// *************************
		// kernel mode setting
		// *************************


		printk("%d : mode: %d, prop: %d, enc: %d\n",conn.connection,conn.count_modes,conn.count_props,conn.count_encoders);
		printk("modes: %dx%d FB: %d\n", conn_mode_buf[0].hdisplay, conn_mode_buf[0].vdisplay, (int)fb_base[i]);


		// init encoder
		memset(&enc, 0, sizeof(struct drm_mode_get_encoder));

		enc.encoder_id=conn.encoder_id;

		// get encoder
		err = drm_ioctl(filp, DRM_IOCTL_MODE_GETENCODER, (long unsigned int)&enc);	
		if(err) {
			printk(KERN_ERR "drmtest: error in drm_ioctl, DRM_IOCTL_MODE_GETENCODER (%d)", err);
			goto master_release;
		}

		// init crtc
		memset(&crtc, 0, sizeof(struct drm_mode_crtc));

		crtc.crtc_id=enc.crtc_id;

		err = drm_ioctl(filp, DRM_IOCTL_MODE_GETCRTC, (long unsigned int)&crtc);
		if(err) {
			printk(KERN_ERR "drmtest: error in drm_ioctl, DRM_IOCTL_MODE_GETCRTC (%d)", err);
			goto master_release;
		}

		crtc.fb_id=cmd_dumb.fb_id;
		crtc.set_connectors_ptr=(PTR_TYPE)&res_conn_buf[i];
		crtc.count_connectors=1;
		crtc.mode=conn_mode_buf[0];
		crtc.mode_valid=1;

		err = drm_ioctl(filp, DRM_IOCTL_MODE_SETCRTC, (long unsigned int)&crtc);
		if(err) {
			printk(KERN_ERR "drmtest: error in drm_ioctl, DRM_IOCTL_MODE_SETCRTC (%d)", err);
			goto master_release;
		}
	}

	connectors_count = res.count_connectors;


	return 0;

master_release:
	exit_drm();	
	return err;
}

void put_pixel(int x, int y, int color, int conn) {
	PTR_TYPE location;
	location = y*(fb_w[conn]+10) + x;
	*(((PTR_TYPE*)fb_base[conn])+location)=color;
}

//***************************************************

static int __init drmtest_init(void) {
	int result;

	result = register_chrdev(drmtest_major, "drmtest", &drmtest_fops);
	if (result < 0) {
		printk(KERN_ERR "drmtest: cannot register the /dev/drmtest device with major number: %d\n", drmtest_major);
		return result;
	}

	printk(KERN_INFO "drmtest: module has been inserted.\n");
	return 0;
}

static void __exit drmtest_exit(void) {
	unregister_chrdev(drmtest_major, "drmtest");

	printk(KERN_INFO "drmtest: module has been removed\n");
}

static void display_animation(void) {
	int color, i, j, dx, px = 500, py = 500, k, len, x=0;

	for(k = 0; k < ANIM_LENGTH; k++) {
		color = get_random_int();
		for(j=0; j<connectors_count; j++) {
			if(fb_h[j] > 0 && fb_w[j] > 0) {

				if(get_random_int() % 2) {
					dx = 1;
				} else {
					dx = -1;
				}

				len = get_random_int() % 100;

				if(x) { // horizontal
					for(i=0; i<len; i++) {
						px = (px+dx) % fb_w[j];
						if(px < 0) px += fb_w[j];
						put_pixel(px, py, color, j);
						udelay(500);
						x = 0;
					}
				} else { // vertical
					for(i=0; i<len; i++) {
						py = (py+dx) % fb_h[j];
						if(py < 0) py += fb_h[j];
						put_pixel(px, py, color, j);
						udelay(500);
						x = 1;
					}
				}
			}
		}	
	}
}

ssize_t drmtest_write(struct file *filp, const char __user *user_buf, size_t count, loff_t *f_pos) {
	
	init_drm();
	display_animation();
	exit_drm();

	return count;
}

module_init(drmtest_init);
module_exit(drmtest_exit);
 
