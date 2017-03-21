/*
 * Copyright (C) 2013 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/cpu.h>
#include <linux/regulator/consumer.h>
//#include <asm/system.h>
#include <trace/events/power.h>

#include <soc/sprd/hardware.h>
#include <soc/sprd/regulator.h>
#include <soc/sprd/adi.h>
#include <soc/sprd/sci.h>
#include <soc/sprd/sci_glb_regs.h>
#include <soc/sprd/arch_misc.h>

#if defined(CONFIG_ARCH_SC8825)
#define MHz                     (1000000)
#define GR_MPLL_REFIN_2M        (2 * MHz)
#define GR_MPLL_REFIN_4M        (4 * MHz)
#define GR_MPLL_REFIN_13M       (13 * MHz)
#define GR_MPLL_REFIN_SHIFT     16
#define GR_MPLL_REFIN_MASK      (0x3)
#define GR_MPLL_N_MASK          (0x7ff)
#define GR_MPLL_MN		(REG_GLB_M_PLL_CTL0)
#define GR_GEN1			(REG_GLB_GEN1)
#endif

#define FREQ_TABLE_SIZE 	15
#define DVFS_BOOT_TIME	(30 * HZ)
#define SHARK_TDPLL_FREQUENCY	(768000)
#define TRANSITION_LATENCY	(100 * 1000) /* ns */

#define MAX_VOLT (1125 * 1000)
#define MIN_VOLT (825 * 1000)

static DEFINE_MUTEX(freq_lock);
struct cpufreq_freqs global_freqs;
unsigned int percpu_target[CONFIG_NR_CPUS] = {0};
static unsigned long boot_done;
static unsigned int sprd_top_frequency; /* khz */

struct cpufreq_conf {
	struct clk 					*clk;
	struct clk 					*mpllclk;
	struct clk 					*tdpllclk;
	struct regulator 				*regulator;
	struct cpufreq_frequency_table			*freq_tbl;
	unsigned long					*vddarm_mv;
};

struct cpufreq_table_data {
	struct cpufreq_frequency_table 		freq_tbl[FREQ_TABLE_SIZE];
	unsigned long				vddarm_mv[FREQ_TABLE_SIZE];
};

struct cpufreq_conf *sprd_cpufreq_conf = NULL;
static struct mutex cpufreq_vddarm_lock;

enum clocking_levels {
#ifdef SPRD_OC
	OC4, OC3, OC2, OC1,
#endif
	NOC, UC1, UC2, UC3, UC4,
	UC5, UC6, UC7, UC8, UC9,
	MIN_CL=UC9,
	EC,
};
static struct cpufreq_table_data sc8830t_cpufreq_table_data_es = {
        .freq_tbl = {
#ifdef SPRD_OC
		{OC4, 1536000},
		{OC3, 1497600},
		{OC2, 1401600},
		{OC1, 1363200},
#endif
		{NOC, 1300000},
		{UC1, 1248000},
		{UC2, 1209600},
		{UC3, 1152000},
		{UC4, 1094400},
		{UC5, 998400},
		{UC6, 800000},
		{UC7, 533333},
		{UC8, 400000},
		{UC9, 200000},
		{EC,  CPUFREQ_TABLE_END},
        },
        .vddarm_mv = {
#ifdef SPRD_OC
		[OC4]  = 1125000,
		[OC3]  = 1100000,
		[OC2]  = 1075000,
		[OC1]  = 1050000,
#endif
		[NOC]  = 1050000,
		[UC1]  = 1025000,
		[UC2]  = 1000000,
		[UC3]  = 975000,
		[UC4]  = 950000,
		[UC5]  = 925000,
		[UC6]  = 900000,
		[UC7]  = 875000,
		[UC8]  = 850000,
		[UC9]  = 825000,
		[EC]   = 825000,
        },
};

struct cpufreq_conf sc8830_cpufreq_conf = {
	.clk = NULL,
	.mpllclk = NULL,
	.tdpllclk = NULL,
	.regulator = NULL,
	.freq_tbl = NULL,
	.vddarm_mv = NULL,
};

int cpufreq_table_thermal_update(unsigned int freq, unsigned int voltage)
{
	struct cpufreq_frequency_table *freq_tbl;
	unsigned int *vddarm;
	int i;

	if (NULL == sprd_cpufreq_conf)
		return -1;
	freq_tbl = sprd_cpufreq_conf->freq_tbl;
	vddarm = sprd_cpufreq_conf->vddarm_mv;
	if (NULL == freq_tbl && NULL == vddarm)
		return -1;

	for (i = 0; freq_tbl[i].frequency != CPUFREQ_TABLE_END; ++i) {
		if (freq_tbl[i].frequency == freq)
			goto done;
	}
	pr_err(KERN_ERR "%s cpufreq %dMHz isn't find!\n", __func__, freq);
	return -1;
done:
	printk(KERN_ERR "%s: %dMHz voltage is %dmV\n",
		__func__, freq, voltage);
	if (vddarm[i] == voltage)
		return 0;

	mutex_lock(&cpufreq_vddarm_lock);
	vddarm[i] = voltage;
	mutex_unlock(&cpufreq_vddarm_lock);

	return 0;
}

static unsigned int sprd_raw_get_cpufreq(void)
{
#if defined(CONFIG_ARCH_SCX35)
	return clk_get_rate(sprd_cpufreq_conf->clk) / 1000;
#elif defined(CONFIG_ARCH_SC8825)
	return get_mcu_clk_freq() / 1000;
#endif
}

static void cpufreq_set_clock(unsigned int freq)
{
	int ret;

	ret = clk_set_parent(sprd_cpufreq_conf->clk, sprd_cpufreq_conf->tdpllclk);
	if (ret)
		pr_err("Failed to set cpu parent to tdpll\n");
	if (freq == SHARK_TDPLL_FREQUENCY/2) {
		//ca7 clk div
		#ifndef CONFIG_ARCH_SCX35L
		sci_glb_set(REG_AP_AHB_CA7_CKG_CFG, BITS_CA7_MCU_CKG_DIV(1));
		#else
		#ifndef CONFIG_ARCH_SCX35LT8	//TODO
		sci_glb_set(REG_AP_AHB_CA7_CKG_DIV_CFG, BITS_CA7_MCU_CKG_DIV(1));
		#endif
		#endif
	} else if (freq == SHARK_TDPLL_FREQUENCY) {
		#ifndef CONFIG_ARCH_SCX35L
		sci_glb_clr(REG_AP_AHB_CA7_CKG_CFG, BITS_CA7_MCU_CKG_DIV(1));
		#else
		#ifndef CONFIG_ARCH_SCX35LT8	//TODO
		sci_glb_clr(REG_AP_AHB_CA7_CKG_DIV_CFG, BITS_CA7_MCU_CKG_DIV(1));
		#endif
		#endif
	} else {
	/*
		if (clk_get_parent(sprd_cpufreq_conf->clk) != sprd_cpufreq_conf->tdpllclk) {
			ret = clk_set_parent(sprd_cpufreq_conf->clk, sprd_cpufreq_conf->tdpllclk);
			if (ret)
				pr_err("Failed to set cpu parent to tdpll\n");
		}
		*/
		if (!(sci_glb_read(REG_PMU_APB_MPLL_REL_CFG, -1) & BIT_MPLL_AP_SEL)) {
			sci_glb_set(REG_PMU_APB_MPLL_REL_CFG, BIT_MPLL_AP_SEL);
			udelay(500);
		}
		ret = clk_set_rate(sprd_cpufreq_conf->mpllclk, (freq * 1000));
		if (ret)
			pr_err("Failed to set mpll rate\n");
		ret = clk_set_parent(sprd_cpufreq_conf->clk, sprd_cpufreq_conf->mpllclk);
		if (ret)
			pr_err("Failed to set cpu parent to mpll\n");
		#ifndef CONFIG_ARCH_SCX35L
		sci_glb_clr(REG_AP_AHB_CA7_CKG_CFG, BITS_CA7_MCU_CKG_DIV(1));
		#else
		#ifndef CONFIG_ARCH_SCX35LT8	//TODO
		sci_glb_clr(REG_AP_AHB_CA7_CKG_DIV_CFG, BITS_CA7_MCU_CKG_DIV(1));
		#endif
		#endif
	}
}
static void sprd_raw_set_cpufreq(int cpu, struct cpufreq_freqs *freq, int index)
{
#if defined(CONFIG_ARCH_SCX35)
	int ret;

#define CPUFREQ_SET_VOLTAGE() \
	do { \
		mutex_lock(&cpufreq_vddarm_lock);       \
	    ret = regulator_set_voltage(sprd_cpufreq_conf->regulator, \
			sprd_cpufreq_conf->vddarm_mv[index], \
			sprd_cpufreq_conf->vddarm_mv[index]); \
			mutex_unlock(&cpufreq_vddarm_lock);	\
		if (ret) \
			pr_err("Failed to set vdd to %d mv\n", \
				sprd_cpufreq_conf->vddarm_mv[index]); \
	} while (0)
#define CPUFREQ_SET_CLOCK() \
	do { \
		if (freq->new == SHARK_TDPLL_FREQUENCY) { \
			ret = clk_set_parent(sprd_cpufreq_conf->clk, sprd_cpufreq_conf->tdpllclk); \
			if (ret) \
				pr_err("Failed to set cpu parent to tdpll\n"); \
		} else { \
			if (clk_get_parent(sprd_cpufreq_conf->clk) != sprd_cpufreq_conf->tdpllclk) { \
				ret = clk_set_parent(sprd_cpufreq_conf->clk, sprd_cpufreq_conf->tdpllclk); \
				if (ret) \
					pr_err("Failed to set cpu parent to tdpll\n"); \
			} \
			ret = clk_set_rate(sprd_cpufreq_conf->mpllclk, (freq->new * 1000)); \
			if (ret) \
				pr_err("Failed to set mpll rate\n"); \
			ret = clk_set_parent(sprd_cpufreq_conf->clk, sprd_cpufreq_conf->mpllclk); \
			if (ret) \
				pr_err("Failed to set cpu parent to mpll\n"); \
		} \
	} while (0)
	trace_cpu_frequency(freq->new, cpu);

	if (freq->new >= sprd_raw_get_cpufreq()) {
		CPUFREQ_SET_VOLTAGE();
		cpufreq_set_clock(freq->new);
	} else {
		cpufreq_set_clock(freq->new);
		CPUFREQ_SET_VOLTAGE();
	}

	pr_info("%u --> %u, real=%u, index=%d\n",
		freq->old, freq->new, sprd_raw_get_cpufreq(), index);

#undef CPUFREQ_SET_VOLTAGE
#undef CPUFREQ_SET_CLOCK

#elif defined(CONFIG_ARCH_SC8825)
	set_mcu_clk_freq(freq->new * 1000);
#endif
	return;
}

static void sprd_real_set_cpufreq(struct cpufreq_policy *policy, unsigned int new_speed, int index)
{
	mutex_lock(&freq_lock);

	if (global_freqs.old == new_speed) {
		pr_debug("do nothing for cpu%u, new=old=%u\n",
				policy->cpu, new_speed);
		mutex_unlock(&freq_lock);
		return;
	}
	pr_info("--xing-- set %u khz for cpu%u\n",
		new_speed, policy->cpu);
	global_freqs.cpu = policy->cpu;
	global_freqs.new = new_speed;

	cpufreq_notify_transition(policy, &global_freqs, CPUFREQ_PRECHANGE);

	sprd_raw_set_cpufreq(policy->cpu, &global_freqs, index);

	cpufreq_notify_transition(policy, &global_freqs, CPUFREQ_POSTCHANGE);

	global_freqs.old = global_freqs.new;

	mutex_unlock(&freq_lock);
	return;
}

static void sprd_find_real_index(unsigned int new_speed, int *index)
{
	int i;
	struct cpufreq_frequency_table *pfreq = sprd_cpufreq_conf->freq_tbl;

	*index = pfreq[0].index;
	for (i = 0; (pfreq[i].frequency != CPUFREQ_TABLE_END); i++) {
		if (new_speed == pfreq[i].frequency) {
			*index = pfreq[i].index;
			break;
		}
	}
	return;
}

static int sprd_update_cpu_speed(struct cpufreq_policy *policy,
	unsigned int target_speed, int index)
{
	int i, real_index = 0;
	unsigned int new_speed = 0;

	/*
	 * CONFIG_NR_CPUS cores are always in the same voltage, at the same
	 * frequency. But, cpu load is calculated individual in each cores,
	 * So we remeber the original target frequency and voltage of core0,
	 * and use the higher one
	 */

	for_each_online_cpu(i) {
		new_speed = max(new_speed, percpu_target[i]);
	}

	if (new_speed > sprd_top_frequency)
		new_speed = sprd_top_frequency;

	if (new_speed != sprd_cpufreq_conf->freq_tbl[index].frequency)
		sprd_find_real_index(new_speed, &real_index);
	else
		real_index = index;
	sprd_real_set_cpufreq(policy, new_speed, real_index);
	return 0;
}

static int sprd_cpufreq_verify_speed(struct cpufreq_policy *policy)
{
	if (policy->cpu > CONFIG_NR_CPUS) {
		pr_err("%s no such cpu id %d\n", __func__, policy->cpu);
		return -EINVAL;
	}

	return cpufreq_frequency_table_verify(policy, sprd_cpufreq_conf->freq_tbl);
}

unsigned int cpufreq_min_limit = 200000;
unsigned int cpufreq_max_limit = 1300000;
unsigned int dvfs_score_select = 5;
unsigned int dvfs_unplug_select = 2;
unsigned int dvfs_plug_select = 0;
unsigned int dvfs_score_hi[4] = {0};
unsigned int dvfs_score_mid[4] = {0};
unsigned int dvfs_score_critical[4] = {0};
extern unsigned int percpu_load[4];
extern unsigned int cur_window_size[4];
extern unsigned int cur_window_index[4];
extern unsigned int ga_percpu_total_load[4][8];

static DEFINE_SPINLOCK(cpufreq_state_lock);

static int sprd_cpufreq_target(struct cpufreq_policy *policy,
		       unsigned int target_freq,
		       unsigned int relation)
{
	int ret = -EFAULT;
	int index;
	unsigned int new_speed;
	struct cpufreq_frequency_table *table;
	int max_freq = cpufreq_max_limit;
	int min_freq = cpufreq_min_limit;

	/* delay 30s to enable dvfs&dynamic-hotplug,
         * except requirment from thermal-cooling device
         */
	if(time_before(jiffies, boot_done)){
		return 0;
	}

	if((target_freq < min_freq) || (target_freq > max_freq))
	{
		pr_err("invalid target_freq: %d min_freq %d max_freq %d\n", target_freq,min_freq,max_freq);
		return -EINVAL;
	}
	table = cpufreq_frequency_get_table(policy->cpu);

	if (cpufreq_frequency_table_target(policy, table,
					target_freq, relation, &index)) {
		pr_err("invalid target_freq: %d\n", target_freq);
		return -EINVAL;
	}

	pr_debug("CPU_%d target %d relation %d (%d-%d) selected %d\n",
			policy->cpu, target_freq, relation,
			policy->min, policy->max, table[index].frequency);

	new_speed = table[index].frequency;

	percpu_target[policy->cpu] = new_speed;
	pr_debug("%s cpu:%d new_speed:%u on cpu%d\n",
			__func__, policy->cpu, new_speed, smp_processor_id());

	ret = sprd_update_cpu_speed(policy, new_speed, index);

	return ret;

}

static unsigned int sprd_cpufreq_getspeed(unsigned int cpu)
{
	if (cpu > CONFIG_NR_CPUS) {
		pr_err("%s no such cpu id %d\n", __func__, cpu);
		return -EINVAL;
	}

	return sprd_raw_get_cpufreq();
}

static void sprd_set_cpufreq_limit(void)
{
	cpufreq_min_limit = sprd_cpufreq_conf->freq_tbl[MIN_CL].frequency;
#ifdef SPRD_OC
	cpufreq_max_limit = sprd_cpufreq_conf->freq_tbl[OC4].frequency;
#else
	cpufreq_max_limit = sprd_cpufreq_conf->freq_tbl[NOC].frequency;
#endif
	pr_info("--xing-- %s max=%u min=%u\n", __func__, cpufreq_max_limit, cpufreq_min_limit);
}

#if defined(CONFIG_ARCH_SCX35LT8)
#define AON_APB_CHIP_ID		REG_AON_APB_CHIP_ID0
#else
#define AON_APB_CHIP_ID		REG_AON_APB_CHIP_ID
#endif
static int sprd_freq_table_init(void)
{
	/* Instantly initialize frequency table, no need detecting - koquantam */
	sprd_cpufreq_conf->freq_tbl = sc8830t_cpufreq_table_data_es.freq_tbl;
	sprd_cpufreq_conf->vddarm_mv = sc8830t_cpufreq_table_data_es.vddarm_mv;
	pr_info("sprd_freq_table_init \n");
	sprd_set_cpufreq_limit();
	return 0;
}

static int sprd_cpufreq_init(struct cpufreq_policy *policy)
{
	int ret;

	cpufreq_frequency_table_cpuinfo(policy, sprd_cpufreq_conf->freq_tbl);
	policy->cur = sprd_raw_get_cpufreq(); /* current cpu frequency: KHz*/
	 /*
	  * transition_latency 5us is enough now
	  * but sampling too often, unbalance and irregular on each online cpu
	  * so we set 500us here.
	  */
	policy->cpuinfo.transition_latency = TRANSITION_LATENCY;
	policy->shared_type = CPUFREQ_SHARED_TYPE_ALL;

	cpufreq_frequency_table_get_attr(sprd_cpufreq_conf->freq_tbl, policy->cpu);

	percpu_target[policy->cpu] = policy->cur;

	ret = cpufreq_frequency_table_cpuinfo(policy, sprd_cpufreq_conf->freq_tbl);
	if (ret != 0)
		pr_err("%s Failed to config freq table: %d\n", __func__, ret);

	pr_info("%s cpu=%d, cur=%u, ret=%d\n",
		__func__, policy->cpu, policy->cur, ret);

	//cpumask_setall(policy->cpus);

	return ret;
}

static int sprd_cpufreq_exit(struct cpufreq_policy *policy)
{
	return 0;
}

static struct freq_attr *sprd_cpufreq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

ssize_t sprd_vdd_get(char *buf) {
	int i, len = 0;

#define freq_table sprd_cpufreq_conf->freq_tbl[i].frequency
#define voltage_table sprd_cpufreq_conf->vddarm_mv[i]

	for (i = 0; i <= MIN_CL; i++) {
		len += sprintf(buf + len, "%umhz: %lu mV\n", freq_table / 1000, voltage_table / 1000);
	}
	return len;
}

void sprd_vdd_set(const char *buf) {
	int ret = -EINVAL;
	int i = 0;
	int j = 0;
	int u[MIN_CL + 1];
	while (j < MIN_CL + 1) {
		int consumed;
		int val;
		ret = sscanf(buf, "%d%n", &val, &consumed);
		if (ret > 0) {
			buf += consumed;
			u[j++] = val;
		}
		else {
			break;
		}
	}

	for (i = 0; i < j; i++) {
		if (u[i] > MAX_VOLT / 1000) {
			u[i] = MAX_VOLT / 1000;
		}
         if( u[i] % 25 == 0 ) {
		 sprd_cpufreq_conf->vddarm_mv[i] = u[i] * 1000; }
	}
   return;
}

static struct vdd_levels_control sprd_vdd_control = {
      .get = sprd_vdd_get,
      .set = sprd_vdd_set,
};

static struct cpufreq_driver sprd_cpufreq_driver = {
	.verify		= sprd_cpufreq_verify_speed,
	.target		= sprd_cpufreq_target,
	.get		= sprd_cpufreq_getspeed,
	.init		= sprd_cpufreq_init,
	.exit		= sprd_cpufreq_exit,
	.name		= "sprd",
	.attr		= sprd_cpufreq_attr,
	.volt_control 	= &sprd_vdd_control,
	.flags		= CPUFREQ_SHARED
};

static ssize_t cpufreq_min_limit_show(struct device *dev, struct device_attribute *attr,char *buf)
{
	memcpy(buf,&cpufreq_min_limit,sizeof(int));
	return sizeof(int);
}

static ssize_t cpufreq_max_limit_show(struct device *dev, struct device_attribute *attr,char *buf)
{
	memcpy(buf,&cpufreq_max_limit,sizeof(int));
	return sizeof(int);
}

static ssize_t cpufreq_min_limit_debug_show(struct device *dev, struct device_attribute *attr,char *buf)
{
	snprintf(buf,10,"%d\n",cpufreq_min_limit);
	return strlen(buf) + 1;
}

static ssize_t cpufreq_max_limit_debug_show(struct device *dev, struct device_attribute *attr,char *buf)
{
	snprintf(buf,10,"%d\n",cpufreq_max_limit);
	return strlen(buf) + 1;
}

static ssize_t cpufreq_min_limit_store(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{
	int ret;
	int value;
	unsigned long irq_flags;

	ret = strict_strtoul(buf,16,(long unsigned int *)&value);

	spin_lock_irqsave(&cpufreq_state_lock, irq_flags);
	/*
	   for debug use
	   echo 0xabcde258 > /sys/power/cpufreq_min_limit means set the minimum limit to 600Mhz
	 */
	if((value & 0xfffff000) == 0xabcde000)
	{
		cpufreq_min_limit = value & 0x00000fff;
		cpufreq_min_limit *= 1000;
		printk(KERN_ERR"cpufreq_min_limit value %s %d\n",buf,cpufreq_min_limit);
	}
	else
	{
		cpufreq_min_limit = *(int *)buf;
	}
	spin_unlock_irqrestore(&cpufreq_state_lock, irq_flags);
	return count;
}

static ssize_t cpufreq_max_limit_store(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{
	int ret;
	int value;
	unsigned long irq_flags;

	ret = strict_strtoul(buf,16,(long unsigned int *)&value);

	spin_lock_irqsave(&cpufreq_state_lock, irq_flags);

	/*
	   for debug use
	   echo 0xabcde4b0 > /sys/power/cpufreq_max_limit means set the maximum limit to 1200Mhz
	 */
	if((value & 0xfffff000) == 0xabcde000)
	{
		cpufreq_max_limit = value & 0x00000fff;
		cpufreq_max_limit *= 1000;
		printk(KERN_ERR"cpufreq_max_limit value %s %d\n",buf,cpufreq_max_limit);
	}
	else
	{
		cpufreq_max_limit = *(int *)buf;
	}
	spin_unlock_irqrestore(&cpufreq_state_lock, irq_flags);

	return count;
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SPRDEMAND
static ssize_t dvfs_score_store(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{
	int ret;
	int value;
	unsigned long irq_flags;

	ret = strict_strtoul(buf,16,(long unsigned int *)&value);

	printk(KERN_ERR"dvfs_score_input %x\n",value);

	dvfs_score_select = (value >> 24) & 0x0f;
	if(dvfs_score_select < 4)
	{
		dvfs_score_critical[dvfs_score_select] = (value >> 16) & 0xff;
		dvfs_score_hi[dvfs_score_select] = (value >> 8) & 0xff;
		dvfs_score_mid[dvfs_score_select] = value & 0xff;
	}


	return count;
}

static ssize_t dvfs_score_show(struct device *dev, struct device_attribute *attr,char *buf)
{
	int ret = 0;

	ret = snprintf(buf + ret,50,"dvfs_score_select %d\n",dvfs_score_select);
	ret += snprintf(buf + ret,200,"dvfs_score_critical[1] = %d dvfs_score_hi[1] = %d dvfs_score_mid[1] = %d\n",dvfs_score_critical[1],dvfs_score_hi[1],dvfs_score_mid[1]);
	ret += snprintf(buf + ret,200,"dvfs_score_critical[2] = %d dvfs_score_hi[2] = %d dvfs_score_mid[2] = %d\n",dvfs_score_critical[2],dvfs_score_hi[2],dvfs_score_mid[2]);
	ret += snprintf(buf + ret,200,"dvfs_score_critical[3] = %d dvfs_score_hi[3] = %d dvfs_score_mid[3] = %d\n",dvfs_score_critical[3],dvfs_score_hi[3],dvfs_score_mid[3]);

	ret += snprintf(buf + ret,200,"percpu_total_load[0] = %d,%d->%d\n",
		percpu_load[0],ga_percpu_total_load[0][(cur_window_index[0] - 1 + 10) % 10],ga_percpu_total_load[0][cur_window_index[0]]);
	ret += snprintf(buf + ret,200,"percpu_total_load[1] = %d,%d->%d\n",
		percpu_load[1],ga_percpu_total_load[1][(cur_window_index[1] - 1 + 10) % 10],ga_percpu_total_load[1][cur_window_index[1]]);
	ret += snprintf(buf + ret,200,"percpu_total_load[2] = %d,%d->%d\n",
		percpu_load[2],ga_percpu_total_load[2][(cur_window_index[2] - 1 + 10) % 10],ga_percpu_total_load[2][cur_window_index[2]]);
	ret += snprintf(buf + ret,200,"percpu_total_load[3] = %d,%d->%d\n",
		percpu_load[3],ga_percpu_total_load[3][(cur_window_index[3] - 1 + 10) % 10],ga_percpu_total_load[3][cur_window_index[3]]);

	return strlen(buf) + 1;
}

static ssize_t dvfs_unplug_store(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{
	int ret;
	int value;
	unsigned long irq_flags;

	ret = strict_strtoul(buf,16,(long unsigned int *)&value);

	printk(KERN_ERR"dvfs_score_input %x\n",value);

	dvfs_unplug_select = (value >> 24) & 0x0f;
	if(dvfs_unplug_select > 7)
	{
		cur_window_size[0]= (value >> 8) & 0xff;
		cur_window_size[1]= (value >> 8) & 0xff;
		cur_window_size[2]= (value >> 8) & 0xff;
		cur_window_size[3]= (value >> 8) & 0xff;
	}
	return count;
}

static ssize_t dvfs_unplug_show(struct device *dev, struct device_attribute *attr,char *buf)
{
	int ret = 0;

	ret = snprintf(buf + ret,50,"dvfs_unplug_select %d\n",dvfs_unplug_select);
	ret += snprintf(buf + ret,100,"cur_window_size[0] = %d\n",cur_window_size[0]);
	ret += snprintf(buf + ret,100,"cur_window_size[1] = %d\n",cur_window_size[1]);
	ret += snprintf(buf + ret,100,"cur_window_size[2] = %d\n",cur_window_size[2]);
	ret += snprintf(buf + ret,100,"cur_window_size[3] = %d\n",cur_window_size[3]);

	return strlen(buf) + 1;
}


static ssize_t dvfs_plug_store(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{
	int ret;
	int value;
	unsigned long irq_flags;

	ret = strict_strtoul(buf,16,(long unsigned int *)&value);

	printk(KERN_ERR"dvfs_plug_select %x\n",value);

	dvfs_plug_select = (value ) & 0x0f;
	return count;
}


static ssize_t dvfs_plug_show(struct device *dev, struct device_attribute *attr,char *buf)
{
	int ret = 0;

	ret = snprintf(buf + ret,50,"dvfs_plug_select %d\n",dvfs_plug_select);

	return strlen(buf) + 1;
}
#endif /* CONFIG_CPU_FREQ_DEFAULT_GOV_SPRDEMAND */

static ssize_t cpufreq_table_show(struct device *dev, struct device_attribute *attr,char *buf)
{
	memcpy(buf,sprd_cpufreq_conf->freq_tbl,sizeof(* sprd_cpufreq_conf->freq_tbl));
	return sizeof(* sprd_cpufreq_conf->freq_tbl);
}

static ssize_t dvfs_prop_store(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{
	int ret;
	int value;

	printk(KERN_ERR"dvfs_status %s\n",buf);
	ret = strict_strtoul(buf,16,(long unsigned int *)&value);

	printk(KERN_ERR"dvfs_plug_select %x\n",value);

	dvfs_plug_select = (value ) & 0x0f;
	return count;
}

static ssize_t dvfs_prop_show(struct device *dev, struct device_attribute *attr,char *buf)
{
	int ret = 0;

	ret = snprintf(buf + ret,50,"dvfs_plug_select %d\n",dvfs_plug_select);

	return strlen(buf) + 1;
}

#ifdef CONFIG_SPRD_AVS_DEBUG
extern unsigned int g_avs_log_flag;

static ssize_t avs_log_store(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{
	int ret;
	int value;
	unsigned long irq_flags;

	printk(KERN_ERR"g_avs_log_flag %s\n",buf);
	ret = strict_strtoul(buf,16,(long unsigned int *)&value);

	printk(KERN_ERR"g_avs_log_flag %x\n",value);

	g_avs_log_flag = (value ) & 0x0f;
	return count;
}

static ssize_t avs_log_show(struct device *dev, struct device_attribute *attr,char *buf)
{
	int ret = 0;

	ret = snprintf(buf + ret,50,"g_avs_log_flag %d\n",g_avs_log_flag);

	return strlen(buf) + 1;
}
#endif
static DEVICE_ATTR(cpufreq_min_limit_sprd, 0660, cpufreq_min_limit_show, cpufreq_min_limit_store);
static DEVICE_ATTR(cpufreq_max_limit_sprd, 0660, cpufreq_max_limit_show, cpufreq_max_limit_store);
static DEVICE_ATTR(cpufreq_min_limit_debug, 0440, cpufreq_min_limit_debug_show, NULL);
static DEVICE_ATTR(cpufreq_max_limit_debug, 0440, cpufreq_max_limit_debug_show, NULL);
static DEVICE_ATTR(cpufreq_table_sprd, 0440, cpufreq_table_show, NULL);

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SPRDEMAND
static DEVICE_ATTR(dvfs_score, 0660, dvfs_score_show, dvfs_score_store);
static DEVICE_ATTR(dvfs_unplug, 0660, dvfs_unplug_show, dvfs_unplug_store);
static DEVICE_ATTR(dvfs_plug, 0660, dvfs_plug_show, dvfs_plug_store);
#endif
static DEVICE_ATTR(dvfs_prop, 0660, dvfs_prop_show, dvfs_prop_store);

#ifdef CONFIG_SPRD_AVS_DEBUG
static DEVICE_ATTR(avs_log, 0660, avs_log_show, avs_log_store);
#endif
static struct attribute *g[] = {
	&dev_attr_cpufreq_min_limit_sprd.attr,
	&dev_attr_cpufreq_max_limit_sprd.attr,
	&dev_attr_cpufreq_min_limit_debug.attr,
	&dev_attr_cpufreq_max_limit_debug.attr,
	&dev_attr_cpufreq_table_sprd.attr,
#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SPRDEMAND
	&dev_attr_dvfs_score.attr,
	&dev_attr_dvfs_unplug.attr,
	&dev_attr_dvfs_plug.attr,
#endif
	&dev_attr_dvfs_prop.attr,
#ifdef CONFIG_SPRD_AVS_DEBUG
	&dev_attr_avs_log.attr,
#endif
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = g,
};

static int sprd_cpufreq_policy_notifier(
	struct notifier_block *nb, unsigned long event, void *data)
{
	return NOTIFY_OK;
}

static struct notifier_block sprd_cpufreq_policy_nb = {
	.notifier_call = sprd_cpufreq_policy_notifier,
};

static int __init sprd_cpufreq_modinit(void)
{
	int ret;
	sprd_cpufreq_conf = &sc8830_cpufreq_conf;

	ret = sprd_freq_table_init();
	if (ret)
		return ret;

	sprd_top_frequency = sprd_cpufreq_conf->freq_tbl[0].frequency;
	/* TODO:need verify for the initialization of limited max freq */

	sprd_cpufreq_conf->clk = clk_get_sys(NULL, "clk_mcu");
	if (IS_ERR(sprd_cpufreq_conf->clk))
		return PTR_ERR(sprd_cpufreq_conf->clk);

	sprd_cpufreq_conf->mpllclk = clk_get_sys(NULL, "clk_mpll");
	if (IS_ERR(sprd_cpufreq_conf->mpllclk))
		return PTR_ERR(sprd_cpufreq_conf->mpllclk);

	sprd_cpufreq_conf->tdpllclk = clk_get_sys(NULL, "clk_tdpll");
	if (IS_ERR(sprd_cpufreq_conf->tdpllclk))
		return PTR_ERR(sprd_cpufreq_conf->tdpllclk);
	mutex_init(&cpufreq_vddarm_lock);

	sprd_cpufreq_conf->regulator = regulator_get(NULL, "vddarm");

	if (IS_ERR(sprd_cpufreq_conf->regulator))
		return PTR_ERR(sprd_cpufreq_conf->regulator);

	/* set max voltage first */
	/*
	regulator_set_voltage(sprd_cpufreq_conf->regulator,
		sprd_cpufreq_conf->vddarm_mv[0],
		sprd_cpufreq_conf->vddarm_mv[0]);
	*/
	clk_set_parent(sprd_cpufreq_conf->clk, sprd_cpufreq_conf->tdpllclk);
	/*
	* clk_set_rate(sprd_cpufreq_conf->mpllclk, (sprd_top_frequency * 1000));
	*/
	clk_set_parent(sprd_cpufreq_conf->clk, sprd_cpufreq_conf->mpllclk);
	global_freqs.old = sprd_raw_get_cpufreq();

	boot_done = jiffies + DVFS_BOOT_TIME;
	ret = cpufreq_register_notifier(
		&sprd_cpufreq_policy_nb, CPUFREQ_POLICY_NOTIFIER);
	if (ret)
		return ret;

	ret = cpufreq_register_driver(&sprd_cpufreq_driver);

	ret = sysfs_create_group(power_kobj, &attr_group);
	return ret;
}

static void __exit sprd_cpufreq_modexit(void)
{
	if (!IS_ERR_OR_NULL(sprd_cpufreq_conf->regulator))
		regulator_put(sprd_cpufreq_conf->regulator);

	cpufreq_unregister_driver(&sprd_cpufreq_driver);
	cpufreq_unregister_notifier(
		&sprd_cpufreq_policy_nb, CPUFREQ_POLICY_NOTIFIER);
	return;
}

module_init(sprd_cpufreq_modinit);
module_exit(sprd_cpufreq_modexit);

MODULE_DESCRIPTION("cpufreq driver for Spreadtrum");
MODULE_LICENSE("GPL");
