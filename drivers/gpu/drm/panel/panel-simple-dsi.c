/*
 * Copyright (C) 2021
 * This simple dsi driver porting from rock-chip panel-simple.c on linux-4.4
 */

#include <linux/backlight.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <video/display_timing.h>
#include <video/mipi_display.h>
#include <linux/of_device.h>
#include <video/of_display_timing.h>
#include <linux/of_graph.h>
#include <video/videomode.h>
#include <linux/delay.h>

struct cmd_ctrl_hdr {
	u8 dtype;	/* data type */
	u8 wait;	/* ms */
	u8 dlen;	/* payload len */
} __packed;

struct cmd_desc {
	struct cmd_ctrl_hdr dchdr;
	u8 *payload;
};

struct panel_cmds {
	u8 *buf;
	int blen;
	struct cmd_desc *cmds;
	int cmd_cnt;
};

struct panel_desc {
	const struct drm_display_mode *modes;
	unsigned int num_modes;
	const struct display_timing *timings;
	unsigned int num_timings;

	unsigned int bpc;

	struct {
		unsigned int width;
		unsigned int height;
	} size;

	/**
	 * @reset: the time (in milliseconds) indicates the delay time
	 *         after the panel to operate reset gpio
	 * @init: the time (in milliseconds) that it takes for the panel to
	 *           power on and dsi host can send command to panel
	 * @prepare: the time (in milliseconds) that it takes for the panel to
	 *           become ready and start receiving video data
	 * @enable: the time (in milliseconds) that it takes for the panel to
	 *          display the first valid frame after starting to receive
	 *          video data
	 * @disable: the time (in milliseconds) that it takes for the panel to
	 *           turn the display off (no content is visible)
	 * @unprepare: the time (in milliseconds) that it takes for the panel
	 *             to power itself down completely
	 */
	struct {
		unsigned int reset;
		unsigned int init;
		unsigned int prepare;
		unsigned int enable;
		unsigned int disable;
		unsigned int unprepare;
	} delay;

	u32 bus_format;
};

struct panel_simple {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;
	bool prepared;
	bool enabled;
	bool power_invert;

	struct device *dev;
	const struct panel_desc *desc;

	struct regulator *supply;

	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset_gpio;
	int cmd_type;

	struct panel_cmds *on_cmds;
	struct panel_cmds *off_cmds;
	struct device_node *np_crtc;
	
	int reset_level;
	enum drm_panel_orientation orientation;
};

enum rockchip_cmd_type {
	CMD_TYPE_DEFAULT,
	CMD_TYPE_SPI,
	CMD_TYPE_MCU
};

static void panel_simple_sleep(unsigned int msec)
{
	if (msec > 20)
		msleep(msec);
	else
		usleep_range(msec * 1000, (msec + 1) * 1000);
}

static inline struct panel_simple *to_panel_simple(struct drm_panel *panel)
{
	return container_of(panel, struct panel_simple, base);
}

static void panel_simple_cmds_cleanup(struct panel_simple *p)
{
	if (p->on_cmds) {
		kfree(p->on_cmds->buf);
		kfree(p->on_cmds->cmds);
	}

	if (p->off_cmds) {
		kfree(p->off_cmds->buf);
		kfree(p->off_cmds->cmds);
	}
}

static int panel_simple_parse_cmds(struct device *dev,
				   const u8 *data, int blen,
				   struct panel_cmds *pcmds)
{
	unsigned int len;
	char *buf, *bp;
	struct cmd_ctrl_hdr *dchdr;
	int i, cnt;

	if (!pcmds)
		return -EINVAL;

	buf = kmemdup(data, blen, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* scan init commands */
	bp = buf;
	len = blen;
	cnt = 0;
	while (len > sizeof(*dchdr)) {
		dchdr = (struct cmd_ctrl_hdr *)bp;

		if (dchdr->dlen > len) {
			dev_err(dev, "%s: error, len=%d", __func__,
				dchdr->dlen);
			return -EINVAL;
		}

		bp += sizeof(*dchdr);
		len -= sizeof(*dchdr);
		bp += dchdr->dlen;
		len -= dchdr->dlen;
		cnt++;
	}

	if (len != 0) {
		dev_err(dev, "%s: dcs_cmd=%x len=%d error!",
			__func__, buf[0], blen);
		kfree(buf);
		return -EINVAL;
	}

	pcmds->cmds = kcalloc(cnt, sizeof(struct cmd_desc), GFP_KERNEL);
	if (!pcmds->cmds) {
		kfree(buf);
		return -ENOMEM;
	}

	pcmds->cmd_cnt = cnt;
	pcmds->buf = buf;
	pcmds->blen = blen;

	bp = buf;
	len = blen;
	for (i = 0; i < cnt; i++) {
		dchdr = (struct cmd_ctrl_hdr *)bp;
		len -= sizeof(*dchdr);
		bp += sizeof(*dchdr);
		pcmds->cmds[i].dchdr = *dchdr;
		pcmds->cmds[i].payload = bp;
		bp += dchdr->dlen;
		len -= dchdr->dlen;
	}

	return 0;
}

static int panel_simple_dsi_send_cmds(struct panel_simple *panel,
				      struct panel_cmds *cmds)
{
	struct mipi_dsi_device *dsi = panel->dsi;
	int i, err;

	if (!cmds)
		return -EINVAL;

	for (i = 0; i < cmds->cmd_cnt; i++) {
		struct cmd_desc *cmd = &cmds->cmds[i];

		switch (cmd->dchdr.dtype) {
		case MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM:
		case MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM:
		case MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM:
		case MIPI_DSI_GENERIC_LONG_WRITE:
			err = mipi_dsi_generic_write(dsi, cmd->payload,
						     cmd->dchdr.dlen);
			break;
		case MIPI_DSI_DCS_SHORT_WRITE:
		case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
		case MIPI_DSI_DCS_LONG_WRITE:
			err = mipi_dsi_dcs_write_buffer(dsi, cmd->payload,
							cmd->dchdr.dlen);
			break;
		default:
			return -EINVAL;
		}

		if (err < 0)
			dev_err(panel->dev, "failed to write dcs cmd: %d\n",
				err);

		if (cmd->dchdr.wait)
			panel_simple_sleep(cmd->dchdr.wait);
	}

	return 0;
}

static int panel_simple_get_cmds(struct panel_simple *panel)
{
	const void *data;
	int len;
	int err;

	data = of_get_property(panel->dev->of_node, "panel-init-sequence",
			       &len);
	if (data) {
		panel->on_cmds = devm_kzalloc(panel->dev,
					      sizeof(*panel->on_cmds),
					      GFP_KERNEL);
		if (!panel->on_cmds)
			return -ENOMEM;

		err = panel_simple_parse_cmds(panel->dev, data, len,
					      panel->on_cmds);
		if (err) {
			dev_err(panel->dev, "failed to parse panel init sequence\n");
			return err;
		}
	}

	data = of_get_property(panel->dev->of_node, "panel-exit-sequence",
			       &len);
	if (data) {
		panel->off_cmds = devm_kzalloc(panel->dev,
					       sizeof(*panel->off_cmds),
					       GFP_KERNEL);
		if (!panel->off_cmds)
			return -ENOMEM;

		err = panel_simple_parse_cmds(panel->dev, data, len,
					      panel->off_cmds);
		if (err) {
			dev_err(panel->dev, "failed to parse panel exit sequence\n");
			return err;
		}
	}
	return 0;
}

static int panel_simple_get_modes(struct drm_panel *panel,struct drm_connector *connector)
{
	struct panel_simple *p = to_panel_simple(panel);
	struct drm_device *drm = connector->dev;
	struct drm_display_mode *mode;
	struct device_node *timings_np;
	int ret;

	timings_np = of_get_child_by_name(panel->dev->of_node,
					  "display-timings");
	if (!timings_np) {
		dev_dbg(panel->dev, "failed to find display-timings node\n");
		return 0;
	}

	of_node_put(timings_np);
	mode = drm_mode_create(drm);
	if (!mode)
		return 0;

	ret = of_get_drm_display_mode(panel->dev->of_node, mode, (u32 *)&p->desc->bus_format,
				      OF_USE_NATIVE_MODE);
	if (ret) {
		dev_dbg(panel->dev, "failed to find dts display timings\n");
		drm_mode_destroy(drm, mode);
		return 0;
	}

	drm_mode_set_name(mode);
	mode->type |= DRM_MODE_TYPE_PREFERRED;

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	drm_mode_probed_add(connector, mode);

	/*
	 * TODO: Remove once all drm drivers call
	 * drm_connector_set_orientation_from_panel()
	 */
	drm_connector_set_panel_orientation(connector, p->orientation);
	
	return 1;
}

static int panel_simple_regulator_enable(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);
	int err = 0;

	if (p->power_invert) {
		if (regulator_is_enabled(p->supply) > 0)
			regulator_disable(p->supply);
	} else {
		err = regulator_enable(p->supply);
		if (err < 0) {
			dev_err(panel->dev, "failed to enable supply: %d\n",
				err);
			return err;
		}
	}

	return err;
}

static int panel_simple_regulator_disable(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);
	int err = 0;

	if (p->power_invert) {
		if (!regulator_is_enabled(p->supply)) {
			err = regulator_enable(p->supply);
			if (err < 0) {
				dev_err(panel->dev, "failed to enable supply: %d\n",
					err);
				return err;
			}
		}
	} else {
		regulator_disable(p->supply);
	}

	return err;
}

static int panel_simple_disable(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);

	if (!p->enabled)
		return 0;

	if (p->desc && p->desc->delay.disable)
		panel_simple_sleep(p->desc->delay.disable);

	p->enabled = false;

	return 0;
}

static int panel_simple_unprepare(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);
	int err = 0;

	if (!p->prepared)
		return 0;

	if (p->off_cmds) {
		if (p->dsi)
			err = panel_simple_dsi_send_cmds(p, p->off_cmds);
		if (err)
			dev_err(p->dev, "failed to send off cmds\n");
	}

	if (p->reset_gpio)
		gpiod_direction_output(p->reset_gpio, !p->reset_level);

	if (p->enable_gpio)
		gpiod_direction_output(p->enable_gpio, 0);

	panel_simple_regulator_disable(panel);

	if (p->desc && p->desc->delay.unprepare)
		panel_simple_sleep(p->desc->delay.unprepare);

	p->prepared = false;

	return 0;
}

static int panel_simple_prepare(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);
	int err;

	if (p->prepared)
		return 0;

	err = panel_simple_regulator_enable(panel);
	if (err < 0) {
		dev_err(panel->dev, "failed to enable supply: %d\n", err);
		return err;
	}

	if (p->enable_gpio)
		gpiod_direction_output(p->enable_gpio, 1);

	if (p->desc && p->desc->delay.prepare)
		panel_simple_sleep(p->desc->delay.prepare);

	if (p->reset_gpio)
		gpiod_direction_output(p->reset_gpio, !p->reset_level);
	
	if (p->desc && p->desc->delay.reset)
		panel_simple_sleep(p->desc->delay.reset);

	if (p->reset_gpio)
		gpiod_direction_output(p->reset_gpio, p->reset_level);
		
	if (p->desc && p->desc->delay.init)
		panel_simple_sleep(p->desc->delay.init);

	p->prepared = true;

	return 0;
}

static int panel_simple_enable(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);
	int err = 0;

	if (p->enabled)
		return 0;

	if (p->desc && p->desc->delay.enable)
		panel_simple_sleep(p->desc->delay.enable);

	if (p->on_cmds) {
		if (p->dsi)
			err = panel_simple_dsi_send_cmds(p, p->on_cmds);
		if (err)
			dev_err(p->dev, "failed to send on cmds\n");
	}

	p->enabled = true;

	return err;
}

static int panel_simple_get_timings(struct drm_panel *panel,
				    unsigned int num_timings,
				    struct display_timing *timings)
{
	struct panel_simple *p = to_panel_simple(panel);
	unsigned int i;

	if (!p->desc)
		return 0;

	if (p->desc->num_timings < num_timings)
		num_timings = p->desc->num_timings;

	if (timings)
		for (i = 0; i < num_timings; i++)
			timings[i] = p->desc->timings[i];

	return p->desc->num_timings;
}

static enum drm_panel_orientation panel_simple_get_orientation(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);

	return p->orientation;
}


static const struct drm_panel_funcs panel_simple_funcs = {
	.disable = panel_simple_disable,
	.unprepare = panel_simple_unprepare,
	.prepare = panel_simple_prepare,
	.enable = panel_simple_enable,
	.get_modes = panel_simple_get_modes,
	.get_orientation = panel_simple_get_orientation,
	.get_timings = panel_simple_get_timings,
};

static int panel_simple_probe(struct device *dev, const struct panel_desc *desc)
{
	struct panel_simple *panel;
	struct panel_desc *of_desc;
	u32 val;
	int err;

	panel = devm_kzalloc(dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;

	if (!desc)
		of_desc = devm_kzalloc(dev, sizeof(*of_desc), GFP_KERNEL);
	else
		of_desc = devm_kmemdup(dev, desc, sizeof(*of_desc), GFP_KERNEL);

	if (!of_property_read_u32(dev->of_node, "bus-format", &val))
		of_desc->bus_format = val;
	if (!of_property_read_u32(dev->of_node, "bpc", &val))
		of_desc->bpc = val;
	if (!of_property_read_u32(dev->of_node, "prepare-delay-ms", &val))
		of_desc->delay.prepare = val;
	if (!of_property_read_u32(dev->of_node, "enable-delay-ms", &val))
		of_desc->delay.enable = val;
	if (!of_property_read_u32(dev->of_node, "disable-delay-ms", &val))
		of_desc->delay.disable = val;
	if (!of_property_read_u32(dev->of_node, "unprepare-delay-ms", &val))
		of_desc->delay.unprepare = val;
	if (!of_property_read_u32(dev->of_node, "reset-delay-ms", &val))
		of_desc->delay.reset = val;
	if (!of_property_read_u32(dev->of_node, "init-delay-ms", &val))
		of_desc->delay.init = val;
	if (!of_property_read_u32(dev->of_node, "width-mm", &val))
		of_desc->size.width = val;
	if (!of_property_read_u32(dev->of_node, "height-mm", &val))
		of_desc->size.height = val;

	panel->enabled = false;
	panel->prepared = false;
	panel->desc = of_desc;
	panel->dev = dev;

	err = panel_simple_get_cmds(panel);
	if (err) {
		dev_err(dev, "failed to get init cmd: %d\n", err);
		return err;
	}
	panel->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(panel->supply))
		return PTR_ERR(panel->supply);

	panel->enable_gpio = devm_gpiod_get_optional(dev, "enable", 0);
	if (IS_ERR(panel->enable_gpio)) {
		err = PTR_ERR(panel->enable_gpio);
		dev_err(dev, "failed to request enable GPIO: %d\n", err);
		return err;
	}

	panel->reset_gpio = devm_gpiod_get_optional(dev, "reset", 0);
	if (IS_ERR(panel->reset_gpio)) {
		err = PTR_ERR(panel->reset_gpio);
		dev_err(dev, "failed to request reset GPIO: %d\n", err);
		return err;
	}
	
	if (!of_property_read_u32(dev->of_node, "reset-level", &val)) {
		panel->reset_level = val;
	} else {
		panel->reset_level = 0;
	}
	
	err = of_drm_get_panel_orientation(dev->of_node, &panel->orientation);
	if (err) {
		dev_err(dev, "%pOF: failed to get orientation %d\n", dev->of_node, err);
		return err;
	}

	panel->cmd_type = CMD_TYPE_DEFAULT;

	panel->power_invert =
			of_property_read_bool(dev->of_node, "power-invert");

	drm_panel_init(&panel->base, dev, &panel_simple_funcs,DRM_MODE_CONNECTOR_DSI);
	panel->base.dev = dev;
	panel->base.funcs = &panel_simple_funcs;

	err = drm_panel_of_backlight(&panel->base);
	if (err)
		return err;

	drm_panel_add(&panel->base);

	dev_set_drvdata(dev, panel);

	return 0;
}

static int panel_simple_remove(struct device *dev)
{
	struct panel_simple *panel = dev_get_drvdata(dev);

	drm_panel_remove(&panel->base);

	panel_simple_disable(&panel->base);
	panel_simple_unprepare(&panel->base);

	panel_simple_cmds_cleanup(panel);

	return 0;
}

static void panel_simple_shutdown(struct device *dev)
{
	struct panel_simple *panel = dev_get_drvdata(dev);

	panel_simple_disable(&panel->base);

	if (panel->prepared) {
		if (panel->reset_gpio)
			gpiod_direction_output(panel->reset_gpio, !panel->reset_level);

		if (panel->enable_gpio)
			gpiod_direction_output(panel->enable_gpio, 0);

		panel_simple_regulator_disable(&panel->base);
	}
}

struct panel_desc_dsi {
	struct panel_desc desc;

	unsigned long flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;
};

/*
static const struct drm_display_mode panasonic_vvx10f004b00_mode = {
	.clock = 157200,
	.hdisplay = 1920,
	.hsync_start = 1920 + 154,
	.hsync_end = 1920 + 154 + 16,
	.htotal = 1920 + 154 + 16 + 32,
	.vdisplay = 1200,
	.vsync_start = 1200 + 17,
	.vsync_end = 1200 + 17 + 2,
	.vtotal = 1200 + 17 + 2 + 16,
	.vrefresh = 60,
};
static const struct panel_desc_dsi panasonic_vvx10f004b00 = {
	.desc = {
		.modes = &panasonic_vvx10f004b00_mode,
		.num_modes = 1,
		.bpc = 8,
		.size = {
			.width = 217,
			.height = 136,
		},
	},
	.flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
		 MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};
*/

static const struct of_device_id dsi_of_match[] = {
	{
		.compatible = "panel-dsi-simple",
		.data = NULL
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, dsi_of_match);

static int panel_simple_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct panel_simple *panel;
	const struct panel_desc_dsi *desc;
	const struct of_device_id *id;
	const struct panel_desc *pdesc;
	int err;
	u32 val;

	id = of_match_node(dsi_of_match, dev->of_node);
	if (!id)
		return -ENODEV;

	desc = id->data;

	if (desc) {
		dsi->mode_flags = desc->flags;
		dsi->format = desc->format;
		dsi->lanes = desc->lanes;
		pdesc = &desc->desc;
	} else {
		pdesc = NULL;
	}

	err = panel_simple_probe(dev, pdesc);
	if (err < 0)
		return err;

	panel = dev_get_drvdata(dev);
	panel->dsi = dsi;

	if (!of_property_read_u32(dev->of_node, "dsi,flags", &val))
		dsi->mode_flags = val;

	if (!of_property_read_u32(dev->of_node, "dsi,format", &val))
		dsi->format = val;

	if (!of_property_read_u32(dev->of_node, "dsi,lanes", &val))
		dsi->lanes = val;

	err = mipi_dsi_attach(dsi);

	if (err)
		panel_simple_remove(&dsi->dev);

	return err;
}

static void panel_simple_dsi_remove(struct mipi_dsi_device *dsi)
{
	int err;

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", err);

	panel_simple_remove(&dsi->dev);
}

static void panel_simple_dsi_shutdown(struct mipi_dsi_device *dsi)
{
	panel_simple_shutdown(&dsi->dev);
}

static struct mipi_dsi_driver panel_simple_dsi_driver = {
	.driver = {
		.name = "panel-dsi-simple",
		.of_match_table = dsi_of_match,
	},
	.probe = panel_simple_dsi_probe,
	.remove = panel_simple_dsi_remove,
	.shutdown = panel_simple_dsi_shutdown,
};

module_mipi_dsi_driver(panel_simple_dsi_driver);

MODULE_AUTHOR("iamdrq <iamdrq@qq.com>");
MODULE_DESCRIPTION("DRM Driver for DSI Simple Panels");
MODULE_LICENSE("GPL");
