/*
 * Copyright (C) 2009 Red Hat <mjg@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

/*
 * Authors:
 *  Matthew Garrett <mjg@redhat.com>
 *
 * Register locations derived from NVClock by Roderick Colenbrander
 */

#include <linux/apple-gmux.h>
#include <linux/backlight.h>
#include <linux/idr.h>

#include "nouveau_drv.h"
#include "nouveau_reg.h"
#include "nouveau_encoder.h"

static struct ida bl_ida;
#define BL_NAME_SIZE 24 // 12 for name + 11 for digits + 1 for '\0'

struct backlight_connector {
	struct list_head head;
	int id;
};

static bool
nouveau_get_backlight_name(char backlight_name[BL_NAME_SIZE], struct backlight_connector
		*connector)
{
	const int nb = ida_simple_get(&bl_ida, 0, 0, GFP_KERNEL);
	if (nb < 0 || nb >= 100)
		return false;
	if (nb > 0)
		snprintf(backlight_name, BL_NAME_SIZE, "nv_backlight%d", nb);
	else
		snprintf(backlight_name, BL_NAME_SIZE, "nv_backlight");
	connector->id = nb;
	return true;
}

static int
nv40_get_intensity(struct backlight_device *bd)
{
	struct nouveau_drm *drm = bl_get_data(bd);
	struct nvif_object *device = &drm->client.device.object;
	int val = (nvif_rd32(device, NV40_PMC_BACKLIGHT) &
				   NV40_PMC_BACKLIGHT_MASK) >> 16;

	return val;
}

static int
nv40_set_intensity(struct backlight_device *bd)
{
	struct nouveau_drm *drm = bl_get_data(bd);
	struct nvif_object *device = &drm->client.device.object;
	int val = bd->props.brightness;
	int reg = nvif_rd32(device, NV40_PMC_BACKLIGHT);

	nvif_wr32(device, NV40_PMC_BACKLIGHT,
		 (val << 16) | (reg & ~NV40_PMC_BACKLIGHT_MASK));

	return 0;
}

static const struct backlight_ops nv40_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.get_brightness = nv40_get_intensity,
	.update_status = nv40_set_intensity,
};

static int
nv40_backlight_init(struct drm_connector *connector)
{
	struct nouveau_drm *drm = nouveau_drm(connector->dev);
	struct nvif_object *device = &drm->client.device.object;
	struct backlight_properties props;
	struct backlight_device *bd;
	struct backlight_connector bl_connector;
	char backlight_name[BL_NAME_SIZE];

	if (!(nvif_rd32(device, NV40_PMC_BACKLIGHT) & NV40_PMC_BACKLIGHT_MASK))
		return 0;

	memset(&props, 0, sizeof(struct backlight_properties));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = 31;
	if (!nouveau_get_backlight_name(backlight_name, &bl_connector)) {
		NV_ERROR(drm, "Failed to retrieve a unique name for the backlight interface\n");
		return 0;
	}
	bd = backlight_device_register(backlight_name , connector->kdev, drm,
				       &nv40_bl_ops, &props);

	if (IS_ERR(bd)) {
		if (bl_connector.id >= 0)
			ida_simple_remove(&bl_ida, bl_connector.id);
		return PTR_ERR(bd);
	}
	list_add(&bl_connector.head, &drm->bl_connectors);
	drm->backlight = bd;
	bd->props.brightness = nv40_get_intensity(bd);
	backlight_update_status(bd);

	return 0;
}

static int
nv50_get_intensity(struct backlight_device *bd)
{
	struct nouveau_encoder *nv_encoder = bl_get_data(bd);
	struct nouveau_drm *drm = nouveau_drm(nv_encoder->base.base.dev);
	struct nvif_object *device = &drm->client.device.object;
	int or = ffs(nv_encoder->dcb->or) - 1;
	u32 div = 1025;
	u32 val;

	val  = nvif_rd32(device, NV50_PDISP_SOR_PWM_CTL(or));
	val &= NV50_PDISP_SOR_PWM_CTL_VAL;
	return ((val * 100) + (div / 2)) / div;
}

static int
nv50_set_intensity(struct backlight_device *bd)
{
	struct nouveau_encoder *nv_encoder = bl_get_data(bd);
	struct nouveau_drm *drm = nouveau_drm(nv_encoder->base.base.dev);
	struct nvif_object *device = &drm->client.device.object;
	int or = ffs(nv_encoder->dcb->or) - 1;
	u32 div = 1025;
	u32 val = (bd->props.brightness * div) / 100;

	nvif_wr32(device, NV50_PDISP_SOR_PWM_CTL(or),
			NV50_PDISP_SOR_PWM_CTL_NEW | val);
	return 0;
}

static const struct backlight_ops nv50_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.get_brightness = nv50_get_intensity,
	.update_status = nv50_set_intensity,
};

static int
nva3_get_intensity(struct backlight_device *bd)
{
	struct nouveau_encoder *nv_encoder = bl_get_data(bd);
	struct nouveau_drm *drm = nouveau_drm(nv_encoder->base.base.dev);
	struct nvif_object *device = &drm->client.device.object;
	int or = ffs(nv_encoder->dcb->or) - 1;
	u32 div, val;

	div  = nvif_rd32(device, NV50_PDISP_SOR_PWM_DIV(or));
	val  = nvif_rd32(device, NV50_PDISP_SOR_PWM_CTL(or));
	val &= NVA3_PDISP_SOR_PWM_CTL_VAL;
	if (div && div >= val)
		return ((val * 100) + (div / 2)) / div;

	return 100;
}

static int
nva3_set_intensity(struct backlight_device *bd)
{
	struct nouveau_encoder *nv_encoder = bl_get_data(bd);
	struct nouveau_drm *drm = nouveau_drm(nv_encoder->base.base.dev);
	struct nvif_object *device = &drm->client.device.object;
	int or = ffs(nv_encoder->dcb->or) - 1;
	u32 div, val;

	div = nvif_rd32(device, NV50_PDISP_SOR_PWM_DIV(or));
	val = (bd->props.brightness * div) / 100;
	if (div) {
		nvif_wr32(device, NV50_PDISP_SOR_PWM_CTL(or), val |
				NV50_PDISP_SOR_PWM_CTL_NEW |
				NVA3_PDISP_SOR_PWM_CTL_UNK);
		return 0;
	}

	return -EINVAL;
}

static const struct backlight_ops nva3_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.get_brightness = nva3_get_intensity,
	.update_status = nva3_set_intensity,
};

static int
nv50_backlight_init(struct drm_connector *connector)
{
	struct nouveau_drm *drm = nouveau_drm(connector->dev);
	struct nvif_object *device = &drm->client.device.object;
	struct nouveau_encoder *nv_encoder;
	struct backlight_properties props;
	struct backlight_device *bd;
	const struct backlight_ops *ops;
	struct backlight_connector bl_connector;
	char backlight_name[BL_NAME_SIZE];

	nv_encoder = find_encoder(connector, DCB_OUTPUT_LVDS);
	if (!nv_encoder) {
		nv_encoder = find_encoder(connector, DCB_OUTPUT_DP);
		if (!nv_encoder)
			return -ENODEV;
	}

	if (!nvif_rd32(device, NV50_PDISP_SOR_PWM_CTL(ffs(nv_encoder->dcb->or) - 1)))
		return 0;

	if (drm->client.device.info.chipset <= 0xa0 ||
	    drm->client.device.info.chipset == 0xaa ||
	    drm->client.device.info.chipset == 0xac)
		ops = &nv50_bl_ops;
	else
		ops = &nva3_bl_ops;

	memset(&props, 0, sizeof(struct backlight_properties));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = 100;
	if (!nouveau_get_backlight_name(backlight_name, &bl_connector)) {
		NV_ERROR(drm, "Failed to retrieve a unique name for the backlight interface\n");
		return 0;
	}
	bd = backlight_device_register(backlight_name , connector->kdev,
				       nv_encoder, ops, &props);

	if (IS_ERR(bd)) {
		if (bl_connector.id >= 0)
			ida_simple_remove(&bl_ida, bl_connector.id);
		return PTR_ERR(bd);
	}

	list_add(&bl_connector.head, &drm->bl_connectors);
	drm->backlight = bd;
	bd->props.brightness = bd->ops->get_brightness(bd);
	backlight_update_status(bd);
	return 0;
}

int
nouveau_backlight_init(struct drm_device *dev)
{
	struct nouveau_drm *drm = nouveau_drm(dev);
	struct nvif_device *device = &drm->client.device;
	struct drm_connector *connector;
	struct drm_connector_list_iter conn_iter;

	INIT_LIST_HEAD(&drm->bl_connectors);

	if (apple_gmux_present()) {
		NV_INFO(drm, "Apple GMUX detected: not registering Nouveau backlight interface\n");
		return 0;
	}

	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		if (connector->connector_type != DRM_MODE_CONNECTOR_LVDS &&
		    connector->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		switch (device->info.family) {
		case NV_DEVICE_INFO_V0_CURIE:
			return nv40_backlight_init(connector);
		case NV_DEVICE_INFO_V0_TESLA:
		case NV_DEVICE_INFO_V0_FERMI:
		case NV_DEVICE_INFO_V0_KEPLER:
		case NV_DEVICE_INFO_V0_MAXWELL:
			return nv50_backlight_init(connector);
		default:
			break;
		}
	}
	drm_connector_list_iter_end(&conn_iter);

	return 0;
}

void
nouveau_backlight_exit(struct drm_device *dev)
{
	struct nouveau_drm *drm = nouveau_drm(dev);
	struct backlight_connector *connector;

	list_for_each_entry(connector, &drm->bl_connectors, head) {
		if (connector->id >= 0)
			ida_simple_remove(&bl_ida, connector->id);
	}

	if (drm->backlight) {
		backlight_device_unregister(drm->backlight);
		drm->backlight = NULL;
	}
}

void
nouveau_backlight_ctor(void)
{
	ida_init(&bl_ida);
}

void
nouveau_backlight_dtor(void)
{
	ida_destroy(&bl_ida);
}
