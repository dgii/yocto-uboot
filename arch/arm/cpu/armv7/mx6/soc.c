/*
 * (C) Copyright 2007
 * Sascha Hauer, Pengutronix
 *
 * (C) Copyright 2009-2013 Freescale Semiconductor, Inc.
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <common.h>
#include <asm/armv7.h>
#include <asm/pl310.h>
#include <asm/errno.h>
#include <asm/io.h>
#include <asm/arch/imx-regs.h>
#include <asm/arch/crm_regs.h>
#include <asm/arch/clock.h>
#include <asm/arch/sys_proto.h>
#include <asm/imx-common/boot_mode.h>
#include <asm/imx-common/dma.h>
#include <stdbool.h>

struct scu_regs {
	u32	ctrl;
	u32	config;
	u32	status;
	u32	invalidate;
	u32	fpga_rev;
};

#define TEMPERATURE_MIN		-40
#define FACTOR1			15976
#define FACTOR2			4297157
#define MEASURE_FREQ		327

#define REG_VALUE_TO_CEL(ratio, raw) \
	((raw_n40c - raw) * 100 / ratio - 40)

u32 get_cpu_rev(void)
{
	struct anatop_regs *anatop = (struct anatop_regs *)ANATOP_BASE_ADDR;
	u32 reg = readl(&anatop->digprog_sololite);
	u32 type = ((reg >> 16) & 0xff);

	if (type != MXC_CPU_MX6SL) {
		reg = readl(&anatop->digprog);
		struct scu_regs *scu = (struct scu_regs *)SCU_BASE_ADDR;
		u32 cfg = readl(&scu->config) & 3;
		type = ((reg >> 16) & 0xff);
		if (type == MXC_CPU_MX6DL) {
			if (!cfg)
				type = MXC_CPU_MX6SOLO;
		}

		if (type == MXC_CPU_MX6Q) {
			if (cfg == 1)
				type = MXC_CPU_MX6D;
		}

	}
	reg &= 0xff;		/* mx6 silicon revision */
	return (type << 12) | (reg + 0x10);
}

#ifdef CONFIG_REVISION_TAG
u32 __weak get_board_rev(void)
{
	u32 cpurev = get_cpu_rev();
	u32 type = ((cpurev >> 12) & 0xff);
	if (type == MXC_CPU_MX6SOLO)
		cpurev = (MXC_CPU_MX6DL) << 12 | (cpurev & 0xFFF);

	if (type == MXC_CPU_MX6D)
		cpurev = (MXC_CPU_MX6Q) << 12 | (cpurev & 0xFFF);

	return cpurev;
}
#endif

void init_aips(void)
{
	struct aipstz_regs *aips1, *aips2;

	aips1 = (struct aipstz_regs *)AIPS1_BASE_ADDR;
	aips2 = (struct aipstz_regs *)AIPS2_BASE_ADDR;

	/*
	 * Set all MPROTx to be non-bufferable, trusted for R/W,
	 * not forced to user-mode.
	 */
	writel(0x77777777, &aips1->mprot0);
	writel(0x77777777, &aips1->mprot1);
	writel(0x77777777, &aips2->mprot0);
	writel(0x77777777, &aips2->mprot1);

	/*
	 * Set all OPACRx to be non-bufferable, not require
	 * supervisor privilege level for access,allow for
	 * write access and untrusted master access.
	 */
	writel(0x00000000, &aips1->opacr0);
	writel(0x00000000, &aips1->opacr1);
	writel(0x00000000, &aips1->opacr2);
	writel(0x00000000, &aips1->opacr3);
	writel(0x00000000, &aips1->opacr4);
	writel(0x00000000, &aips2->opacr0);
	writel(0x00000000, &aips2->opacr1);
	writel(0x00000000, &aips2->opacr2);
	writel(0x00000000, &aips2->opacr3);
	writel(0x00000000, &aips2->opacr4);
}

/*
 * Set the VDDSOC
 *
 * Mask out the REG_CORE[22:18] bits (REG2_TRIG) and set
 * them to the specified millivolt level.
 * Possible values are from 0.725V to 1.450V in steps of
 * 0.025V (25mV).
 */
void set_vddsoc(u32 mv)
{
	struct anatop_regs *anatop = (struct anatop_regs *)ANATOP_BASE_ADDR;
	u32 val, reg = readl(&anatop->reg_core);

	if (mv < 725)
		val = 0x00;	/* Power gated off */
	else if (mv > 1450)
		val = 0x1F;	/* Power FET switched full on. No regulation */
	else
		val = (mv - 700) / 25;

	/*
	 * Mask out the REG_CORE[22:18] bits (REG2_TRIG)
	 * and set them to the calculated value (0.7V + val * 0.25V)
	 */
	reg = (reg & ~(0x1F << 18)) | (val << 18);
	writel(reg, &anatop->reg_core);

	/* ROM may modify LDO ramp up time according to fuse setting for safe,
	 * we need to reset these settings to match the reset value: 0'b00
	 */
	reg = readl(&anatop->ana_misc2);
	reg &= ~(0x3f << 24);
	writel(reg, &anatop->ana_misc2);

}

static void imx_set_wdog_powerdown(bool enable)
{
	struct wdog_regs *wdog1 = (struct wdog_regs *)WDOG1_BASE_ADDR;
	struct wdog_regs *wdog2 = (struct wdog_regs *)WDOG2_BASE_ADDR;

	/* Write to the PDE (Power Down Enable) bit */
	writew(enable, &wdog1->wmcr);
	writew(enable, &wdog2->wmcr);
}

static u32 read_temp_fuse(void)
{
	struct ocotp_regs *ocotp = (struct ocotp_regs *)OCOTP_BASE_ADDR;
	struct mxc_ccm_reg *mxc_ccm = (struct mxc_ccm_reg *)CCM_BASE_ADDR;
	struct fuse_bank *bank = &ocotp->bank[1];
	struct fuse_bank1_regs *fuse_bank1 =
			(struct fuse_bank1_regs *)bank->fuse_regs;
	unsigned int ccm_ccgr2;
	u32 fuse;

	/* enable OCOTP_CTRL clock in CCGR2 */
	ccm_ccgr2 = readl(&mxc_ccm->CCGR2);
	writel(ccm_ccgr2 | MXC_CCM_CCGR2_OCOTP_CTRL_MASK, &mxc_ccm->CCGR2);

	/* Read temp parameters from fuses */
	fuse = readl(&fuse_bank1->ana1);

	/* restore CCGR2 */
	writel(ccm_ccgr2, &mxc_ccm->CCGR2);

	return fuse;
}

static int read_cpu_temperature(void)
{
	int temperature;
	unsigned int reg, tmp;
	unsigned int raw_25c, raw_n40c, ratio;
	struct anatop_regs *anatop = (struct anatop_regs *)ANATOP_BASE_ADDR;
	u32 fuse;

	/* need to make sure pll3 is enabled for thermal sensor */
	if ((readl(&anatop->usb1_pll_480_ctrl) &
			BM_ANADIG_USB1_PLL_480_CTRL_LOCK) == 0) {
		/* enable pll's power */
		writel(BM_ANADIG_USB1_PLL_480_CTRL_POWER,
				&anatop->usb1_pll_480_ctrl_set);
		writel(0x80, &anatop->ana_misc2_clr);
		/* wait for pll lock */
		while ((readl(&anatop->usb1_pll_480_ctrl) &
			BM_ANADIG_USB1_PLL_480_CTRL_LOCK) == 0)
			;
		/* disable bypass */
		writel(BM_ANADIG_USB1_PLL_480_CTRL_BYPASS,
				&anatop->usb1_pll_480_ctrl_clr);
		/* enable pll output */
		writel(BM_ANADIG_USB1_PLL_480_CTRL_ENABLE,
				&anatop->usb1_pll_480_ctrl_set);
	}

	fuse = read_temp_fuse();
	if (fuse == 0 || fuse == 0xffffffff || (fuse & 0xfff00000) == 0)
		return TEMPERATURE_MIN;

	/*
	 * fuse data layout:
	 * [31:20] sensor value @ 25C
	 * [19:8] sensor value of hot
	 * [7:0] hot temperature value
	 */
	raw_25c = fuse >> 20;

	/*
	 * The universal equation for thermal sensor
	 * is slope = 0.4297157 - (0.0015976 * 25C fuse),
	 * here we convert them to integer to make them
	 * easy for counting, FACTOR1 is 15976,
	 * FACTOR2 is 4297157. Our ratio = -100 * slope
	 */
	ratio = ((FACTOR1 * raw_25c - FACTOR2) + 50000) / 100000;
	debug("Thermal sensor with ratio = %d\n", ratio);

	raw_n40c = raw_25c + (13 * ratio) / 20;

	/*
	 * now we only use single measure, every time we read
	 * the temperature, we will power on/down anadig thermal
	 * module
	 */
	writel(BM_ANADIG_TEMPSENSE0_POWER_DOWN, &anatop->tempsense0_clr);
	writel(BM_ANADIG_ANA_MISC0_REFTOP_SELBIASOFF, &anatop->ana_misc0_set);

	/* write measure freq */
	reg = readl(&anatop->tempsense1);
	reg &= ~BM_ANADIG_TEMPSENSE1_MEASURE_FREQ;
	reg |= MEASURE_FREQ;
	writel(reg, &anatop->tempsense1);

	writel(BM_ANADIG_TEMPSENSE0_MEASURE_TEMP, &anatop->tempsense0_clr);
	writel(BM_ANADIG_TEMPSENSE0_FINISHED, &anatop->tempsense0_clr);
	writel(BM_ANADIG_TEMPSENSE0_MEASURE_TEMP, &anatop->tempsense0_set);

	while ((readl(&anatop->tempsense0) &
			BM_ANADIG_TEMPSENSE0_FINISHED) == 0)
		udelay(10000);

	reg = readl(&anatop->tempsense0);
	tmp = (reg & BM_ANADIG_TEMPSENSE0_TEMP_VALUE)
		>> BP_ANADIG_TEMPSENSE0_TEMP_VALUE;
	writel(BM_ANADIG_TEMPSENSE0_FINISHED, &anatop->tempsense0_clr);

	if (tmp <= raw_n40c)
		temperature = REG_VALUE_TO_CEL(ratio, tmp);
	else
		temperature = TEMPERATURE_MIN;
	/* power down anatop thermal sensor */
	writel(BM_ANADIG_TEMPSENSE0_POWER_DOWN, &anatop->tempsense0_set);
	writel(BM_ANADIG_ANA_MISC0_REFTOP_SELBIASOFF, &anatop->ana_misc0_clr);

	return temperature;
}

#define OCOTP_HOT_TEMP_MASK	0xff
void check_cpu_temperature(void)
{
	int cpu_tmp, temp_max, temp_hot;
	u32 fuse;

	/* Set temperature limits for U-Boot depending on on the maximum die
	 * temperature stored in the OCOTP fuses. This allows higher margin in
	 * industrial and automotive rated i.MX6 SoCs
	 */
	fuse = read_temp_fuse();
	temp_max = fuse & OCOTP_HOT_TEMP_MASK;
	temp_hot = temp_max - CONFIG_BOOT_TEMP_BELOW_MAX;
	debug("temp_max: %d C, temp_hot: %d C\n", temp_max, temp_hot);

	cpu_tmp = read_cpu_temperature();
	while (cpu_tmp > temp_hot) {
		printf("CPU is %d C, too hot to boot, waiting...\n", cpu_tmp);
		udelay(5000000);
		cpu_tmp = read_cpu_temperature();
	}
	if (cpu_tmp < TEMPERATURE_MIN || cpu_tmp > temp_max)
		printf("CPU:   WARNING. Measured temperature out of limits!\n");
	else
		printf("CPU:   Temperature %d C, calibration data: 0x%x\n",
			cpu_tmp, fuse);
}

static void imx_reset_pfd(void)
{
	struct anatop_regs *anatop = (struct anatop_regs *)ANATOP_BASE_ADDR;

	/*
	 * Per the IC design, we need to gate/ungate all the unused PFDs
	 * to make sure PFD is working correctly, otherwise, PFDs may not
	 * not output clock after reset.
	 */

	writel(BM_ANADIG_PFD_480_PFD3_CLKGATE  |
		BM_ANADIG_PFD_480_PFD2_CLKGATE |
		BM_ANADIG_PFD_480_PFD1_CLKGATE |
		BM_ANADIG_PFD_480_PFD0_CLKGATE, &anatop->pfd_480_set);
#ifdef CONFIG_MX6Q
	writel(BM_ANADIG_PFD_528_PFD2_CLKGATE  |
		BM_ANADIG_PFD_528_PFD1_CLKGATE |
		BM_ANADIG_PFD_528_PFD0_CLKGATE, &anatop->pfd_528_set);
#else
	writel(BM_ANADIG_PFD_528_PFD1_CLKGATE  |
		BM_ANADIG_PFD_528_PFD0_CLKGATE, &anatop->pfd_528_set);
#endif
	writel(BM_ANADIG_PFD_480_PFD3_CLKGATE  |
		BM_ANADIG_PFD_480_PFD2_CLKGATE |
		BM_ANADIG_PFD_480_PFD1_CLKGATE |
		BM_ANADIG_PFD_480_PFD0_CLKGATE, &anatop->pfd_480_clr);
#ifdef CONFIG_MX6Q
	writel(BM_ANADIG_PFD_528_PFD2_CLKGATE  |
		BM_ANADIG_PFD_528_PFD1_CLKGATE |
		BM_ANADIG_PFD_528_PFD0_CLKGATE, &anatop->pfd_528_clr);
#else
	writel(BM_ANADIG_PFD_528_PFD1_CLKGATE  |
		BM_ANADIG_PFD_528_PFD0_CLKGATE, &anatop->pfd_528_clr);
#endif
}

static void imx_set_vddpu_power_down(void)
{
	struct anatop_regs *anatop = (struct anatop_regs *)ANATOP_BASE_ADDR;
	u32 val;

	/* need to power down xPU in GPC before turn off PU LDO */
	val = readl(GPC_BASE_ADDR + 0x260);
	writel(val | 0x1, GPC_BASE_ADDR + 0x260);

	val = readl(GPC_BASE_ADDR + 0x0);
	writel(val | 0x1, GPC_BASE_ADDR + 0x0);
	while (readl(GPC_BASE_ADDR + 0x0) & 0x1)
		;

	/* disable VDDPU */
	val = 0x3e00;
	writel(val, &anatop->reg_core_clr);
}

static void imx_set_pcie_phy_power_down(void)
{
	u32 val;

	val = readl(IOMUXC_BASE_ADDR + 0x4);
	val |= 0x1 << 18;
	writel(val, IOMUXC_BASE_ADDR + 0x4);
}

int arch_cpu_init(void)
{
	init_aips();
	set_vddsoc(1200);	/* Set VDDSOC to 1.2V */

	imx_set_wdog_powerdown(false); /* Disable PDE bit of WMCR register */

	imx_reset_pfd();
	imx_set_pcie_phy_power_down();
	imx_set_vddpu_power_down();

#ifdef CONFIG_APBH_DMA
	/* Start APBH DMA */
	mxs_dma_init();
#endif

	return 0;
}

#ifndef CONFIG_SYS_DCACHE_OFF
void enable_caches(void)
{
	/* Enable D-cache. I-cache is already enabled in start.S */
	dcache_enable();
}
#endif

#if defined(CONFIG_FEC_MXC)
void imx_get_mac_from_fuse(int dev_id, unsigned char *mac)
{
	struct ocotp_regs *ocotp = (struct ocotp_regs *)OCOTP_BASE_ADDR;
	struct fuse_bank *bank = &ocotp->bank[4];
	struct fuse_bank4_regs *fuse =
			(struct fuse_bank4_regs *)bank->fuse_regs;

	u32 value = readl(&fuse->mac_addr_high);
	mac[0] = (value >> 8);
	mac[1] = value ;

	value = readl(&fuse->mac_addr_low);
	mac[2] = value >> 24 ;
	mac[3] = value >> 16 ;
	mac[4] = value >> 8 ;
	mac[5] = value ;

}
#endif

void boot_mode_apply(unsigned cfg_val)
{
	unsigned reg;
	struct src *psrc = (struct src *)SRC_BASE_ADDR;
	writel(cfg_val, &psrc->gpr9);
	reg = readl(&psrc->gpr10);
	if (cfg_val)
		reg |= 1 << 28;
	else
		reg &= ~(1 << 28);
	writel(reg, &psrc->gpr10);
}
/*
 * cfg_val will be used for
 * Boot_cfg4[7:0]:Boot_cfg3[7:0]:Boot_cfg2[7:0]:Boot_cfg1[7:0]
 * After reset, if GPR10[28] is 1, ROM will copy GPR9[25:0]
 * to SBMR1, which will determine the boot device.
 */
const struct boot_mode soc_boot_modes[] = {
	{"normal",	MAKE_CFGVAL(0x00, 0x00, 0x00, 0x00)},
	/* reserved value should start rom usb */
	{"usb",		MAKE_CFGVAL(0x01, 0x00, 0x00, 0x00)},
	{"sata",	MAKE_CFGVAL(0x20, 0x00, 0x00, 0x00)},
	{"escpi1:0",	MAKE_CFGVAL(0x30, 0x00, 0x00, 0x08)},
	{"escpi1:1",	MAKE_CFGVAL(0x30, 0x00, 0x00, 0x18)},
	{"escpi1:2",	MAKE_CFGVAL(0x30, 0x00, 0x00, 0x28)},
	{"escpi1:3",	MAKE_CFGVAL(0x30, 0x00, 0x00, 0x38)},
	/* 4 bit bus width */
	{"esdhc1",	MAKE_CFGVAL(0x40, 0x20, 0x00, 0x00)},
	{"esdhc2",	MAKE_CFGVAL(0x40, 0x28, 0x00, 0x00)},
	{"esdhc3",	MAKE_CFGVAL(0x40, 0x30, 0x00, 0x00)},
	{"esdhc4",	MAKE_CFGVAL(0x40, 0x38, 0x00, 0x00)},
	{NULL,		0},
};

/* The following array contains the boot device string, and the value and mask
 * of SBMR1 register associated to it.
 */
const struct boot_device boot_device_sel[] = {
	{"NOR/OneNAND",	0x00,	0xf0},
	{"usb",		0x10,	0xf0},
	{"sata",	0x20,	0xf0},
	{"esdhc1",	0x40,	0x18C0},
	{"esdhc2",	0x840,	0x18C0},
	{"esdhc3",	0x1040,	0x18C0},
	{"esdhc4",	0x1840,	0x18C0},
	{NULL,		0},
};

const char * boot_mode_string(void)
{
	struct src *psrc = (struct src *)SRC_BASE_ADDR;
	unsigned reg;
	int i;

	reg = readl(&psrc->sbmr1);
	for (i = 0; boot_device_sel[i].name != NULL; i++) {
		if ((reg & boot_device_sel[i].mask) == boot_device_sel[i].val)
			return boot_device_sel[i].name;
	}

	return "unknown";
}

void s_init(void)
{
}

#ifndef CONFIG_SYS_L2CACHE_OFF
#define IOMUXC_GPR11_L2CACHE_AS_OCRAM 0x00000002
void v7_outer_cache_enable(void)
{
	struct pl310_regs *const pl310 = (struct pl310_regs *)L2_PL310_BASE;
	unsigned int val;

#if defined CONFIG_MX6SL
	struct iomuxc *iomux = (struct iomuxc *)IOMUXC_BASE_ADDR;
	val = readl(&iomux->gpr[11]);
	if (val & IOMUXC_GPR11_L2CACHE_AS_OCRAM) {
		/* L2 cache configured as OCRAM, reset it */
		val &= ~IOMUXC_GPR11_L2CACHE_AS_OCRAM;
		writel(val, &iomux->gpr[11]);
	}
#endif

	writel(0x132, &pl310->pl310_tag_latency_ctrl);
	writel(0x132, &pl310->pl310_data_latency_ctrl);

	val = readl(&pl310->pl310_prefetch_ctrl);

	/* Turn on the L2 I/D prefetch */
	val |= 0x30000000;

	/*
	 * The L2 cache controller(PL310) version on the i.MX6D/Q is r3p1-50rel0
	 * The L2 cache controller(PL310) version on the i.MX6DL/SOLO/SL is r3p2
	 * But according to ARM PL310 errata: 752271
	 * ID: 752271: Double linefill feature can cause data corruption
	 * Fault Status: Present in: r3p0, r3p1, r3p1-50rel0. Fixed in r3p2
	 * Workaround: The only workaround to this erratum is to disable the
	 * double linefill feature. This is the default behavior.
	 */

#ifndef CONFIG_MX6Q
	val |= 0x40800000;
#endif
	writel(val, &pl310->pl310_prefetch_ctrl);

	val = readl(&pl310->pl310_power_ctrl);
	val |= L2X0_DYNAMIC_CLK_GATING_EN;
	val |= L2X0_STNDBY_MODE_EN;
	writel(val, &pl310->pl310_power_ctrl);

	setbits_le32(&pl310->pl310_ctrl, L2X0_CTRL_EN);
}

void v7_outer_cache_disable(void)
{
	struct pl310_regs *const pl310 = (struct pl310_regs *)L2_PL310_BASE;

	clrbits_le32(&pl310->pl310_ctrl, L2X0_CTRL_EN);
}
#endif /* !CONFIG_SYS_L2CACHE_OFF */
