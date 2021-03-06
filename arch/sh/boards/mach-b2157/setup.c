/*
 * arch/sh/boards/mach-b2157/setup.c
 *
 * Copyright (C) 2012 STMicroelectronics Limited
 * Author: Patrice Chotard (patrice.chotard@st.com)
 *
 * May be copied or modified under the terms of the GNU General Public
 * License.  See linux/COPYING for more information.
 */

#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/phy.h>
#include <linux/gpio.h>
#include <linux/gpio_keys.h>
#include <linux/leds.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/nand.h>
#include <linux/spi/spi.h>
#include <linux/spi/flash.h>
#include <linux/stm/platform.h>
#include <linux/stm/stxh205.h>
#include <linux/stm/sysconf.h>
#include <asm/irq-ilc.h>

#define B2157_GPIO_FLASH_WP		stm_gpio(6, 2)
#define B2157_GPIO_POWER_ON_ETH		stm_gpio(3, 3)
#define B2157_POWER_ON			stm_gpio(3, 7)

static void __init b2157_setup(char **cmdline_p)
{
	printk(KERN_INFO "STMicroelectronics B2157 board initialisation\n");

	stxh205_early_device_init();

	/*
	 * UART_SW: On board DB9 connector JK5
	 */
	stxh205_configure_asc(STXH205_ASC(10), &(struct stxh205_asc_config) {
			.hw_flow_control = 0,
			.is_console = 1, });

	/*
	 * Header JB4
	 */
	stxh205_configure_asc(STXH205_ASC(1), &(struct stxh205_asc_config) {
			.hw_flow_control = 0,
		});
}

static struct platform_device b2157_leds = {
	.name = "leds-gpio",
	.id = -1,
	.dev.platform_data = &(struct gpio_led_platform_data) {
		.num_leds = 1,
		.leds = (struct gpio_led[]) {
			{
				.name = "GREEN",
				.default_trigger = "heartbeat",
				.gpio = stm_gpio(15, 7),
			},
		},
	},
};

/* NAND Flash */
static struct stm_nand_bank_data b2157_nand_flash = {
	.csn		= 0,	/* EMI_CSA hardwired to NAND_CS */
	.options	= NAND_NO_AUTOINCR,
	.bbt_options	= NAND_BBT_USE_FLASH,
	.nr_partitions	= 2,
	.partitions	= (struct mtd_partition []) {
		{
			.name	= "NAND Flash 1",
			.offset	= 0,
			.size	= 0x00800000
		}, {
			.name	= "NAND Flash 2",
			.offset = MTDPART_OFS_NXTBLK,
			.size	= MTDPART_SIZ_FULL
		},
	},
	.timing_data	=  &(struct stm_nand_timing_data) {
		.sig_setup	= 50,		/* times in ns */
		.sig_hold	= 50,
		.CE_deassert	= 0,
		.WE_to_RBn	= 100,
		.wr_on		= 10,
		.wr_off		= 40,
		.rd_on		= 10,
		.rd_off		= 40,
		.chip_delay	= 30,		/* in us */
	},
};

static int b2157_phy_reset(void *bus)
{
	/*
	 * IC+ IP101 datasheet specifies 10mS low period and device usable
	 * 2.5mS after rising edge. However experimentally it appear
	 * 10mS is required for reliable functioning.
	 */
	gpio_set_value(B2157_GPIO_POWER_ON_ETH, 0);
	mdelay(10);
	gpio_set_value(B2157_GPIO_POWER_ON_ETH, 1);
	mdelay(10);

	return 1;
}

static struct stmmac_mdio_bus_data stmmac_mdio_bus = {
	.phy_reset = b2157_phy_reset,
	.phy_mask = 0,
	.probed_phy_irq = ILC_IRQ(25), /* MDINT */
};

static struct platform_device *b2157_devices[] __initdata = {
	&b2157_leds,
};

static int __init device_init(void)
{
	/* The "POWER_ON_ETH" line should be rather called "PHY_RESET",
	 * but it isn't... ;-) */
	gpio_request(B2157_GPIO_POWER_ON_ETH, "POWER_ON_ETH");
	gpio_direction_output(B2157_GPIO_POWER_ON_ETH, 0);

	gpio_request(B2157_POWER_ON, "POWER_ON");
	gpio_direction_output(B2157_POWER_ON, 0);

	stxh205_configure_ethernet(&(struct stxh205_ethernet_config) {
			.interface = PHY_INTERFACE_MODE_MII,
			.ext_clk = 1,
			.phy_bus = 0,
			.phy_addr = -1,
			.mdio_bus_data = &stmmac_mdio_bus,
		});

	/* PHY IRQ has to be triggered LOW */
	irq_set_irq_type(ILC_IRQ(25), IRQ_TYPE_LEVEL_LOW);

	stxh205_configure_usb(0);

	stxh205_configure_usb(1);

	/*
	 * SSC1: FE/DEMO
	 * U17: LNBH25PQR
	 */
	stxh205_configure_ssc_i2c(STXH205_SSC(1), &(struct stxh205_ssc_config) {
			.routing.ssc1.sclk = stxh205_ssc1_sclk_pio12_0,
			.routing.ssc1.mtsr = stxh205_ssc1_mtsr_pio12_1, });
	/*
	 * SSC3: SYS
	 * UQ1: STV6440, UK2: M24256 (EEPROM), JQ3
	 * Also JK2 (front panel) on rev A only
	 */
	stxh205_configure_ssc_i2c(STXH205_SSC(3), &(struct stxh205_ssc_config) {
			.routing.ssc3.sclk = stxh205_ssc3_sclk_pio15_5,
			.routing.ssc3.mtsr = stxh205_ssc3_mtsr_pio15_6, });
	/*
	 * SSC11: HDMI
	 * UG1: HDMI2C1 -> HDMI
	 */
	stxh205_configure_ssc_i2c(STXH205_SSC(11), NULL);

	stxh205_configure_lirc(&(struct stxh205_lirc_config) {
#ifdef CONFIG_LIRC_STM_UHF
			.rx_mode = stxh205_lirc_rx_mode_uhf, });
#else
			.rx_mode = stxh205_lirc_rx_mode_ir, });
#endif

	stxh205_configure_pwm(&(struct stxh205_pwm_config) {
			/*
			 * PWM10 is connected to 12V->1.2V power supply
			 * for "debug purposes". Enable at your own risk!
			 */
			.out_channel_config[0].enabled = 0,
		});

	/*
	 * NAND MTD has no concept of write-protect, so permanently disable WP
	 */
	gpio_request(B2157_GPIO_FLASH_WP, "FLASH_WP");
	gpio_direction_output(B2157_GPIO_FLASH_WP, 1);

	stxh205_configure_nand(&(struct stm_nand_config) {
			.driver = stm_nand_bch,
			.nr_banks = 1,
			.banks = &b2157_nand_flash,
			.rbn.flex_connected = 1,
			.bch_ecc_cfg = BCH_ECC_CFG_AUTO});

	return platform_add_devices(b2157_devices,
			ARRAY_SIZE(b2157_devices));
}
arch_initcall(device_init);

struct sh_machine_vector mv_b2157 __initmv = {
	.mv_name = "b2157",
	.mv_setup = b2157_setup,
	.mv_nr_irqs = NR_IRQS,
};

#if defined(CONFIG_HIBERNATION_ON_MEMORY)

#include "../../kernel/cpu/sh4/stm_hom.h"

static int b2157_board_freeze(struct stm_wakeup_devices *wkd)
{
	if (!wkd->stm_mac0_can_wakeup)
		gpio_set_value(B2157_GPIO_POWER_ON_ETH, 0);
	return 0;
}

static int b2157_board_defrost(struct stm_wakeup_devices *wkd)
{
	b2157_phy_reset(NULL);
	return 0;
}

static struct stm_hom_board b2157_hom = {
	.freeze = b2157_board_freeze,
	.restore = b2157_board_defrost,
};

static int __init b2157_hom_register(void)
{
	return stm_hom_board_register(&b2157_hom);
}

module_init(b2157_hom_register);
#endif
