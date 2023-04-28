/*
 * Copyright 2020 TechNexion Ltd.
 *
 * Author: Richard Hu <richard.hu@technexion.com>
 *
 * SPDX-License-Identifier:     GPL-2.0+
 */

#include <common.h>
#include <command.h>
#include <env.h>
#include <init.h>
#include <malloc.h>
#include <errno.h>
#include <asm/io.h>
#include <miiphy.h>
#include <netdev.h>
#include <linux/delay.h>
#include <asm/mach-imx/iomux-v3.h>
#include <asm-generic/gpio.h>
#include <fsl_esdhc_imx.h>
#include <mmc.h>
#include <asm/arch/imx8mq_pins.h>
#include <asm/arch/sys_proto.h>
#include <asm/mach-imx/gpio.h>
#include <asm/mach-imx/mxc_i2c.h>
#include <i2c.h>
#include <asm/arch/clock.h>
#include <spl.h>
#include <usb.h>
#include <asm/armv8/mmu.h>
#include <dwc3-uboot.h>
#include <splash.h>

DECLARE_GLOBAL_DATA_PTR;

#define UART_PAD_CTRL	(PAD_CTL_DSE6 | PAD_CTL_FSEL1)

#define WDOG_PAD_CTRL	(PAD_CTL_DSE6 | PAD_CTL_HYS | PAD_CTL_PUE)

static iomux_v3_cfg_t const wdog_pads[] = {
	IMX8MQ_PAD_GPIO1_IO02__WDOG1_WDOG_B | MUX_PAD_CTRL(WDOG_PAD_CTRL),
};

static iomux_v3_cfg_t const uart_pads[] = {
	IMX8MQ_PAD_UART1_RXD__UART1_RX | MUX_PAD_CTRL(UART_PAD_CTRL),
	IMX8MQ_PAD_UART1_TXD__UART1_TX | MUX_PAD_CTRL(UART_PAD_CTRL),
};

int board_early_init_f(void)
{
	struct wdog_regs *wdog = (struct wdog_regs *)WDOG1_BASE_ADDR;

	imx_iomux_v3_setup_multiple_pads(wdog_pads, ARRAY_SIZE(wdog_pads));
	set_wdog_reset(wdog);

	imx_iomux_v3_setup_multiple_pads(uart_pads, ARRAY_SIZE(uart_pads));

	return 0;
}

int board_phys_sdram_size(phys_size_t *size)
{
	if (!size)
		return -EINVAL;

	/*************************************************
	ToDo: It's a dirty workaround to store the
	information of DDR size into start address of TCM.
	It'd be better to detect DDR size from DDR controller.
	**************************************************/
	u32 ddr_size = readl(MCU_BOOTROM_BASE_ADDR);

	switch (ddr_size) {
	case 0x4: /* DRAM size: 4GB */
		*size = SZ_4G;
		break;
	case 0x3: /* DRAM size: 3GB */
		*size = SZ_3G;
		break;
	case 0x2: /* DRAM size: 2GB */
		*size = SZ_2G;
		break;
	case 0x1: /* DRAM size: 1GB */
		*size = SZ_1G;
		break;
	default:
		puts("Unknown DDR type!!!\n");
	}

	return 0;
}

/* Get the top of usable RAM */
ulong board_get_usable_ram_top(ulong total_size)
{
	if(gd->ram_top > 0x100000000)
		gd->ram_top = 0x100000000;

	return gd->ram_top;
}

#ifdef CONFIG_FEC_MXC
static int setup_fec(void)
{
	struct iomuxc_gpr_base_regs *gpr =
		(struct iomuxc_gpr_base_regs *)IOMUXC_GPR_BASE_ADDR;

	/* Use 125M anatop REF_CLK1 for ENET1, not from external */
	clrsetbits_le32(&gpr->gpr[1],
		IOMUXC_GPR_GPR1_GPR_ENET1_TX_CLK_SEL_MASK, 0);
	return set_clk_enet(ENET_125MHZ);
}

int board_phy_config(struct phy_device *phydev)
{
#ifndef CONFIG_DM_ETH
	/* enable rgmii rxc skew and phy mode select to RGMII copper */
	phy_write(phydev, MDIO_DEVAD_NONE, 0x1d, 0x1f);
	phy_write(phydev, MDIO_DEVAD_NONE, 0x1e, 0x8);

	phy_write(phydev, MDIO_DEVAD_NONE, 0x1d, 0x00);
	phy_write(phydev, MDIO_DEVAD_NONE, 0x1e, 0x82ee);
	phy_write(phydev, MDIO_DEVAD_NONE, 0x1d, 0x05);
	phy_write(phydev, MDIO_DEVAD_NONE, 0x1e, 0x100);
#endif
	if (phydev->drv->config)
		phydev->drv->config(phydev);
	return 0;
}
#endif

#define CSI1_RST IMX_GPIO_NR(1, 12)
#define CSI2_RST IMX_GPIO_NR(3, 16)

static iomux_v3_cfg_t const csi_rst_pads[] = {
	IMX8MQ_PAD_GPIO1_IO12__GPIO1_IO12 | MUX_PAD_CTRL(NO_PAD_CTRL),
	IMX8MQ_PAD_NAND_READY_B__GPIO3_IO16 | MUX_PAD_CTRL(NO_PAD_CTRL),
};

static void setup_csi(void)
{
	imx_iomux_v3_setup_multiple_pads(csi_rst_pads, ARRAY_SIZE(csi_rst_pads));

	gpio_request(CSI1_RST, "csi1_rst");
	gpio_direction_output(CSI1_RST, 0);
	udelay(500);
	gpio_direction_output(CSI1_RST, 1);

	gpio_request(CSI2_RST, "csi2_rst");
	gpio_direction_output(CSI2_RST, 0);
	udelay(500);
	gpio_direction_output(CSI2_RST, 1);
}

#ifdef CONFIG_USB_DWC3

#define USB_PHY_CTRL0			0xF0040
#define USB_PHY_CTRL0_REF_SSP_EN	BIT(2)

#define USB_PHY_CTRL1			0xF0044
#define USB_PHY_CTRL1_RESET		BIT(0)
#define USB_PHY_CTRL1_COMMONONN		BIT(1)
#define USB_PHY_CTRL1_ATERESET		BIT(3)
#define USB_PHY_CTRL1_VDATSRCENB0	BIT(19)
#define USB_PHY_CTRL1_VDATDETENB0	BIT(20)

#define USB_PHY_CTRL2			0xF0048
#define USB_PHY_CTRL2_TXENABLEN0	BIT(8)

static struct dwc3_device dwc3_device_data = {
	.maximum_speed = USB_SPEED_HIGH,
	.base = USB1_BASE_ADDR,
	.dr_mode = USB_DR_MODE_PERIPHERAL,
	.index = 0,
	.power_down_scale = 2,
};

int usb_gadget_handle_interrupts(void)
{
	dwc3_uboot_handle_interrupt(0);
	return 0;
}

static void dwc3_nxp_usb_phy_init(struct dwc3_device *dwc3)
{
	u32 RegData;

	RegData = readl(dwc3->base + USB_PHY_CTRL1);
	RegData &= ~(USB_PHY_CTRL1_VDATSRCENB0 | USB_PHY_CTRL1_VDATDETENB0 |
			USB_PHY_CTRL1_COMMONONN);
	RegData |= USB_PHY_CTRL1_RESET | USB_PHY_CTRL1_ATERESET;
	writel(RegData, dwc3->base + USB_PHY_CTRL1);

	RegData = readl(dwc3->base + USB_PHY_CTRL0);
	RegData |= USB_PHY_CTRL0_REF_SSP_EN;
	writel(RegData, dwc3->base + USB_PHY_CTRL0);

	RegData = readl(dwc3->base + USB_PHY_CTRL2);
	RegData |= USB_PHY_CTRL2_TXENABLEN0;
	writel(RegData, dwc3->base + USB_PHY_CTRL2);

	RegData = readl(dwc3->base + USB_PHY_CTRL1);
	RegData &= ~(USB_PHY_CTRL1_RESET | USB_PHY_CTRL1_ATERESET);
	writel(RegData, dwc3->base + USB_PHY_CTRL1);
}
#endif

#if defined(CONFIG_USB_DWC3) || defined(CONFIG_USB_XHCI_IMX8M)
int board_usb_init(int index, enum usb_init_type init)
{
	int ret = 0;
	imx8m_usb_power(index, true);

	if (index == 0 && init == USB_INIT_DEVICE) {
		dwc3_nxp_usb_phy_init(&dwc3_device_data);
		return dwc3_uboot_init(&dwc3_device_data);
	} else if (index == 0 && init == USB_INIT_HOST) {
		return ret;
	}

	return 0;
}

int board_usb_cleanup(int index, enum usb_init_type init)
{
	int ret = 0;
	if (index == 0 && init == USB_INIT_DEVICE)
			dwc3_uboot_exit(index);

	imx8m_usb_power(index, false);

	return ret;
}
#endif

#ifdef CONFIG_SPLASH_SCREEN
static struct splash_location imx_splash_locations[] = {
	{
		.name = "sf",
		.storage = SPLASH_STORAGE_SF,
		.flags = SPLASH_STORAGE_RAW,
		.offset = 0x100000,
	},
	{
		.name = "mmc_fs",
		.storage = SPLASH_STORAGE_MMC,
		.flags = SPLASH_STORAGE_FS,
		.devpart = "0:1",
	},
	{
		.name = "usb_fs",
		.storage = SPLASH_STORAGE_USB,
		.flags = SPLASH_STORAGE_FS,
		.devpart = "0:1",
	},
	{
		.name = "sata_fs",
		.storage = SPLASH_STORAGE_SATA,
		.flags = SPLASH_STORAGE_FS,
		.devpart = "0:1",
	},
};

/*This function is defined in common/splash.c.
  Declare here to remove warning. */
int splash_video_logo_load(void);

int splash_screen_prepare(void)
{
	imx_splash_locations[1].devpart[0] = mmc_get_env_dev() + '0';
	int ret;
	ret = splash_source_load(imx_splash_locations, ARRAY_SIZE(imx_splash_locations));
	if (!ret)
		return 0;
	else {
		printf("\nNo splash.bmp in boot partition!!\n");
		printf("Using default logo!!\n\n");
		return splash_video_logo_load();
	}
}
#endif /* CONFIG_SPLASH_SCREEN */

#define WL_REG_ON_PAD IMX_GPIO_NR(3, 24)
static iomux_v3_cfg_t const wl_reg_on_pads[] = {
	IMX8MQ_PAD_SAI5_RXD3__GPIO3_IO24 | MUX_PAD_CTRL(NO_PAD_CTRL),
};

#define BT_ON_PAD IMX_GPIO_NR(3, 21)
static iomux_v3_cfg_t const bt_on_pads[] = {
	IMX8MQ_PAD_SAI5_RXD0__GPIO3_IO21 | MUX_PAD_CTRL(NO_PAD_CTRL),
};

void setup_wifi(void)
{
	imx_iomux_v3_setup_multiple_pads(wl_reg_on_pads, ARRAY_SIZE(wl_reg_on_pads));
	imx_iomux_v3_setup_multiple_pads(bt_on_pads, ARRAY_SIZE(bt_on_pads));

	gpio_request(WL_REG_ON_PAD, "wl_reg_on");
	gpio_direction_output(WL_REG_ON_PAD, 0);
	gpio_set_value(WL_REG_ON_PAD, 0);

	gpio_request(BT_ON_PAD, "bt_on");
	gpio_direction_output(BT_ON_PAD, 0);
	gpio_set_value(BT_ON_PAD, 0);
}

int board_init(void)
{
	setup_wifi();

#ifdef CONFIG_FEC_MXC
	setup_fec();
#endif

	setup_csi();

#if defined(CONFIG_USB_DWC3) || defined(CONFIG_USB_XHCI_IMX8M)
	init_usb_clk();
#endif

	return 0;
}

int board_mmc_get_env_dev(int devno)
{
	return devno;
}

static int check_mmc_autodetect(void)
{
	char *autodetect_str = env_get("mmcautodetect");

	if ((autodetect_str != NULL) &&
		(strcmp(autodetect_str, "yes") == 0)) {
		return 1;
	}

	return 0;
}

/* This should be defined for each board */
__weak int mmc_map_to_kernel_blk(int dev_no)
{
	return dev_no;
}

void board_late_mmc_env_init(void)
{
	char cmd[32];
	char mmcblk[32];
	u32 dev_no = mmc_get_env_dev();

	if (!check_mmc_autodetect())
		return;

	env_set_ulong("mmcdev", dev_no);

	/* Set mmcblk env */
	sprintf(mmcblk, "/dev/mmcblk%dp2 rootwait rw",
		mmc_map_to_kernel_blk(dev_no));
	env_set("mmcroot", mmcblk);

	sprintf(cmd, "mmc dev %d", dev_no);
	run_command(cmd, 0);
}

#define FT5336_TOUCH_I2C_BUS 2
#define FT5336_TOUCH_I2C_ADDR 0x38
#define ADV7535_HDMI_I2C_ADDR 0x3d
#define PCA9555_23_I2C_ADDR 0x23
#define PCA9555_26_I2C_ADDR 0x26
#define EXPANSION_IC_I2C_BUS 2

int detect_baseboard(void)
{
	struct udevice *bus = NULL;
	struct udevice *i2c_dev = NULL;
	int ret;
	char *fdtfile, *baseboard, str_fdtfile[64];

	fdtfile = env_get("fdtfile");
	if (fdtfile && !strcmp(fdtfile, "undefined")) {
		ret = uclass_get_device_by_seq(UCLASS_I2C, EXPANSION_IC_I2C_BUS, &bus);
		if (ret) {
			printf("%s: Can't find bus\n", __func__);
			return -EINVAL;
		}

		baseboard = env_get("baseboard");
		if (!dm_i2c_probe(bus, PCA9555_23_I2C_ADDR, 0, &i2c_dev) && \
		!dm_i2c_probe(bus, PCA9555_26_I2C_ADDR, 0, &i2c_dev) )
			env_set("baseboard", "wizard");
		else
			env_set("baseboard", "pi");
		baseboard = env_get("baseboard");

		strcpy(str_fdtfile, "imx8mq-pico-");
		strcat(str_fdtfile, baseboard);
		strcat(str_fdtfile, ".dtb");
		env_set("fdtfile", str_fdtfile);
	}
	return 0;

}

int add_dtoverlay(char *ov_name)
{
	char *dtoverlay, arr_dtov[64];

	dtoverlay = env_get("dtoverlay");
	if (dtoverlay) {
		strcpy(arr_dtov, dtoverlay);
		if (!strstr(arr_dtov, ov_name)) {
			strcat(arr_dtov, " ");
			strcat(arr_dtov, ov_name);
			env_set("dtoverlay", arr_dtov);
		}
	} else
		env_set("dtoverlay", ov_name);

	return 0;
}

int detect_display_panel(void)
{
	struct udevice *bus = NULL;
	struct udevice *i2c_dev = NULL;
	int ret, touch_id;

	ret = uclass_get_device_by_seq(UCLASS_I2C, FT5336_TOUCH_I2C_BUS, &bus);
	if (ret) {
		printf("%s: Can't find bus\n", __func__);
		return -EINVAL;
	}
	/* detect different MIPI panel by touch controller */
	ret = dm_i2c_probe(bus, FT5336_TOUCH_I2C_ADDR, 0, &i2c_dev);
	if (! ret) {
		touch_id = dm_i2c_reg_read(i2c_dev, 0xA3);
		switch (touch_id) {
		case 0x54:
			add_dtoverlay("ili9881c");
			break;
		case 0x58:
			add_dtoverlay("g080uan01");
			break;
		case 0x59:
			add_dtoverlay("g101uan02");
			break;
		default:
			printf("Unknown panel ID!\r\n");
		}
	}

	/* detect MIPI2HDMI controller */
	ret = dm_i2c_probe(bus, ADV7535_HDMI_I2C_ADDR, 0, &i2c_dev);
	if (! ret) {
		add_dtoverlay("mipi2hdmi-adv7535");
	}

	return 0;
}

struct camera_cfg {
       u8 camera_index;
       u8 i2c_bus_index;
       u8 eeprom_i2c_addr;
};

const struct camera_cfg tevi_camera[] = {
       {1, 1, 0x54},
       {2, 2, 0x54},
};

#define NUMS(x)        (sizeof(x) / sizeof(x[0]))

int detect_tevi_camera(void)
{
	struct udevice *bus = NULL;
	struct udevice *i2c_dev = NULL;
	int i, ret;

	for (i = 0; i < NUMS(tevi_camera); i++) {
	        ret = uclass_get_device_by_seq(UCLASS_I2C, tevi_camera[i].i2c_bus_index, &bus);
	        if (ret) {
	                printf("%s: Can't find bus\n", __func__);
	                continue;
	        }
	        ret = dm_i2c_probe(bus, tevi_camera[i].eeprom_i2c_addr, 0, &i2c_dev);
	        if (! ret) {
	                add_dtoverlay("tevi-ov5640");
	                return 0;
	        } else {
	                /* ov7251 chip address is 0x60 */
	                ret = dm_i2c_probe(bus, 0x60, 0, &i2c_dev);
	                if (! ret) {
	                        add_dtoverlay("ov7251");
	                        return 0;
	                } else {

                                /* ov5645 chip address is 0x3c */
	                        ret = dm_i2c_probe(bus, 0x3c, 0, &i2c_dev);
	                        if (! ret) {
	                                add_dtoverlay("ov5645");
	                                return 0;
	                        }
	                }
	        }
	}
	return 0;
}

#ifdef CONFIG_OF_BOARD_SETUP
int ft_board_setup(void *blob, struct bd_info *bd)
{
	const int *cell;
	int offs;
	uint32_t cma_size;
	char *cmasize;

	offs = fdt_path_offset(blob, "/reserved-memory/linux,cma");
	cell = fdt_getprop(blob, offs, "size", NULL);
	cma_size = fdt32_to_cpu(cell[1]);
	cmasize = env_get("cma_size");
	if(cmasize || ((u64)(gd->ram_size >> 1) < cma_size)) {
		/* CMA is aligned by 32MB on i.mx8mq,
		   so CMA size can only be multiple of 32MB */
		cma_size = env_get_ulong("cma_size", 10, (18 * 32) * 1024 * 1024);
		fdt_setprop_u64(blob, offs, "size", (uint64_t)cma_size);
	}

	return 0;
}
#endif

int board_late_init(void)
{
#ifndef CONFIG_AVB_SUPPORT
	detect_baseboard();
	detect_display_panel();
	detect_tevi_camera();
#endif

#ifdef CONFIG_ENV_IS_IN_MMC
	board_late_mmc_env_init();
#endif

#ifdef CONFIG_ENV_VARS_UBOOT_RUNTIME_CONFIG
	env_set("board_name", "PICO");
	env_set("board_rev", "iMX8MQ");
#endif

	return 0;
}

#ifdef CONFIG_FSL_FASTBOOT
#ifdef CONFIG_ANDROID_RECOVERY
int is_recovery_key_pressing(void)
{
	return 0; /*TODO*/
}
#endif /*CONFIG_ANDROID_RECOVERY*/
#endif /*CONFIG_FSL_FASTBOOT*/
