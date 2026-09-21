// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author:Mark Yao <mark.yao@rock-chips.com>
 */

#include <linux/moduleparam.h>

#include <drm/drm.h>
#include <drm/drm_client.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_probe_helper.h>

#include "rockchip_drm_drv.h"
#include "rockchip_drm_gem.h"
#include "rockchip_drm_fb.h"
#include "rockchip_drm_fbdev.h"

#define PREFERRED_BPP		32

static unsigned int fbdev_tv_margin = 80;
module_param(fbdev_tv_margin, uint, 0444);
MODULE_PARM_DESC(fbdev_tv_margin,
		 "percent of its mode a TV-only console occupies (100 disables)");

/*
 * The mode the console is actually being looked at in.
 *
 * drm_fb_helper sizes fbcon to the *smallest* mode across the probed modesets,
 * so that a cloned console is wholly visible on every output, while the buffer
 * behind it is sized to the largest. A composite encoder has no detect line and
 * always reads connected, so on a board that has one the console is laid out for
 * 720x480 on every boot, HDMI or not: a small window in the corner of a
 * 1920x1080 framebuffer, which fbcon then pans over because yres_virtual is more
 * than twice yres - and that pan walks the CRTC past the end of the buffer.
 *
 * Size it to what is really on screen instead. @width and @height are updated to
 * the largest non-TV mode when anything else is driving the console, and to the
 * TV's own mode when it is alone; they are left alone if no modeset has a mode.
 *
 * Returns true when a TV connector is the only thing displaying the console.
 */
static bool rockchip_fbdev_console_mode(struct drm_fb_helper *helper,
					unsigned int *width,
					unsigned int *height)
{
	unsigned int tv_w = 0, tv_h = 0, other_w = 0, other_h = 0;
	struct drm_mode_set *modeset;
	unsigned int i;

	mutex_lock(&helper->client.modeset_mutex);
	drm_client_for_each_modeset(modeset, &helper->client) {
		unsigned int w, h;
		bool tv;

		if (!modeset->mode || !modeset->num_connectors)
			continue;

		w = modeset->mode->hdisplay + modeset->x;
		h = modeset->mode->vdisplay + modeset->y;

		/* a mixed clone is not a TV-only console */
		tv = true;
		for (i = 0; i < modeset->num_connectors; i++)
			if (modeset->connectors[i]->connector_type !=
			    DRM_MODE_CONNECTOR_TV)
				tv = false;

		if (tv) {
			tv_w = max(tv_w, w);
			tv_h = max(tv_h, h);
		} else {
			other_w = max(other_w, w);
			other_h = max(other_h, h);
		}
	}
	mutex_unlock(&helper->client.modeset_mutex);

	if (other_w && other_h) {
		*width = other_w;
		*height = other_h;
		return false;
	}

	if (tv_w && tv_h) {
		*width = tv_w;
		*height = tv_h;
		return true;
	}

	return false;
}

static int rockchip_fbdev_mmap(struct fb_info *info,
			       struct vm_area_struct *vma)
{
	struct drm_fb_helper *helper = info->par;
	struct rockchip_drm_private *private = helper->dev->dev_private;

	return drm_gem_mmap_obj(private->fbdev_bo, private->fbdev_bo->size, vma);
}

static const struct fb_ops rockchip_drm_fbdev_ops = {
	.owner		= THIS_MODULE,
	DRM_FB_HELPER_DEFAULT_OPS,
	.fb_mmap	= rockchip_fbdev_mmap,
	.fb_fillrect	= drm_fb_helper_cfb_fillrect,
	.fb_copyarea	= drm_fb_helper_cfb_copyarea,
	.fb_imageblit	= drm_fb_helper_cfb_imageblit,
};

static int rockchip_drm_fbdev_create(struct drm_fb_helper *helper,
				     struct drm_fb_helper_surface_size *sizes)
{
	struct rockchip_drm_private *private = helper->dev->dev_private;
	struct drm_mode_fb_cmd2 mode_cmd = { 0 };
	struct drm_device *dev = helper->dev;
	struct rockchip_gem_object *rk_obj;
	unsigned int console_w, console_h;
	struct drm_framebuffer *fb;
	unsigned int bytes_per_pixel;
	unsigned int mode_w, mode_h;
	unsigned long offset;
	struct fb_info *fbi;
	bool tv_only;
	size_t size;
	int ret;

	bytes_per_pixel = DIV_ROUND_UP(sizes->surface_bpp, 8);

	mode_cmd.width = sizes->surface_width;
	mode_cmd.height = sizes->surface_height;
	mode_cmd.pitches[0] = sizes->surface_width * bytes_per_pixel;
	mode_cmd.pixel_format = drm_mode_legacy_fb_format(sizes->surface_bpp,
		sizes->surface_depth);

	size = mode_cmd.pitches[0] * mode_cmd.height;

	rk_obj = rockchip_gem_create_object(dev, size, true, 0);
	if (IS_ERR(rk_obj))
		return -ENOMEM;

	private->fbdev_bo = &rk_obj->base;

	fbi = drm_fb_helper_alloc_fbi(helper);
	if (IS_ERR(fbi)) {
		DRM_DEV_ERROR(dev->dev, "Failed to create framebuffer info.\n");
		ret = PTR_ERR(fbi);
		goto out;
	}

	helper->fb = rockchip_drm_framebuffer_init(dev, &mode_cmd,
						   private->fbdev_bo);
	if (IS_ERR(helper->fb)) {
		DRM_DEV_ERROR(dev->dev,
			      "Failed to allocate DRM framebuffer.\n");
		ret = PTR_ERR(helper->fb);
		goto out;
	}

	fbi->fbops = &rockchip_drm_fbdev_ops;

	fb = helper->fb;
	drm_fb_helper_fill_info(fbi, helper, sizes);

	mode_w = fbi->var.xres;
	mode_h = fbi->var.yres;
	tv_only = rockchip_fbdev_console_mode(helper, &mode_w, &mode_h);

	/* drm_fb_helper_check_var() rejects a visible size past the buffer */
	mode_w = min_t(unsigned int, mode_w, fb->width);
	mode_h = min_t(unsigned int, mode_h, fb->height);

	console_w = mode_w;
	console_h = mode_h;

	/*
	 * A television overscans: the edges of a full-size console are not on
	 * the tube at all. Lay fbcon out smaller and start it part-way into the
	 * buffer. The VP's overscan margins would do it too, but they drive a
	 * post-scaler - CRTC state, so every client on the video port gets a
	 * filtered downscale it never asked for. This is the console only.
	 */
	if (tv_only && fbdev_tv_margin >= 50 && fbdev_tv_margin < 100) {
		console_w = (mode_w * fbdev_tv_margin / 100) & ~1U;
		console_h = (mode_h * fbdev_tv_margin / 100) & ~1U;
	}

	offset = fbi->var.xoffset * bytes_per_pixel;
	offset += fbi->var.yoffset * fb->pitches[0];
	offset += ((mode_h - console_h) / 2) * fb->pitches[0];
	offset += ((mode_w - console_w) / 2) * bytes_per_pixel;

	if (console_w != fbi->var.xres || console_h != fbi->var.yres) {
		DRM_DEV_INFO(dev->dev, "console %ux%u in a %ux%u %s mode\n",
			     console_w, console_h, mode_w, mode_h,
			     tv_only ? "TV" : "display");

		/*
		 * fix.line_length stays the full pitch, so a console row still
		 * steps one whole scanline of the larger buffer.
		 */
		fbi->var.xres = console_w;
		fbi->var.yres = console_h;
	}

	/*
	 * fbcon pans whenever the virtual size exceeds the visible one, and
	 * drm_fb_helper_check_var() puts xres_virtual and yres_virtual back to
	 * the framebuffer's own size on every set_par, so the pan cannot be
	 * closed off there. Withdraw the capability instead and let fbcon
	 * scroll by redrawing; panning here would scan out past the buffer.
	 */
	if (console_w < fb->width || console_h < fb->height) {
		fbi->fix.xpanstep = 0;
		fbi->fix.ypanstep = 0;
	}

	dev->mode_config.fb_base = 0;
	fbi->screen_base = rk_obj->kvaddr + offset;
	fbi->screen_size = rk_obj->base.size - offset;
	fbi->fix.smem_len = rk_obj->base.size - offset;

	DRM_DEBUG_KMS("FB [%dx%d]-%d kvaddr=%p offset=%ld size=%zu\n",
		      fb->width, fb->height, fb->format->depth,
		      rk_obj->kvaddr,
		      offset, size);

	return 0;

out:
	drm_gem_object_put(&rk_obj->base);
	return ret;
}

static const struct drm_fb_helper_funcs rockchip_drm_fb_helper_funcs = {
	.fb_probe = rockchip_drm_fbdev_create,
};

int rockchip_drm_fbdev_init(struct drm_device *dev)
{
	struct rockchip_drm_private *private = dev->dev_private;
	struct drm_fb_helper *helper;
	int ret;

	if (!dev->mode_config.num_crtc || !dev->mode_config.num_connector)
		return -EINVAL;

	helper = devm_kzalloc(dev->dev, sizeof(*helper), GFP_KERNEL);
	if (!helper)
		return -ENOMEM;
	private->fbdev_helper = helper;

	drm_fb_helper_prepare(dev, helper, &rockchip_drm_fb_helper_funcs);

	ret = drm_fb_helper_init(dev, helper);
	if (ret < 0) {
		DRM_DEV_ERROR(dev->dev,
			      "Failed to initialize drm fb helper - %d.\n",
			      ret);
		return ret;
	}

	ret = drm_fb_helper_initial_config(helper, PREFERRED_BPP);
	if (ret < 0) {
		DRM_DEV_ERROR(dev->dev,
			      "Failed to set initial hw config - %d.\n",
			      ret);
		goto err_drm_fb_helper_fini;
	}

	return 0;

err_drm_fb_helper_fini:
	drm_fb_helper_fini(helper);
	return ret;
}

void rockchip_drm_fbdev_fini(struct drm_device *dev)
{
	struct rockchip_drm_private *private = dev->dev_private;
	struct drm_fb_helper *helper = private->fbdev_helper;

	if (!helper)
		return;

	drm_fb_helper_unregister_fbi(helper);

	if (helper->fb)
		drm_framebuffer_put(helper->fb);

	drm_fb_helper_fini(helper);
}
