/*
 * (c) 2010 STMicroelectronics Limited
 *
 * Author: Pawel Moll <pawel.moll@st.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */



#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/ata_platform.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/partitions.h>
#include <linux/stm/emi.h>
#include <linux/stm/device.h>
#include <linux/stm/sysconf.h>
#include <linux/stm/stx7108.h>
#include <asm/irq-ilc.h>
#include "pio-control.h"

/* EMI resources ---------------------------------------------------------- */

static int __initdata stx7108_emi_bank_configured[EMI_BANKS];

#if defined(CONFIG_SUSPEND) && defined(CONFIG_HIBERNATION_ON_MEMORY)
/*
 * Don't provide power management of the EMI if both suspend and HoM
 * are configured.  If we do, it appears to be impossible to perform a
 * suspend following a HoM hibernate/resume cycle.
 */

static int stx7108_emi_init(struct stm_device_state *device_state)
{
	stm_device_sysconf_write(device_state, "EMI_PWR", 0);
	return 0;
}
#define stx7108_emi_power NULL
#else
#define stx7108_emi_init NULL

static void stx7108_emi_power(struct stm_device_state *device_state,
		enum stm_device_power_state power)
{
	int i;
	int value = (power == stm_device_power_on) ? 0 : 1;

	stm_device_sysconf_write(device_state, "EMI_PWR", value);
	for (i = 5; i; --i) {
		if (stm_device_sysconf_read(device_state, "EMI_ACK")
			== value)
			break;
		mdelay(10);
	}

	return;
}
#endif

static struct platform_device stx7108_emi = {
	.name = "emi",
	.id = -1,
	.num_resources = 3,
	.resource = (struct resource[]) {
		STM_PLAT_RESOURCE_MEM_NAMED("emi memory", 0, 256 * 1024 * 1024),
		STM_PLAT_RESOURCE_MEM_NAMED("emi4 config", 0xfe900000, 0x874),
		STM_PLAT_RESOURCE_MEM_NAMED("emiss config", 0xfdaa9000, 0x80),
	},
	.dev.platform_data = &(struct stm_device_config){
		.sysconfs_num = 2,
		.sysconfs = (struct stm_device_sysconf []){
			STM_DEVICE_SYS_CFG_BANK(2, 30, 0, 0, "EMI_PWR"),
			STM_DEVICE_SYS_STA_BANK(2, 1, 0, 0, "EMI_ACK"),
		},
		.init = stx7108_emi_init,
		.power = stx7108_emi_power,
	}
};

/* PATA resources --------------------------------------------------------- */

/* EMI A20 = CS1 (active low)
 * EMI A21 = CS0 (active low)
 * EMI A19 = DA2
 * EMI A18 = DA1
 * EMI A17 = DA0 */
static struct resource stx7108_pata_resources[] = {
	/* I/O base: CS1=N, CS0=A */
	[0] = STM_PLAT_RESOURCE_MEM(1 << 20, 8 << 17),
	/* CTL base: CS1=A, CS0=N, DA2=A, DA1=A, DA0=N */
	[1] = STM_PLAT_RESOURCE_MEM((1 << 21) + (6 << 17), 4),
	/* IRQ */
	[2] = STM_PLAT_RESOURCE_IRQ(-1, -1),
};

static struct platform_device stx7108_pata_device = {
	.name		= "pata_platform",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(stx7108_pata_resources),
	.resource	= stx7108_pata_resources,
	.dev.platform_data = &(struct pata_platform_info) {
		.ioport_shift = 17,
	},
};

void __init stx7108_configure_pata(struct stx7108_pata_config *config)
{
	unsigned long bank_base;

	if (!config) {
		BUG();
		return;
	}

	BUG_ON(config->emi_bank < 0 || config->emi_bank >= EMI_BANKS);
	BUG_ON(stx7108_emi_bank_configured[config->emi_bank]);
	stx7108_emi_bank_configured[config->emi_bank] = 1;

	bank_base = emi_bank_base(config->emi_bank);

	stx7108_pata_resources[0].start += bank_base;
	stx7108_pata_resources[0].end += bank_base;
	stx7108_pata_resources[1].start += bank_base;
	stx7108_pata_resources[1].end += bank_base;
	stx7108_pata_resources[2].start = config->irq;
	stx7108_pata_resources[2].end = config->irq;

	emi_config_pata(config->emi_bank, config->pc_mode);

	platform_device_register(&stx7108_pata_device);
}

/* SPI FSM setup ---------------------------------------------------------- */

static struct stm_pad_config stx7108_spifsm_pad_config = {
	.gpios_num = 6,
	.gpios = (struct stm_pad_gpio[]) {
		STM_PAD_PIO_OUT_NAMED(1, 6, 1, "spi-fsm-clk"),
		STM_PAD_PIO_OUT_NAMED(1, 7, 1, "spi-fsm-cs"),
		/* To support QUAD mode operations, each of the following pads
		 * may be used by the IP as an input or an output.  Here we
		 * specify either PIO_OUT or PIO_IN, which sets pu = 0 && od =
		 * 0. 'oe' is taken from a signal generated by the SPI-FSM IP
		 * itself.
		 */
		STM_PAD_PIO_OUT_NAMED(2, 0, 1, "spi-fsm-mosi"),
		STM_PAD_PIO_IN_NAMED(2, 1, 1, "spi-fsm-miso"),
		STM_PAD_PIO_OUT_NAMED(2, 2, 1, "spi-fsm-hold"),
		STM_PAD_PIO_OUT_NAMED(2, 3, 1, "spi-fsm-wp"),
	}
};

static struct platform_device stx7108_spifsm_device = {
	.name		= "stm-spi-fsm",
	.id		= 0,
	.num_resources	= 1,
	.resource	= (struct resource[]) {
		{
			.start	= 0xfe902000,
			.end	= 0xfe9024ff,
			.flags	= IORESOURCE_MEM,
		},
	},
};

void __init stx7108_configure_spifsm(struct stm_plat_spifsm_data *data)
{
	struct sysconf_field *sc;

	/* SPI FSM Controller not functional on stx7108 cut 1.0 */
	BUG_ON(cpu_data->cut_major == 1);

	stx7108_spifsm_device.dev.platform_data = data;

	sc = sysconf_claim(SYS_STA_BANK1, 3, 2, 6, "mode-pins");

	data->pads = &stx7108_spifsm_pad_config;

	/* SoC/IP Capabilities */
	data->capabilities.no_read_repeat = 1;
	data->capabilities.no_write_repeat = 1;
	data->capabilities.read_status_bug = spifsm_read_status_clkdiv4;
	data->capabilities.no_poll_mode_change = 1;
	data->capabilities.boot_from_spi = (sysconf_read(sc) == 0x1a) ? 1 : 0;

	sysconf_release(sc);
	platform_device_register(&stx7108_spifsm_device);
}


/* NAND Resources --------------------------------------------------------- */

static struct platform_device stx7108_nand_emi_device = {
	.name			= "stm-nand-emi",
	.dev.platform_data	= &(struct stm_plat_nand_emi_data) {
	},
};

static struct stm_plat_nand_flex_data stx7108_nand_flex_data;
static struct stm_plat_nand_bch_data stx7108_nand_bch_data;

static struct platform_device stx7108_nandi_device = {
	.id			= -1,
	.num_resources		= 3,
	.resource		= (struct resource[]) {
		STM_PLAT_RESOURCE_MEM_NAMED("nand_mem", 0xFE901000, 0x1000),
		STM_PLAT_RESOURCE_MEM_NAMED("nand_dma", 0xFDAA8800, 0x0800),
		STM_PLAT_RESOURCE_IRQ_NAMED("nand_irq", ILC_IRQ(121), -1),
	},
};

void __init stx7108_configure_nand(struct stm_nand_config *config)
{
	struct stm_plat_nand_emi_data *emi_data;

	switch (config->driver) {
	case stm_nand_emi:
		/* Configure device for stm-nand-emi driver */
		emi_data = stx7108_nand_emi_device.dev.platform_data;
		emi_data->nr_banks = config->nr_banks;
		emi_data->banks = config->banks;
		emi_data->emi_rbn_gpio = config->rbn.emi_gpio;
		platform_device_register(&stx7108_nand_emi_device);
		break;
	case stm_nand_flex:
	case stm_nand_afm:
		/* Configure device for stm-nand-flex/afm driver */
		emiss_nandi_select(STM_NANDI_HAMMING);
		stx7108_nand_flex_data.nr_banks = config->nr_banks;
		stx7108_nand_flex_data.banks = config->banks;
		stx7108_nand_flex_data.flex_rbn_connected =
			config->rbn.flex_connected;
		stx7108_nandi_device.dev.platform_data =
			&stx7108_nand_flex_data;
		stx7108_nandi_device.name =
			(config->driver == stm_nand_flex) ?
			"stm-nand-flex" : "stm-nand-afm";
		platform_device_register(&stx7108_nandi_device);
		break;
	case stm_nand_bch:
		/* Configure device for stm-nand-bch driver */
		BUG_ON(cpu_data->cut_major < 2);
		BUG_ON(config->nr_banks > 1);
		stx7108_nand_bch_data.bank = config->banks;
		stx7108_nand_bch_data.bch_ecc_cfg = config->bch_ecc_cfg;
		stx7108_nand_bch_data.bch_bitflip_threshold =
			config->bch_bitflip_threshold;
		stx7108_nandi_device.dev.platform_data =
			&stx7108_nand_bch_data;
		stx7108_nandi_device.name = "stm-nand-bch";
		platform_device_register(&stx7108_nandi_device);
		break;
	default:
		BUG();
		return;
	}
}


/* FDMA resources --------------------------------------------------------- */

static struct stm_plat_fdma_fw_regs stm_fdma_firmware_7108 = {
	.rev_id    = 0x8000 + (0x000 << 2), /* 0x8000 */
	.cmd_statn = 0x8000 + (0x450 << 2), /* 0x9140 */
	.req_ctln  = 0x8000 + (0x460 << 2), /* 0x9180 */
	.ptrn      = 0x8000 + (0x560 << 2), /* 0x9580 */
	.ctrln     = 0x8000 + (0x561 << 2), /* 0x9584 */
	.cntn      = 0x8000 + (0x562 << 2), /* 0x9588 */
	.saddrn    = 0x8000 + (0x563 << 2), /* 0x958c */
	.daddrn    = 0x8000 + (0x564 << 2), /* 0x9590 */
};

static struct stm_plat_fdma_hw stx7108_fdma_hw = {
	.slim_regs = {
		.id       = 0x0000 + (0x000 << 2), /* 0x0000 */
		.ver      = 0x0000 + (0x001 << 2), /* 0x0004 */
		.en       = 0x0000 + (0x002 << 2), /* 0x0008 */
		.clk_gate = 0x0000 + (0x003 << 2), /* 0x000c */
		.slim_pc  = 0x0000 + (0x008 << 2), /* 0x0020 */
	},
	.dmem = {
		.offset = 0x8000,
		.size   = 0x800 << 2, /* 2048 * 4 = 8192 */
	},
	.periph_regs = {
		.sync_reg = 0x8000 + (0xfe2 << 2), /* 0xbf88 */
		.cmd_sta  = 0x8000 + (0xff0 << 2), /* 0xbfc0 */
		.cmd_set  = 0x8000 + (0xff1 << 2), /* 0xbfc4 */
		.cmd_clr  = 0x8000 + (0xff2 << 2), /* 0xbfc8 */
		.cmd_mask = 0x8000 + (0xff3 << 2), /* 0xbfcc */
		.int_sta  = 0x8000 + (0xff4 << 2), /* 0xbfd0 */
		.int_set  = 0x8000 + (0xff5 << 2), /* 0xbfd4 */
		.int_clr  = 0x8000 + (0xff6 << 2), /* 0xbfd8 */
		.int_mask = 0x8000 + (0xff7 << 2), /* 0xbfdc */
	},
	.imem = {
		.offset = 0xc000,
		.size   = 0x1000 << 2, /* 4096 * 4 = 16384 */
	},
};

static struct stm_plat_fdma_data stx7108_fdma_platform_data = {
	.hw = &stx7108_fdma_hw,
	.fw = &stm_fdma_firmware_7108,
};

static struct platform_device stx7108_fdma_devices[] = {
	{
		.name = "stm-fdma",
		.id = 0,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfda00000, 0x10000),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(27), -1),
		},
		.dev.platform_data = &stx7108_fdma_platform_data,
	}, {
		.name = "stm-fdma",
		.id = 1,
		.num_resources = 2,
		.resource = (struct resource[2]) {
			STM_PLAT_RESOURCE_MEM(0xfda10000, 0x10000),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(29), -1),
		},
		.dev.platform_data = &stx7108_fdma_platform_data,
	}, {
		.name = "stm-fdma",
		.id = 2,
		.num_resources = 2,
		.resource = (struct resource[2]) {
			STM_PLAT_RESOURCE_MEM(0xfda20000, 0x10000),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(31), -1),
		},
		.dev.platform_data = &stx7108_fdma_platform_data,
	}
};

static struct platform_device stx7108_fdma_xbar_device = {
	.name = "stm-fdma-xbar",
	.id = -1,
	.num_resources = 1,
	.resource = (struct resource[]) {
		STM_PLAT_RESOURCE_MEM(0xfdabb000, 0x1000),
	},
};


/* st231 coprocessor resources -------------------------------------------- */

static struct platform_device stx7108_st231_coprocessor_devices[3] = {
	{
		.name = "stm-coproc-st200",
		.id = 0,
		.dev.platform_data = &(struct plat_stm_st231_coproc_data) {
			.name = "video",
			.id = 0,
			.device_config = &(struct stm_device_config) {
				.sysconfs_num = 2,
				.sysconfs = (struct stm_device_sysconf []) {
					STM_DEVICE_SYS_CFG_BANK(0, 6,
							6, 31, "BOOT_ADDR"),
					STM_DEVICE_SYS_CFG_BANK(0, 0,
							4, 4, "RESET"),
				},
			},
			.boot_shift = 6,
			.not_reset = 1,
		},
	}, {
		.name = "stm-coproc-st200",
		.id = 1,
		.dev.platform_data = &(struct plat_stm_st231_coproc_data) {
			.name = "audio",
			.id = 0,
			.device_config = &(struct stm_device_config) {
				.sysconfs_num = 2,
				.sysconfs = (struct stm_device_sysconf []) {
					STM_DEVICE_SYS_CFG_BANK(0, 7,
							6, 31, "BOOT_ADDR"),
					STM_DEVICE_SYS_CFG_BANK(0, 0,
							5, 5, "RESET"),
				},
			},
			.boot_shift = 6,
			.not_reset = 1,
		},
	}, {
		.name = "stm-coproc-st200",
		.id = 2,
		.dev.platform_data = &(struct plat_stm_st231_coproc_data) {
			.name = "gp",
			.id = 0,
			.device_config = &(struct stm_device_config) {
				.sysconfs_num = 2,
				.sysconfs = (struct stm_device_sysconf []) {
					STM_DEVICE_SYS_CFG_BANK(0, 8,
							6, 31, "BOOT_ADDR"),
					STM_DEVICE_SYS_CFG_BANK(0, 0,
							6, 6, "RESET"),
				},
			},
			.boot_shift = 6,
			.not_reset = 1,
		},
	},
};


/* NOTE: 7108C2 Rev C datasheet seems to have incorrect bank 0-sysconf5
 * register description, Refer to
 * SPIRIT_registers_of_all_banks_sysconf.pdf for correct settings */

#define ST40_BOOT_ADDR_SHIFT		1

static int stx7108_st40_rt_boot(struct stm_device_state *dev_state,
					unsigned long boot_addr,
					unsigned long phy_addr)
{

	stm_device_sysconf_write(dev_state, "MASK_RESET", 1);
	stm_device_sysconf_write(dev_state, "RESET" , 0);
	stm_device_sysconf_write(dev_state, "BOOT_ADDR",
					 boot_addr >> ST40_BOOT_ADDR_SHIFT);

	if (boot_cpu_data.cut_major == 2) { /* bart always enabled */
		stm_device_sysconf_write(dev_state, "LMI0_NOT_LMI1",
					(phy_addr & 0x40000000) ? 1 : 0);
		stm_device_sysconf_write(dev_state, "LMI_SYS_BASE",
					((phy_addr & 0x3C000000) >> 26));
	} else {
		stm_device_sysconf_write(dev_state, "BART_ENABLE", 1);
		stm_device_sysconf_write(dev_state, "LMI_SYS_BASE",
					((phy_addr & 0xFC000000) >> 26));
	}

	stm_device_sysconf_write(dev_state, "RESET", 1);
	return 0;
}

static struct plat_stm_st40_coproc_data stx7108_cut1_st40_coproc_dev_data = {
	.name = "rt",
	.id = 0,
	.cpu_boot = stx7108_st40_rt_boot,
	.device_config = &(struct stm_device_config) {
		.sysconfs_num = 5,
		.sysconfs = (struct stm_device_sysconf []){
			STM_DEVICE_SYS_CFG_BANK(0, 5, 1, 28, "BOOT_ADDR"),
			STM_DEVICE_SYS_CFG_BANK(0, 1, 1, 1, "MASK_RESET"),
			STM_DEVICE_SYS_CFG_BANK(0, 0, 1, 1, "RESET"),
			STM_DEVICE_SYS_CFG_BANK(1, 0, 1, 1, "BART_ENABLE"),
			STM_DEVICE_SYS_CFG_BANK(1, 1, 8, 13, "LMI_SYS_BASE"),
		},
	},
};

static struct plat_stm_st40_coproc_data stx7108_cut2_st40_coproc_dev_data = {
	.name = "rt",
	.id = 0,
	.cpu_boot = stx7108_st40_rt_boot,
	.device_config = &(struct stm_device_config) {
		.sysconfs_num = 7,
		.sysconfs = (struct stm_device_sysconf []){
			STM_DEVICE_SYS_CFG_BANK(0, 5, 1, 28, "BOOT_ADDR"),
			STM_DEVICE_SYS_CFG_BANK(0, 1, 1, 1, "MASK_RESET"),
			STM_DEVICE_SYS_CFG_BANK(0, 0, 1, 1, "RESET"),
			STM_DEVICE_SYS_CFG_BANK(1, 0, 2, 5, "LMI_SYS_BASE"),
			STM_DEVICE_SYS_CFG_BANK(1, 0, 8, 8, "LMI0_NOT_LMI1"),
			STM_DEVICE_SYS_STA_BANK(1, 13, 0, 31, "BART_STATUS"),
			STM_DEVICE_SYS_CFG_BANK(1, 0, 16, 16,
							"BART_LOCK_ENABLE"),
		},
	},
};

static struct platform_device stx7108_st40_coproc_device = {
	.name = "stm-coproc-st40",
	.id = 0,
	.dev.platform_data  = &stx7108_cut2_st40_coproc_dev_data,
};

/* Hardware RNG resources ------------------------------------------------- */

static struct platform_device stx7108_devhwrandom_device = {
	.name		= "stm-hwrandom",
	.id		= -1,
	.num_resources	= 1,
	.resource	= (struct resource []) {
		STM_PLAT_RESOURCE_MEM(0xfdabd000, 0x1000),
	}
};

/* PIO ports resources ---------------------------------------------------- */

static struct platform_device stx7108_pio_devices[] = {
	[0] = {
		.name = "stm-gpio",
		.id = 0,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfd720000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(129), -1),
		},
	},
	[1] = {
		.name = "stm-gpio",
		.id = 1,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfd721000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(130), -1),
		},
	},
	[2] = {
		.name = "stm-gpio",
		.id = 2,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfd722000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(131), -1),
		},
	},
	[3] = {
		.name = "stm-gpio",
		.id = 3,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfd723000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(132), -1),
		},
	},
	[4] = {
		.name = "stm-gpio",
		.id = 4,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfd724000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(133), -1),
		},
	},
	[5] = {
		.name = "stm-gpio",
		.id = 5,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfd725000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(134), -1),
		},
	},
	[6] = {
		.name = "stm-gpio",
		.id = 6,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfd726000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(135), -1),
		},
	},
	[7] = {
		.name = "stm-gpio",
		.id = 7,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfd727000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(136), -1),
		},
	},
	[8] = {
		.name = "stm-gpio",
		.id = 8,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfd728000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(137), -1),
		},
	},
	[9] = {
		.name = "stm-gpio",
		.id = 9,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfd729000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(138), -1),
		},
	},

	[10] = {
		.name = "stm-gpio",
		.id = 10,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfda60000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(151), -1),
		},
	},
	[11] = {
		.name = "stm-gpio",
		.id = 11,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfda61000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(152), -1),
		},
	},
	[12] = {
		.name = "stm-gpio",
		.id = 12,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfda62000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(153), -1),
		},
	},
	[13] = {
		.name = "stm-gpio",
		.id = 13,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfda63000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(154), -1),
		},
	},
	[14] = {
		.name = "stm-gpio",
		.id = 14,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfda64000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(155), -1),
		},
	},

	[15] = {
		.name = "stm-gpio",
		.id = 15,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfe740000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(139), -1),
		},
	},
	[16] = {
		.name = "stm-gpio",
		.id = 16,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfe741000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(140), -1),
		},
	},
	[17] = {
		.name = "stm-gpio",
		.id = 17,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfe742000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(141), -1),
		},
	},
	[18] = {
		.name = "stm-gpio",
		.id = 18,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfe743000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(142), -1),
		},
	},
	[19] = {
		.name = "stm-gpio",
		.id = 19,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfe744000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(143), -1),
		},
	},
	[20] = {
		.name = "stm-gpio",
		.id = 20,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfe745000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(144), -1),
		},
	},
	[21] = {
		.name = "stm-gpio",
		.id = 21,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfe746000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(145), -1),
		},
	},
	[22] = {
		.name = "stm-gpio",
		.id = 22,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfe747000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(146), -1),
		},
	},
	[23] = {
		.name = "stm-gpio",
		.id = 23,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfe748000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(147), -1),
		},
	},
	[24] = {
		.name = "stm-gpio",
		.id = 24,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfe749000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(148), -1),
		},
	},

	[25] = {
		.name = "stm-gpio",
		.id = 25,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfe720000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(149), -1),
		},
	},

	[26] = {
		.name = "stm-gpio",
		.id = 26,
		.num_resources = 2,
		.resource = (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfe721000, 0x100),
			STM_PLAT_RESOURCE_IRQ(ILC_IRQ(150), -1),
		},
	},
};

static const struct stm_pio_control_retime_offset stx7108_pio_retime_offset = {
	.clk1notclk0_offset	= 0,
	.clknotdata_offset	= 1,
	.delay_lsb_offset	= 2,
	.double_edge_offset	= 3,
	.invertclk_offset	= 4,
	.retime_offset		= 5,
	.delay_msb_offset	= 6,
};

static unsigned int stx7108_pio_control_delays_in[] = {
	0,	/* 00: 0.0ns */
	500,	/* 01: 0.5ns */
	1000,	/* 10: 1.0ns */
	1500,	/* 11: 1.5ns */
};

static unsigned int stx7108_pio_control_delays_out[] = {
	0,	/* 00: 0.0ns */
	1000,	/* 01: 1.0ns */
	2000,	/* 10: 2.0ns */
	3000,	/* 11: 3.0ns */
};

static const struct stm_pio_control_retime_params stx7108_retime_params = {
	.retime_offset = &stx7108_pio_retime_offset,
	.delay_times_in = stx7108_pio_control_delays_in,
	.num_delay_times_in = ARRAY_SIZE(stx7108_pio_control_delays_in),
	.delay_times_out = stx7108_pio_control_delays_out,
	.num_delay_times_out = ARRAY_SIZE(stx7108_pio_control_delays_out),
};

#define STX7108_PIO_CONTROL_(_num, _group, _alt_num,			\
		_oe_num, _pu_num, _od_num, _lsb, _msb,			\
		_style, _rt1_num, _rt2_num)				\
	[_num] = {							\
		.alt = { _group, _alt_num },				\
		.oe = { _group, _oe_num, _lsb, _msb },			\
		.pu = { _group, _pu_num, _lsb, _msb },			\
		.od = { _group, _od_num, _lsb, _msb },			\
		.retime_style = _style,					\
		.retime_pin_mask = 0xff,				\
		.retime_params = &stx7108_retime_params,		\
		.retiming = {						\
			{ _group, _rt1_num },				\
			{ _group, _rt2_num }				\
		},							\
	}

#define STX7108_PIO_CONTROL(_num, _group, _alt_num,			\
		_oe_num, _pu_num, _od_num, _lsb, _msb,			\
		_rt)				\
	STX7108_PIO_CONTROL_(_num, _group, _alt_num,			\
		_oe_num, _pu_num, _od_num, _lsb, _msb,			\
		_rt)

#define STX7108_PIO_CONTROL4(_num, _group, _alt_num,		\
		_oe_num, _pu_num, _od_num,			\
		_rt1, _rt2, _rt3, _rt4)				\
	STX7108_PIO_CONTROL_(_num,   _group, _alt_num,		\
		_oe_num, _pu_num, _od_num,  0,  7,		\
		_rt1),						\
	STX7108_PIO_CONTROL_(_num+1, _group, _alt_num+1,	\
		_oe_num, _pu_num, _od_num,  8, 15,		\
		_rt2),						\
	STX7108_PIO_CONTROL_(_num+2, _group, _alt_num+2,	\
		_oe_num, _pu_num, _od_num, 16, 23,		\
		_rt3),						\
	STX7108_PIO_CONTROL_(_num+3, _group, _alt_num+3,	\
		_oe_num, _pu_num, _od_num, 24, 31,		\
		_rt4)

#define _NO_RETIMING \
	stm_pio_control_retime_style_none, 0, 0
#define _RETIMING(_group, _rt1, _rt2) \
	stm_pio_control_retime_style_packed, _rt1, _rt2

static const struct stm_pio_control_config stx7108_pio_control_configs[27] = {
	STX7108_PIO_CONTROL4(0, SYS_CFG_BANK2, 0, 15, 19, 23,
			     _NO_RETIMING,
			     _RETIMING(SYS_CFG_BANK2, 32, 33),
			     _NO_RETIMING,
			     _NO_RETIMING),
	STX7108_PIO_CONTROL4(4, SYS_CFG_BANK2, 4, 16, 20, 24,
			     _NO_RETIMING,
			     _NO_RETIMING,
			     _RETIMING(SYS_CFG_BANK2, 34, 35),
			     _RETIMING(SYS_CFG_BANK2, 36, 37)),
	STX7108_PIO_CONTROL4(8, SYS_CFG_BANK2, 8, 17, 21, 25,
			     _RETIMING(SYS_CFG_BANK2, 38, 39),
			     _RETIMING(SYS_CFG_BANK2, 40, 41),
			     _RETIMING(SYS_CFG_BANK2, 42, 43),
			     _RETIMING(SYS_CFG_BANK2, 44, 45)),
	STX7108_PIO_CONTROL(12, SYS_CFG_BANK2, 12, 18, 22, 26, 0, 7,
			    _RETIMING(SYS_CFG_BANK2, 46, 47)),
	STX7108_PIO_CONTROL(13, SYS_CFG_BANK2, 13, 18, 22, 26, 8, 15,
			    _RETIMING(SYS_CFG_BANK2, 48, 49)),
	STX7108_PIO_CONTROL(14, SYS_CFG_BANK2, 14, 18, 22, 26, 16, 23,
			    _RETIMING(SYS_CFG_BANK2, 50, 51)),
	STX7108_PIO_CONTROL4(15, SYS_CFG_BANK4, 0, 12, 16, 20,
			     _RETIMING(SYS_CFG_BANK4, 48, 49),
			     _RETIMING(SYS_CFG_BANK4, 50, 51),
			     _RETIMING(SYS_CFG_BANK4, 52, 53),
			     _RETIMING(SYS_CFG_BANK4, 54, 55)),
	STX7108_PIO_CONTROL4(19, SYS_CFG_BANK4, 4, 13, 17, 21,
			     _RETIMING(SYS_CFG_BANK4, 56, 57),
			     _RETIMING(SYS_CFG_BANK4, 58, 59),
			     _RETIMING(SYS_CFG_BANK4, 60, 61),
			     _RETIMING(SYS_CFG_BANK4, 62, 63)),
	STX7108_PIO_CONTROL4(23, SYS_CFG_BANK4, 8, 14, 18, 22,
			     _RETIMING(SYS_CFG_BANK4, 64, 65),
			     _NO_RETIMING,
			     _NO_RETIMING,
			     _NO_RETIMING),
};

static struct stm_pio_control stx7108_pio_controls[27];

static int stx7108_pio_config(unsigned gpio,
		enum stm_pad_gpio_direction direction, int function, void *priv)
{
	struct stm_pio_control_pad_config *config = priv;

	return stm_pio_control_config_all(gpio, direction, function, config,
		stx7108_pio_controls,
		ARRAY_SIZE(stx7108_pio_controls), 6);
}

#ifdef CONFIG_DEBUG_FS
static void stx7108_pio_report(unsigned gpio, char *buf, int len)
{
	stm_pio_control_report_all(gpio, stx7108_pio_controls,
				   buf, len);
}
#else
#define stx7108_pio_report NULL
#endif

static const struct stm_pad_ops stx7108_pad_ops = {
	.gpio_config = stx7108_pio_config,
	.gpio_report = stx7108_pio_report,
};

static void __init stx7108_pio_init(void)
{
	stm_pio_control_init(stx7108_pio_control_configs, stx7108_pio_controls,
			     ARRAY_SIZE(stx7108_pio_control_configs));
}

/* sysconf resources ------------------------------------------------------ */

static struct platform_device stx7108_sysconf_devices[] = {
	{
		.name		= "stm-sysconf",
		.id		= 0,
		.num_resources	= 1,
		.resource	= (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfde30000, 0x34),
		},
		.dev.platform_data = &(struct stm_plat_sysconf_data) {
			.groups_num = 2,
			.groups = (struct stm_plat_sysconf_group []) {
				{
					.group = SYS_STA_BANK0,
					.offset = 0,
					.name = "BANK0 SYS_STA",
				}, {
					.group = SYS_CFG_BANK0,
					.offset = 4,
					.name = "BANK0 SYS_CFG",
				}
			},
		}
	}, {
		.name		= "stm-sysconf",
		.id		= 1,
		.num_resources	= 1,
		.resource	= (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfde20000, 0x94),
		},
		.dev.platform_data = &(struct stm_plat_sysconf_data) {
			.groups_num = 2,
			.groups = (struct stm_plat_sysconf_group []) {
				{
					.group = SYS_STA_BANK1,
					.offset = 0,
					.name = "BANK1 SYS_STA",
				}, {
					.group = SYS_CFG_BANK1,
					.offset = 0x3c,
					.name = "BANK1 SYS_CFG",
				}
			},
		}
	}, {
		.name		= "stm-sysconf",
		.id		= 2,
		.num_resources	= 1,
		.resource	= (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfda50000, 0xfc),
		},
		.dev.platform_data = &(struct stm_plat_sysconf_data) {
			.groups_num = 2,
			.groups = (struct stm_plat_sysconf_group []) {
				{
					.group = SYS_CFG_BANK2,
					.offset = 0,
					.name = "BANK2 SYS_CFG",
				}, {
					.group = SYS_STA_BANK2,
					.offset = 0xe4,
					.name = "BANK2 SYS_STA",
				}
			},
		}
	}, {
		.name		= "stm-sysconf",
		.id		= 3,
		.num_resources	= 1,
		.resource	= (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfd500000, 0x40),
		},
		.dev.platform_data = &(struct stm_plat_sysconf_data) {
			.groups_num = 2,
			.groups = (struct stm_plat_sysconf_group []) {
				{
					.group = SYS_STA_BANK3,
					.offset = 0,
					.name = "BANK3 SYS_STA",
				}, {
					.group = SYS_CFG_BANK3,
					.offset = 0x18,
					.name = "BANK3 SYS_CFG",
				}
			},
		}
	}, {
		.name		= "stm-sysconf",
		.id		= 4,
		.num_resources	= 1,
		.resource	= (struct resource[]) {
			STM_PLAT_RESOURCE_MEM(0xfe700000, 0x12c),
		},
		.dev.platform_data = &(struct stm_plat_sysconf_data) {
			.groups_num = 2,
			.groups = (struct stm_plat_sysconf_group []) {
				{
					.group = SYS_CFG_BANK4,
					.offset = 0,
					.name = "BANK4 SYS_CFG",
				}, {
					.group = SYS_STA_BANK4,
					.offset = 0x11c,
					.name = "BANK4 SYS_STA",
				}
			},
		}
	},
};



/* Early initialisation-----------------------------------------------------*/

/* Initialise devices which are required early in the boot process. */
void __init stx7108_early_device_init(void)
{
	struct sysconf_field *sc;
	unsigned long devid;
	unsigned long chip_revision;

	/* Initialise PIO and sysconf drivers */

	sysconf_early_init(stx7108_sysconf_devices,
			ARRAY_SIZE(stx7108_sysconf_devices));
	stx7108_pio_init();
	stm_gpio_early_init(stx7108_pio_devices,
			ARRAY_SIZE(stx7108_pio_devices),
			ILC_FIRST_IRQ + ILC_NR_IRQS);
	stm_pad_init(ARRAY_SIZE(stx7108_pio_devices) * STM_GPIO_PINS_PER_PORT,
		     0, 0, &stx7108_pad_ops);

	sc = sysconf_claim(SYS_STA_BANK1, 0, 0, 31, "devid");
	devid = sysconf_read(sc);
	chip_revision = (devid >> 28) + 1;
	boot_cpu_data.cut_major = chip_revision;

	printk(KERN_INFO "STx7108 version %ld.x, %s core\n", chip_revision,
			STX7108_HOST_CORE ? "HOST" : "RT");

	/* We haven't configured the LPC, so the sleep instruction may
	 * do bad things. Thus we disable it here. */
	disable_hlt();
}



/* Pre-arch initialisation ------------------------------------------------ */

static int __init stx7108_postcore_setup(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(stx7108_pio_devices); i++)
		platform_device_register(&stx7108_pio_devices[i]);

	return platform_device_register(&stx7108_emi);
}
postcore_initcall(stx7108_postcore_setup);

/* Internal temperature sensor resources ---------------------------------- */
static void stx7108_temp_power(struct stm_device_state *device_state,
		enum stm_device_power_state power)
{
	int value = (power == stm_device_power_on) ? 1 : 0;

	stm_device_sysconf_write(device_state, "TEMP_PWR", value);
}

static struct platform_device stx7108_temp_device = {
	.name		   = "stm-temp",
	.id		     = -1,
	.dev.platform_data      = &(struct plat_stm_temp_data) {
		.correction_factor = 20,
		.device_config = &(struct stm_device_config) {
			.sysconfs_num = 4,
			.power = stx7108_temp_power,
			.sysconfs = (struct stm_device_sysconf []) {
				STM_DEVICE_SYS_CFG_BANK(1,
							8, 9, 9, "TEMP_PWR"),
				STM_DEVICE_SYS_CFG_BANK(3,
							8, 4, 8, "DCORRECT"),
				STM_DEVICE_SYS_CFG_BANK(2,
							7, 8, 8, "OVERFLOW"),
				STM_DEVICE_SYS_CFG_BANK(2,
							7, 10, 16, "DATA"),
			},
		}
	},
};


/* Late initialisation ---------------------------------------------------- */

static struct platform_device *stx7108_devices[] __initdata = {
	&stx7108_fdma_devices[0],
	&stx7108_fdma_devices[1],
	&stx7108_fdma_devices[2],
	&stx7108_fdma_xbar_device,
	&stx7108_st231_coprocessor_devices[0],
	&stx7108_st231_coprocessor_devices[1],
	&stx7108_st231_coprocessor_devices[2],
	&stx7108_sysconf_devices[0],
	&stx7108_sysconf_devices[1],
	&stx7108_sysconf_devices[2],
	&stx7108_sysconf_devices[3],
	&stx7108_sysconf_devices[4],
	&stx7108_devhwrandom_device,
	&stx7108_temp_device,
};

static int __init stx7108_devices_setup(void)
{
	platform_add_devices(stx7108_devices,
			ARRAY_SIZE(stx7108_devices));

	if (boot_cpu_data.cut_major == 1)
		stx7108_st40_coproc_device.dev.platform_data =
					&stx7108_cut1_st40_coproc_dev_data;
	else
		stx7108_st40_coproc_device.dev.platform_data =
					&stx7108_cut2_st40_coproc_dev_data;

	platform_device_register(&stx7108_st40_coproc_device);

	return 0;
}
device_initcall(stx7108_devices_setup);
