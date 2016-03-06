#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/hrtimer.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/cpu.h>
#include <linux/perf_event.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/time.h>

#include <asm/mic_def.h>
#include <asm/mic/micreghelper.h>

#include "micstat.h"

#define NR_CPUS_MIC 244
// #define MICSTAT_BIND_KTHREAD

extern void __iomem *mic_sbox_mmio_va;

#define MIC_READ_MMIO_REG(var, offset) \
    var.value = readl((const volatile void __iomem *)((unsigned long)(mic_sbox_mmio_va) + (offset)));
#define MIC_WRITE_MMIO_REG(var, offset) \
    writel(var.value, (const volatile void __iomem *)((unsigned long)(mic_sbox_mmio_va) + (offset)));
#define mr_sbox_rl(dummy, offset) readl(mic_sbox_mmio_va + (offset))

#define CHECK_RET(ret) do { \
    if (ret < 0) { \
        return ret; \
    } \
} while (0)

#define USE_KTHREAD
// #define USE_TIMER
// #define USE_HTIMER

int lastdie;

static bool randfreq = 1;
static bool on;
static unsigned int period;

// freq

static void init_rand_freq() {
    int ret;
    struct cpufreq_policy policy;
    policy.cpu = 1;
    ret = cpufreq_driver_target(&policy, 857142, CPUFREQ_RELATION_L);
    if (ret < 0) {
        printk(KERN_INFO "micstat: cpufreq_driver_target failed: %d\n", ret);
    }
}


// helpers
// SVID Port


// END of SVID Port
// SMC port

extern int gmbus_i2c_read(uint8_t, uint8_t, uint8_t, uint8_t *, uint16_t);
extern int gmbus_i2c_write(uint8_t, uint8_t, uint8_t, uint8_t *, uint16_t);

#define FIX_DBOX	0

#if FIX_DBOX
/*
 * Pre-emptive restoring DBOX-0 register access.
 * A glitch during clock speed changes (PM or GPU_HOT)
 * may under some rare circumstances break access to DBOX
 * registers. It is very rare, requires hours of tailored
 * simulation to reproduce, never seen in the wild (yet).
 * The gmbus controller sits in the DBOX and is affected.
 * Calling this routine prior to every gmbus read/write
 * reduces risk of hitting this bug to a single SMC register,
 * which has been deemed acceptable for B-step KnCs.
 * Only alternative is to perform repeated transaction(s)
 * until a stable result is obtained, which will be costly
 * in performance.
 */
static void
mr_smc_deglitch(void)
{
  mr_dbox_rl(0, 0x600);
  mr_dbox_rl(0, 0x2440);
}
#else
#define mr_smc_deglitch();	/* As nothing */
#endif

/*
**
** SMC API
**
** See "Knights Corner System Managment Architecture Specification"
** for details on the SMC internals and supported APIs.
**
** This module is based on rev 0.31
**
*/

#define MR_SMC_ADDR		0x28	/* SMC DVO-B Slave address */

#define MR_SMC_PCI_VID		0x00	/* PCI Vendor ID, 4 */
#define MR_SMC_PCI_DID		0x02	/* PCI Device ID, 4 */
#define MR_SMC_PCI_BCC		0x04	/* PCI Base Class Code, 4 */
#define MR_SMC_PCI_SCC		0x05	/* PCI Sub Class Code, 4 */
#define MR_SMC_PCI_PI		0x06	/* PCI Programming Interface, 4 */
#define MR_SMC_PCI_SMBA		0x07	/* PCI MBus Manageability Address, 4 */
#define MR_SMC_UUID		0x10	/* Universally Unique Identification, 16 */
#define MR_SMC_FW_VERSION	0x11	/* SMC Firmware Version, 4 */
#define MR_SMC_EXE_DOMAIN	0x12	/* SMC Execution Domain, 4 */
#define MR_SMC_STS_SELFTEST	0x13	/* SMC Self-Test Results, 4 */
#define MR_SMC_HW_REVISION	0x14	/* SMC Hardware Revision, 4 */
#define MR_SMC_SERIAL		0x15	/* Card serial number, 12 */
#define MR_SMC_SMB_RESTRT	0x17	/* Restart SMBus addr negotiation, 4 */

#define MR_SMC_CPU_POST		0x1a	/* POST Register, 4 */
#define MR_SMC_ZOMBIE		0x1b	/* Zombie Mode Enable, 4 */
#define MR_SMC_CPU_ID		0x1c	/* CPU Identifier, 4 */

#define MR_SMC_SEL_ENTRY_SEL	0x20	/* SEL Entry Selection Register, 4 */
#define MR_SMC_SEL_DATA		0x21	/* SEL Data register, <N> */
#define MR_SMC_SDR_ENTRY_SEL	0x22	/* SDR Entry Selection Register, 4 */
#define MR_SMC_SDR_DATA		0x23	/* SDR Data register, <N> */

#define MR_SMC_PWR_PCIE		0x28	/* PCIe Power Reading, 4 */
#define MR_SMC_PWR_2X3		0x29	/* 2x3 Power Reading, 4 */
#define MR_SMC_PWR_2X4		0x2a	/* 2x4 Power Reading, 4 */
#define MR_SMC_FORCE_TTL	0x2b	/* Forced Throttle, 4 */
#define MR_SMC_PWR_LIM_0	0x2c	/* Power Limit 0, 4 */
#define MR_SMC_TIME_WIN_0	0x2d	/* Time Window 0, 4 */
#define MR_SMC_PWR_LIM0_GRD	0x2e	/* Power Limit 0 Guardband, 4 */
#define MR_SMC_PWR_LIM_1	0x2f	/* Power Limit 1, 4 */
#define MR_SMC_TIME_WIN_1	0x30	/* Time Window 1, 4 */
#define MR_SMC_INCL_3V3		0x31	/* Include 3.3 V, 4 */
#define MR_SMC_PWR_LIM_PERS	0x32	/* Power Limit Persistence, 4 */
#define MR_SMC_CLAMP_MODE	0x33	/* Clamp Mode, 4 */
#define MR_SMC_ENERGY_STS_0	0x34	/* Energy Status 0, 4 */
#define MR_SMC_AVG_PWR_0	0x35	/* Average Power 0, 4 */
#define MR_SMC_AVG_PWR_1	0x36	/* Average Power 1, 4 */
#define MR_SMC_MIN_PWR		0x37	/* Min Power, 4 */
#define MR_SMC_PWR_TTL_DUR	0x38	/* Power Throttle Duration, 4 */
#define MR_SMC_PWR_TTL		0x39	/* Power Throttling, 4 */
#define MR_SMC_PWR_INST		0x3a	/* Instantaneous Power Reading, 4 */
#define MR_SMC_PWR_IMAX		0x3b	/* Maximum Power Reading, 4 */
#define MR_SMC_VOLT_VCCP	0x3c	/* VCCP VR Output Voltage, 4 */
#define MR_SMC_VOLT_VDDQ	0x3d	/* VDDQ VR Output Voltage, 4 */
#define MR_SMC_VOLT_VDDG	0x3e	/* VDDG VR Output Voltage, 4 */

#define MR_SMC_TEMP_CPU		0x40	/* CPU DIE Temperature, 4 */
#define MR_SMC_TEMP_EXHAUST	0x41	/* Card Exhaust Temperature, 4 */
#define MR_SMC_TEMP_INLET	0x42	/* Card Inlet Temperature, 4 */
#define MR_SMC_TEMP_VCCP	0x43	/* VCCP VR Temperature, 4 */
#define MR_SMC_TEMP_VDDG	0x44	/* VDDG VR Temperature, 4 */
#define MR_SMC_TEMP_VDDQ	0x45	/* VDDQ VR Temperature, 4 */
#define MR_SMC_TEMP_GDDR	0x46	/* GDDR Temperature, 4 */
#define MR_SMC_TEMP_EAST	0x47	/* East Temperature, 4 */
#define MR_SMC_TEMP_WEST	0x48	/* West Temperature, 4 */
#define MR_SMC_FAN_TACH		0x49	/* Fan RPM, 4 */
#define MR_SMC_FAN_PWM		0x4a	/* Fan PWM Percent, 4 */
#define MR_SMC_FAN_PWM_ADD	0x4b	/* Fan PWM Adder, 4 */
#define MR_SMC_TCRITICAL	0x4c	/* KNC Tcritical temperature, 4 */
#define MR_SMC_TCONTROL		0x4d	/* KNC Tcontrol temperature, 4 */
#define MR_SMC_TRM_TTL_DUR	0x4e	/* Thermal Throttle Duration, 4 */
#define MR_SMC_TRM_TTL		0x4f	/* Thermal Throttling, 4 */
#define MR_SMC_TRM_PUSH		0x50	/* Target for die temp push, 4 */

#define MR_SMC_PWR_VCCP		0x58	/* VCCP VR Output Power, 4 */
#define MR_SMC_PWR_VDDQ		0x59	/* VDDQ VR Output Power, 4 */
#define MR_SMC_PWR_VDDG		0x5a	/* VDDG VR Output Power, 4 */

#define MR_SMC_LED_CODE		0x60	/* LED blink code, 4 */


/*
 * Simple I/O access routines for most SMC registers.
 * All but UUID & SERIAL are 4 bytes in size.
 */
#define SMC_TRACK	0

#if SMC_TRACK
#define RL	printk("%s: %2x -> %08x, rtn %d\n",    __FUNCTION__, reg, *val, rl)
#define WL	printk("%s: %2x <- %08x, rtn %d\n",    __FUNCTION__, reg, *val, rl)
#else
#define RL	/* As nothing */
#define WL	/* As nothing */
#endif

static char *
gm_err(int err)
{
  char * str = "unknown";

  switch(err) {
    case -1:  str = "timeout"; break;
    case -2:  str = "ack timeout"; break;
    case -3:  str = "interrupted"; break;
    case -4:  str = "invalid command"; break;
  }

  return str;
}


int
mr_smc_rd(uint8_t reg, uint32_t * val)
{
  int		rl;

  mr_smc_deglitch();
  rl = gmbus_i2c_read(2, MR_SMC_ADDR, reg, (uint8_t *) val, sizeof(*val));
  RL;
  if (rl == sizeof(uint32_t))
    return 0;

  /*
   * Something failed, do a dummy read to get I2C bus in a known good state.
   *TBD: Do retries, and if so how many?
   */
  printk("smc_rd: error %d (%s), reg %02x\n", rl, gm_err(rl), reg);
  mr_smc_deglitch();
  gmbus_i2c_read(2, MR_SMC_ADDR, MR_SMC_FW_VERSION, (uint8_t *) &rl, sizeof(rl));
  *val = 0;
  return 1;
}

int
mr_smc_wr(uint8_t reg, uint32_t * val)
{
  int		rl;

  WL;
  mr_smc_deglitch();
  rl = gmbus_i2c_write(2, MR_SMC_ADDR, reg, (uint8_t *) val, sizeof(*val));
  if (rl == sizeof(uint32_t))
    return 0;

  /*
   * Something failed, do a dummy read to get I2C bus in a known good state.
   *TBD: Do retries, and if so how many?
   */
  printk("smc_wr: error %d (%s), reg %02x\n", rl, gm_err(rl), reg);
  mr_smc_deglitch();
  gmbus_i2c_read(2, MR_SMC_ADDR, MR_SMC_FW_VERSION, (uint8_t *) &rl, sizeof(rl));
  return 0;
}
#undef RL
#undef WL

// END of SMC port
#define USE_SMC
//
#define MAX_RATIO_ENTRIES 	4	/* Maximum number of entries in the mic_ratio_data array	*/ 
/* Static data on freq ratios and dividers */
struct mic_ratio_data {
	unsigned int divider;
	unsigned int minratio;
	unsigned int maxratio;
};	

/* Static data on the core clock PLLs */ 
struct mic_pll_data {
	unsigned int	bclk	;	/* Base clock  in MHz*/
	unsigned int	ffpos	;	/* Position of the feedback divider bits in ratio code	*/
	struct mic_ratio_data ratio_data[MAX_RATIO_ENTRIES];
};

#define K1OM_FF_POS	8	/*Bit position of the feed forward divider      */
#define ICCDIV_POS		25
#define ICCDIV_MASK		0x1f
#define ICC_REFCLK		4000
#define MIN_ICCDIV		18
#define MAX_ICCDIV		22
#define	K1OM_MCLK_2_FREQ(mclk)		(((mclk) & 0x7FE) >> 1)	/* Convert mclk ratio read from SBOX to freq ratio */

static struct mic_pll_data pll_data_k1om = {
    .bclk = 200000,
    .ffpos = K1OM_FF_POS,
	.ratio_data = {{8, 8, 15},
		       {4, 8, 15},
		       {2, 8, 15},
		       {1, 8, 15},
		       },
};

static int get_baseclk(void) {
	sboxPcieVendorIdDeviceIdReg reg; /* Any reg type would do */
	int iccdiv;

	if (mic_sbox_mmio_va) {
		MIC_READ_MMIO_REG(reg, SBOX_SCRATCH4);
		iccdiv = (reg.value >> ICCDIV_POS) & ICCDIV_MASK;
		if((iccdiv < MIN_ICCDIV) || (iccdiv > MAX_ICCDIV)){
			printk("Iccdiv (%d) from scratch 4 out of range.\n",iccdiv);
			return 0;
		}
		return((ICC_REFCLK * 1000)/iccdiv);
	}
	return 0;
}

static inline int ff_from_code(unsigned int code, int ffpos) {
    return (8 / (1 << ((code >> ffpos) & 3)));
}

static inline unsigned int fb_from_code(unsigned int code, int ffpos) {
    return (code & (~((~0U) << ffpos)));
}

static inline unsigned int freq_from_code(int baseclk, int code, int ffpos)
{
    int fb, ff;

    ff = ff_from_code(code, ffpos);
    fb = fb_from_code(code, ffpos);
    return (baseclk * fb / ff);
}

// noop
// static uint64_t noop_restart(struct micstat_counter *cnt) {return 0;}
static void noop_reset(struct micstat_counter *cnt) {}

struct uint32_helper {
    uint32_t value;
};

// dietemp
union temp2_union {
    uint64_t value;
    struct {
        uint8_t tdie1;
        uint8_t tdie2;
        uint8_t tdie3;
        uint8_t tdie4;
        uint8_t tdie5;
        uint8_t tdie6;
        uint8_t tdie7;
        uint8_t tdie8;
    } temps;
};
static union temp2_union temp2_data;
STATIC_ASSERT(sizeof(temp2_data) == 8)

static uint64_t temp2_restart(struct micstat_counter *cnt) {
    uint32_t die1, die2, die3;
    die1 = mr_sbox_rl(0, SBOX_CURRENT_DIE_TEMP0);
    die2 = mr_sbox_rl(0, SBOX_CURRENT_DIE_TEMP1);
    die3 = mr_sbox_rl(0, SBOX_CURRENT_DIE_TEMP2);
    temp2_data.temps.tdie1 = GET_BITS(19, 10, die1);
    temp2_data.temps.tdie2 = GET_BITS(29, 20, die1);
    temp2_data.temps.tdie3 = GET_BITS( 9,  0, die2);
    temp2_data.temps.tdie4 = GET_BITS(19, 10, die2);
    temp2_data.temps.tdie5 = GET_BITS(29, 20, die2);
    temp2_data.temps.tdie6 = GET_BITS( 9,  0, die3);
    temp2_data.temps.tdie7 = GET_BITS(19, 10, die3);
    temp2_data.temps.tdie8 = GET_BITS(29, 20, die3);

    return temp2_data.value;
}

static int temp2_scnprintf(char *buf, int limit, uint64_t data) {
    char *ptr = buf;
    int ret;
    union temp2_union temp;
    temp.value = data;

#define PRINT_TEMP(t) do { \
    ret = scnprintf(ptr, limit, "\"%s\":%u,", #t, (unsigned) temp.temps.t); \
    CHECK_RET(ret); \
    ptr += ret; \
    limit -= ret; \
} while (0)
    PRINT_TEMP(tdie1);
    PRINT_TEMP(tdie2);
    PRINT_TEMP(tdie3);
    PRINT_TEMP(tdie4);
    PRINT_TEMP(tdie5);
    PRINT_TEMP(tdie6);
    PRINT_TEMP(tdie7);
    PRINT_TEMP(tdie8);
#undef PRINT_TEMP
    
    return ptr - buf;
}

// temp
union temp_union {
    uint64_t value;
    struct {
        uint8_t tdie;        // highest die
        uint8_t tfin;        // fan inlet
        uint8_t tfout;
        uint8_t tgddr;
        uint8_t tvccp;
        uint8_t tvddg;
        uint8_t tvddq;
        uint8_t tdie0;
    } temps;
};
static union temp_union temp_data;
STATIC_ASSERT(sizeof(temp_data) == 8)

static uint64_t temp_restart(struct micstat_counter *cnt) {
#ifdef USE_SMC
    uint32_t fin, fout, gddr;
    uint32_t vccp, vddg, vddq;
    uint32_t die;
    
    mr_smc_rd(MR_SMC_TEMP_CPU, &die);
    mr_smc_rd(MR_SMC_TEMP_EXHAUST, &fout);
    mr_smc_rd(MR_SMC_TEMP_INLET, &fin);
    mr_smc_rd(MR_SMC_TEMP_VCCP, &vccp);
    mr_smc_rd(MR_SMC_TEMP_VDDG, &vddg);
    mr_smc_rd(MR_SMC_TEMP_VDDQ, &vddq);
    mr_smc_rd(MR_SMC_TEMP_GDDR, &gddr);
    temp_data.temps.tdie = GET_BITS(15, 0, die);
    temp_data.temps.tfin = GET_BITS(15, 0, fin);
    temp_data.temps.tfout = GET_BITS(15, 0, fout);
    temp_data.temps.tvccp = GET_BITS(15, 0, vccp);
    temp_data.temps.tvddg = GET_BITS(15, 0, vddg);
    temp_data.temps.tvddq = GET_BITS(15, 0, vddq);
    temp_data.temps.tgddr = GET_BITS(15, 0, gddr);
#else
    struct uint32_helper btr1, btr2, fsc, tsta;
    // struct uint32_helper die1, die2, dir3;
    MIC_READ_MMIO_REG(btr1, SBOX_BOARD_TEMP1);
    MIC_READ_MMIO_REG(btr2, SBOX_BOARD_TEMP2);
    MIC_READ_MMIO_REG(tsta, SBOX_THERMAL_STATUS);
    // MIC_READ_MMIO_REG(die1, SBOX_CURRENT_DIE_TEMP0);
    // MIC_READ_MMIO_REG(die2, SBOX_CURRENT_DIE_TEMP1);
    // MIC_READ_MMIO_REG(die3, SBOX_CURRENT_DIE_TEMP2);
    MIC_READ_MMIO_REG(fsc, SBOX_STATUS_FAN2);
    
    temp_data.temps.tfin = GET_BITS(8, 0, btr1.value);
    temp_data.temps.tvccp = GET_BITS(24, 16, btr1.value);
    temp_data.temps.tgddr = GET_BITS(8, 0, btr2.value);
    temp_data.temps.tvddq = GET_BITS(24, 16, btr2.value);
    temp_data.temps.tvddg = GET_BITS(19, 12, fsc.value);
    temp_data.temps.tdie = GET_BITS(30, 22, tsta.value);
    temp_data.temps.tfout = 0;
#endif
    {
        uint32_t die1;
        die1 = mr_sbox_rl(0, SBOX_CURRENT_DIE_TEMP0);
        temp_data.temps.tdie0 = GET_BITS(9, 0, die1);
    }

    lastdie = die;

    return temp_data.value;
}

static int temp_scnprintf(char *buf, int limit, uint64_t data) {
    char *ptr = buf;
    int ret;
    union temp_union temp;
    temp.value = data;

#define PRINT_TEMP(t) do { \
    ret = scnprintf(ptr, limit, "\"%s\":%u,", #t, (unsigned) temp.temps.t); \
    CHECK_RET(ret); \
    ptr += ret; \
    limit -= ret; \
} while (0)
    PRINT_TEMP(tfin);
    PRINT_TEMP(tvccp);
    PRINT_TEMP(tgddr);
    PRINT_TEMP(tvddq);
    PRINT_TEMP(tvddg);
    PRINT_TEMP(tdie);
    PRINT_TEMP(tfout);
    PRINT_TEMP(tdie0);
#undef PRINT_TEMP
    
    return ptr - buf;
}

// fan
static uint64_t fan_restart(struct micstat_counter *cnt) {
    uint32_t fan_rd;
    uint64_t ret;
    mr_smc_rd(MR_SMC_FAN_TACH, &fan_rd);
    ret = GET_BITS(15, 0, fan_rd);
    return ret;
}

// power
union power_union {
    uint64_t value;
    struct {
        uint16_t avgpwr;
        uint8_t pciepwr;
        uint8_t c2x3pwr;
        uint8_t c2x4pwr;
        uint8_t vccppwr;
        uint8_t vddgpwr;
        uint8_t vddqpwr;
    } powers;
};
static union power_union power_data;
STATIC_ASSERT(sizeof(power_data) == 8)

static uint64_t power_restart(struct micstat_counter *cnt) {
    uint32_t vccp, vddg, vddq;
    uint32_t /* prd0,*/  prd1;
    uint32_t pcie, p2x3, p2x4;

    mr_smc_rd(MR_SMC_PWR_VCCP, &vccp);
    mr_smc_rd(MR_SMC_PWR_VDDG, &vddg);
    mr_smc_rd(MR_SMC_PWR_VDDQ, &vddq);
    power_data.powers.vccppwr = GET_BITS(15, 0, vccp);
    power_data.powers.vddgpwr = GET_BITS(15, 0, vddg);
    power_data.powers.vddqpwr = GET_BITS(15, 0, vddq);

    mr_smc_rd(MR_SMC_AVG_PWR_1, &prd1);
    mr_smc_rd(MR_SMC_PWR_PCIE, &pcie);
    mr_smc_rd(MR_SMC_PWR_2X3, &p2x3);
    mr_smc_rd(MR_SMC_PWR_2X4, &p2x4);
    power_data.powers.avgpwr = GET_BITS(29, 0, prd1);
    power_data.powers.pciepwr = GET_BITS(15, 0, pcie);
    power_data.powers.c2x3pwr = GET_BITS(15, 0, p2x3);
    power_data.powers.c2x4pwr = GET_BITS(15, 0, p2x4);

    return power_data.value;
}

static int power_scnprintf(char *buf, int limit, uint64_t data) {
    char *ptr = buf;
    int ret;
    union power_union power;
    power.value = data;

#define PRINT_POWER(t) do { \
    ret = scnprintf(ptr, limit, "\"%s\":%u,", #t, (unsigned) power.powers.t); \
    CHECK_RET(ret); \
    ptr += ret; \
    limit -= ret; \
} while (0)
    PRINT_POWER(avgpwr);
    PRINT_POWER(pciepwr);
    PRINT_POWER(c2x3pwr);
    PRINT_POWER(c2x4pwr);
    PRINT_POWER(vccppwr);
    PRINT_POWER(vddgpwr);
    PRINT_POWER(vddqpwr);
#undef PRINT_POWER
    
    return ptr - buf;

}

// volt
static uint64_t volt_restart(struct micstat_counter *cnt) {
    uint32_t smc;;
    mr_smc_rd(MR_SMC_VOLT_VCCP, &smc);
    return GET_BITS(15, 0, smc) * 1000;
}

// freq
static uint64_t freq_restart(struct micstat_counter *cnt) {
    sboxCurrentratioReg clock_ratio;
    MIC_READ_MMIO_REG(clock_ratio, SBOX_CURRENTRATIO);
    return (freq_from_code
            (pll_data_k1om.bclk,
             K1OM_MCLK_2_FREQ(clock_ratio.bits.mclkratio),
             pll_data_k1om.ffpos));
}

// sample tracer: ts, intv, and oh.
static uint64_t st_prev;
static uint64_t st_current;
static uint64_t st_oh;
static uint64_t get_time(void) {
    static struct timespec ts;
    do_posix_clock_monotonic_gettime(&ts);
    return timespec_to_ns(&ts);
}
static uint64_t ts_restart(struct micstat_counter *cnt) {
    st_prev = st_current;
    st_current = get_time();
    return st_current;
}
static uint64_t intv_restart(struct micstat_counter *cnt) {
    return st_current - st_prev;
}
static uint64_t oh_restart(struct micstat_counter *cnt) {
    st_oh = get_time();
    return st_oh - st_current;
}
static void ts_reset(struct micstat_counter *cnt) {
    st_prev = st_current = st_oh = get_time();
}

// jif
#ifdef MICSTAT_JIF
static uint64_t jif;
static uint64_t jif_restart(struct micstat_counter *cnt) {
    uint64_t prev = jif;
    jif = jiffies_64;
    return jif - prev;
}
static void jif_reset(struct micstat_counter *cnt) {
    jif = jiffies_64;
}
#endif

// cycles
#ifdef MICSTAT_CYCLES
static uint64_t cycles_data;
static uint64_t cycles_restart(struct micstat_counter *cnt) {
    uint64_t prev = cycles_data;
    cycles_data = local_clock();
    return cycles_data - prev;
}

static void cycles_reset(struct micstat_counter *cnt) {
    cycles_data = local_clock();
}
#endif

// perf
struct perf_event_value {
    uint64_t total;
    uint64_t enabled;
    uint64_t running;
};

struct perf_event_config {
    __u32       type;
    __u32       config;
    struct perf_event *events[NR_CPUS_MIC];
    struct perf_event_value values[NR_CPUS_MIC];
};

static void overflow_handler(struct perf_event *event, int nmi,
                             struct perf_sample_data *data, struct pt_regs *regs) {
    printk(KERN_INFO "micstat: overflow_handler triggerd.\n");
}

static int perf_init(struct micstat_counter *cnt) {
    int i;
    struct perf_event_attr pe_attr;
    struct perf_event *event;
    struct perf_event_config *cfg = (struct perf_event_config *) cnt->data;
    struct perf_event **events = cfg->events;
    struct perf_event_value *values = cfg->values;

    memset(&pe_attr, 0, sizeof(pe_attr));
    pe_attr.type = cfg->type;
    pe_attr.size = sizeof(pe_attr);
    pe_attr.config = cfg->config;
    pe_attr.sample_period = 0;

    for (i = 0; i < nr_cpu_ids; i++) {
        events[i] = NULL;
        values[i].total = 0;
        values[i].running = 0;
        values[i].enabled = 0;
    }
    for (i = 0; i < nr_cpu_ids; i++) {
        event = perf_event_create_kernel_counter(&pe_attr, i, NULL, overflow_handler);
        if (IS_ERR(event) || event == NULL) {
            printk("micstat: error [code:%lld] on creating perf_event[%d:%x] at cpu %d\n",
                    (int64_t) event, cfg->type, cfg->config, i);
            return -1;
        }
        events[i] = event;
    }
    return 0;
}

static void perf_exit(struct micstat_counter *cnt) {
    struct perf_event_config *cfg = (struct perf_event_config *) cnt->data;
    struct perf_event **events = cfg->events;
    int i;
    for (i = 0; i < nr_cpu_ids; i++) {
        if (events[i]) {
            perf_event_release_kernel(events[i]);
            events[i] = NULL;
        }
    }
}

static uint64_t const perf_overflow_threshold = 2000000000000ULL; // 2T

static uint64_t perf_restart(struct micstat_counter *cnt) {
    struct perf_event_config *cfg = (struct perf_event_config *) cnt->data;
    struct perf_event **events = cfg->events;
    struct perf_event_value *values = cfg->values;
    u64 enabled, running;
    uint64_t total = 0;
    int i;
    uint64_t ret;
    uint64_t tmp;
    for (i = 0; i < nr_cpu_ids; i++) {
        if (events[i]) {
            ret = perf_event_read_value(events[i], &enabled, &running);
            if (ret < values[i].total || enabled < values[i].enabled || running <= values[i].running) {
#ifdef MICSTAT_DEBUG_PERF_OVERFLOW
                printk(KERN_INFO "micstat [%s] overflow at cpu %d.\n"
                       "micstat total %llu -> %llu, enabled %llu -> %llu, running %llu -> %llu.\n",
                       cnt->name, i, values[i].total, ret, values[i].enabled, enabled, values[i].running, running);
#endif
            } else {
                tmp = (uint64_t) ((double) (ret - values[i].total) * (enabled - values[i].enabled) / (running - values[i].running) + 0.5);
                if (tmp > perf_overflow_threshold) {
#ifdef MICSTAT_DEBUG_PERF_OVERFLOW
                    printk("micstat [%s] reading %llu at cpu %i may overflows. ignored.\n"
                           "micstat total %llu -> %llu, enabled %llu -> %llu, running %llu -> %llu.\n",
                           cnt->name, tmp, i, values[i].total, ret, values[i].enabled, enabled, values[i].running, running);
#endif
                } else {
                    total += tmp;
                }
            }
            values[i].total = ret;
            values[i].enabled = enabled;
            values[i].running = running;
        }
    }

#ifdef MICSTAT_DEBUG_PERF_OVERFLOW
    if (total > perf_overflow_threshold) {
        printk("micstat [%s] total(%llu) may overflows.\n", cnt->name, total);
    }
#endif

    return total;
}

static void perf_reset(struct micstat_counter *cnt) {
    // Do not support perf_reset for now.
}

#define PERF_RAW_CONFIG(name, code) static struct perf_event_config perf_##name##_data = { .type = PERF_TYPE_RAW, .config = code }
PERF_RAW_CONFIG(inst, 0x0016);
PERF_RAW_CONFIG(instv, 0x0017); // instruction on vpipe
PERF_RAW_CONFIG(fp, 0x2016);
PERF_RAW_CONFIG(fpv, 0x2017); // v: V-pipe
PERF_RAW_CONFIG(fpa, 0x2018); // a: Active
PERF_RAW_CONFIG(brm, 0x002b);
PERF_RAW_CONFIG(cyc, 0x002a);
PERF_RAW_CONFIG(mcyc, 0x002c); // microcode cycle
PERF_RAW_CONFIG(fes, 0x002d); // FE stall
PERF_RAW_CONFIG(l1d, 0x0028); // l1d access
PERF_RAW_CONFIG(l1dm, 0x0029); // l1d miss
PERF_RAW_CONFIG(l1dr, 0x0000); // l1d read
PERF_RAW_CONFIG(l1dw, 0x0001); // l1d write
PERF_RAW_CONFIG(l1drm, 0x0003); // l1d read miss
PERF_RAW_CONFIG(l1dwm, 0x0004); // l1d write miss
PERF_RAW_CONFIG(l1ir, 0x000c); // l1i read
PERF_RAW_CONFIG(l1im, 0x000e); // l1i miss
PERF_RAW_CONFIG(l2rm, 0x10cb); // l2 read miss
PERF_RAW_CONFIG(fps, 0x2005); // fpu stall

// counters
// counters[0] must be ts(timestamp), counters[1] must be intv(interval), and last element must be oh(overhead)
struct micstat_counter counters[] = {
    __MIC_COUNTER(ts, NULL, ts_restart, ts_reset),
    __MIC_COUNTER(intv, NULL, intv_restart, noop_reset),
#ifdef MICSTAT_JIF
    __MIC_COUNTER(jif, NULL, jif_restart, jif_reset),
#endif
#ifdef MICSTAT_CYCLES
    __MIC_COUNTER(cycles, NULL, cycles_restart, cycles_reset),
#endif
    __MIC_COUNTER(freq, NULL, freq_restart, noop_reset),
    __MIC_COUNTER(temp, temp_scnprintf, temp_restart, noop_reset),
    __MIC_COUNTER(temp2, temp2_scnprintf, temp2_restart, noop_reset),
    __MIC_COUNTER(power, power_scnprintf, power_restart, noop_reset),
//  __MIC_COUNTER(fan, NULL, fan_restart, noop_reset),
    __MIC_COUNTER(volt, NULL, volt_restart, noop_reset),
    __PERF_COUNTER(cyc),
    __PERF_COUNTER(inst),
    __PERF_COUNTER(instv),
    __PERF_COUNTER(fp),
    __PERF_COUNTER(fpv),
    __PERF_COUNTER(fpa),
    __PERF_COUNTER(brm),
    __PERF_COUNTER(l1dr),
    __PERF_COUNTER(l1dw),
    __PERF_COUNTER(l1dm),
    __PERF_COUNTER(l1im),
    __PERF_COUNTER(l2rm),
    __PERF_COUNTER(mcyc),
    __PERF_COUNTER(fes),
    __PERF_COUNTER(fps),
    __MIC_COUNTER(oh, NULL, oh_restart, noop_reset),
};

#define MICSTAT_NCNT (sizeof(counters) / sizeof(counters[0]))
#define MICSTAT_NBUF 128

static uint64_t buffer[MICSTAT_NBUF][MICSTAT_NCNT];
static int buffer_base;
static int buffer_next;
static int buffer_size;
static spinlock_t buffer_lock;
static uint64_t working_buf[MICSTAT_NCNT];

static int roll_buffer(void) {
    int i;
    for (i = 0; i < MICSTAT_NCNT; i++) {
        working_buf[i] = counters[i].restart(&counters[i]);
    }
    spin_lock_bh(&buffer_lock);
    for (i = 0; i < MICSTAT_NCNT; ++i) {
        buffer[buffer_next][i] = working_buf[i];
    }
    buffer_next = (buffer_next + 1) % MICSTAT_NBUF;
    if (buffer_size < MICSTAT_NBUF ){
        buffer_size ++;
    } else {
        buffer_base = (buffer_base + 1) % MICSTAT_NBUF;
    }
    spin_unlock_bh(&buffer_lock);
    return -1;
}

static int print_buffer(char *charbuf, int limit, uint64_t *buf) {
    char *ptr = charbuf;
    int ret;
    int i;

    ret = scnprintf(ptr, limit, "{");
    CHECK_RET(ret);
    ptr += ret;
    limit -= ret;

    for (i = 0; i < MICSTAT_NCNT; ++i) {
        if (counters[i].scnprintf != NULL) {
            ret = counters[i].scnprintf(ptr, limit, buf[i]);
            CHECK_RET(ret);
            ptr += ret;
            limit -= ret;
        } else {
            ret = scnprintf(ptr, limit, "\"%s\":%llu,", counters[i].name, buf[i]);
            CHECK_RET(ret);
            ptr += ret;
            limit -= ret;
        }
    }

    ret = scnprintf(ptr, limit, "}\n");
    CHECK_RET(ret);
    ptr += ret;
    limit -= ret;

    return  ptr - charbuf;
}

#if defined(USE_KTHREAD)
static struct task_struct *kthread_task;
static int kthread_function(void *data) {
    unsigned int oh;
    unsigned int tosleep;
    while (on) {
        roll_buffer();
        oh = (st_oh - st_current) / 1000000;
        tosleep = period - oh;
        if (period < oh * 5) {
            // force overhead / period < 20%
            tosleep = oh * 4;
        }
        msleep(tosleep);
        if (kthread_should_stop()) return 0;
    }
    return 0;
}
static void timer_start(void) {
    struct sched_param param = { .sched_priority = 1 };
    kthread_task = kthread_create(kthread_function, (void*) NULL, "micstat");
#ifdef MICSTAT_BIND_KTHREAD
    kthread_bind(kthread_task, 0);
#endif
    sched_setscheduler(kthread_task, SCHED_RR, &param);
    if (!IS_ERR(kthread_task))
        wake_up_process(kthread_task);
}
static void timer_cleanup(void) {
    kthread_stop(kthread_task);
}
#elif defined(USE_HTIMER)
static struct hrtimer htimer;
static ktime_t kt_periode;
static enum hrtimer_restart htimer_function(struct hrtimer *unused) {
    if (on) {
        roll_buffer();
        hrtimer_forward_now(&htimer, kt_periode);
        return HRTIMER_RESTART;
    } else {
        return HRTIMER_NORESTART;
    }
}
static void timer_start(void) {
    kt_periode = ktime_set(1, 104167);
    hrtimer_start(&htimer, CLOCK_REALTIME, HRTIMER_MODE_REL);
    htimer.function = timer_function;
    hrtimer_start(&htimer, kt_periode, HRTIMER_MODE_REL);
}
static void timer_cleanup(void) {
    hrtimer_cancel(&htimer);
}
#
#else   // USE_TIMER
static struct timer_list timer;
static void timer_function(unsigned long data) {
    if (on) {
        roll_buffer();
        mod_timer(&timer, jiffies + msecs_to_jiffies(1000));
    }
}
static void timer_start(void) {
    setup_timer(&timer, timer_function, 0);
    mod_timer(&timer, jiffies + msecs_to_jiffies(1000));
}
static void timer_cleanup(void) {
    del_timer(&timer);
}
#endif

static void set_on(void) {
    if (!on) {
        int i;
        for (i = 0; i < MICSTAT_NCNT; i++) {
            counters[i].reset(&counters[i]);
        }
        on = true;
        timer_start();
    }
}

static void set_off(void) {
    if (on) {
        timer_cleanup();
        on = false;
    }
}

static ssize_t micstat_show_period(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    return sprintf(buf, "%u\n", period);
}

static ssize_t micstat_store_period(
        struct class *class,
        struct class_attribute *attr,
        const char *buf,
        size_t count) {
    unsigned long tmp;
    int ret;
    ret = kstrtoul(buf, 0, &tmp);
    if (ret == 0 && tmp > 100 && tmp < 10000) {
        period = tmp;
    }
    return count;
}

static ssize_t micstat_show_clear(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    return 0;
}

static ssize_t micstat_store_clear(
        struct class *class,
        struct class_attribute *attr,
        const char *buf,
        size_t count) {
    if ((count >= 2 && strncmp(buf, "on", 2) == 0) || 
        (count >= 1 && strncmp(buf, "1", 1) == 0)) {
        spin_lock_bh(&buffer_lock);
        buffer_size = 0;
        buffer_next = buffer_base;
        spin_unlock_bh(&buffer_lock);
        return count;
    }
}


static ssize_t micstat_show_ctrl(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    if (on) {
        return sprintf(buf, "on\n");
    } else {
        return sprintf(buf, "off\n");
    }
}

static ssize_t micstat_store_ctrl(
        struct class *class,
        struct class_attribute *attr,
        const char *buf,
        size_t count) {
    if (count >= 2 && strncmp(buf, "on", 2) == 0) {
        set_on();
        return count;
    }
    if (count >= 3 && strncmp(buf, "off", 3) == 0) {
        set_off();
        on = false;
        return count;
    }
    return count;
}

static ssize_t micstat_show_stat(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    // WARN: will fail when one data point is larger than 4Kb
    int limit = PAGE_SIZE;
    int ret;
    int count = 0;
    char *ptr = buf;

    // WARN: NOT thread safe. lock on buffer!
    spin_lock_bh(&buffer_lock);
    while (limit > (PAGE_SIZE / 2) && buffer_size > 0) {
        ret = print_buffer(ptr, limit, buffer[buffer_base]);
        if (ret < 0) {
            break;
        }
        count += ret;
        ptr += ret;
        limit -= ret;
        buffer_base = (buffer_base + 1) % MICSTAT_NBUF;
        buffer_size -= 1;
    }
    spin_unlock_bh(&buffer_lock);
    return count;
}

static ssize_t micstat_show_last(
        struct class *class,
        struct class_attribute *attr,
        char *buf) {
    int ret = 0;
    int buffer_last;
    char *ptr = buf;
    spin_lock_bh(&buffer_lock);
    if (buffer_size > 0) {
        buffer_last = (buffer_base + buffer_size - 1) % MICSTAT_NBUF;
        ret = print_buffer(ptr, PAGE_SIZE, buffer[buffer_last]);
    }
    spin_unlock_bh(&buffer_lock);
    return ret;
}

static struct class_attribute micstat_attr[] = {
    __ATTR(ctrl,    0666, micstat_show_ctrl, micstat_store_ctrl),
    __ATTR(period,  0666, micstat_show_period, micstat_store_period),
    __ATTR(clear,   0666, micstat_show_clear, micstat_store_clear),
    __ATTR(stat,    0444, micstat_show_stat, 0),
    __ATTR(last,    0444, micstat_show_last, 0),
    __ATTR_NULL,
};

static struct class micstat_class = {
    .name = "micstat",
    .owner = THIS_MODULE,
    .class_attrs = micstat_attr,
};

static int __init micstat_init(void) {
    int baseclk;
    int err = 0;
    int i;

    lastdie = 0;

    printk(KERN_INFO "micstat_init\n");

    on = false;
    period = 1000;      // default sampling rate at 1Hz

    // init counters
    for (i = 0; i < MICSTAT_NCNT; i++) {
        if (counters[i].init) {
            err = counters[i].init(&counters[i]);
            if (err != 0) {
                goto fail_cnt_init;
            }
        }
    }

    buffer_base = 0;
    buffer_next = 0;
    buffer_size = 0;
    spin_lock_init(&buffer_lock);

    err = class_register(&micstat_class);
    if (err) {
        printk("micstat.init: cannot register class 'micstat', error %d\n", err);
        goto fail_class;
    }

    baseclk = get_baseclk();
    printk("micstat: Base clk for part is %d\n",baseclk);
    if (baseclk) {
        pll_data_k1om.bclk = baseclk;
    }

    return 0;
fail_cnt_init:
    for (; i >= 0; i--) {
        if (counters[i].exit) {
            counters[i].exit(&counters[i]);
        }
    }
fail_class:

    return err;
}

static void __exit micstat_exit(void) {
    int i;
    printk(KERN_INFO "micstat_exit\n");

    timer_cleanup();

    for (i = 0; i < MICSTAT_NCNT; i++) {
        if (counters[i].exit) {
            counters[i].exit(&counters[i]);
        }
    }
}

module_init(micstat_init);
module_exit(micstat_exit);
