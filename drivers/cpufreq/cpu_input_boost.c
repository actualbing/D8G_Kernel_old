/*
 * Copyright (C) 2018, Sultan Alsawaf <sultanxda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "cpu_input_boost: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/msm_drm_notify.h>
#include <linux/input.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/cpuset.h>

static unsigned int input_boost_freq_lp = CONFIG_INPUT_BOOST_FREQ_LP;
static unsigned int input_boost_freq_hp = CONFIG_INPUT_BOOST_FREQ_PERF;
static unsigned short input_boost_duration = CONFIG_INPUT_BOOST_DURATION_MS;

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
static unsigned short dynamic_stune_boost_duration = CONFIG_INPUT_BOOST_DURATION_MS;
static bool stune_boost_active;
static int boost_slot;
static unsigned short dynamic_stune_boost;
module_param(dynamic_stune_boost, short, 0644);
module_param(dynamic_stune_boost_duration, short, 0644);
#endif

module_param(input_boost_freq_lp, uint, 0644);
module_param(input_boost_freq_hp, uint, 0644);
module_param(input_boost_duration, short, 0644);

/* Available bits for boost_drv state */
#define SCREEN_AWAKE		BIT(0)
#define INPUT_BOOST		BIT(1)
#define WAKE_BOOST		BIT(2)
#define MAX_BOOST		BIT(3)

struct boost_drv {
	struct workqueue_struct *wq;
	struct work_struct input_boost;
	struct delayed_work input_unboost;
	struct delayed_work dynamic_stune_unboost;
	struct work_struct max_boost;
	struct delayed_work max_unboost;
	struct notifier_block cpu_notif;
	struct notifier_block msm_drm_notif;
	atomic64_t max_boost_expires;
	atomic_t max_boost_dur;
	atomic_t state;
};

static struct boost_drv *boost_drv_g;

static u32 get_boost_freq(struct boost_drv *b, u32 cpu)
{
	if (cpumask_test_cpu(cpu, cpu_lp_mask))
		return input_boost_freq_lp;

	return input_boost_freq_hp;
}

static u32 get_min_freq(struct boost_drv *b, u32 cpu)
{
	if (cpumask_test_cpu(cpu, cpu_lp_mask))
		return CONFIG_REMOVE_INPUT_BOOST_FREQ_LP;

	return CONFIG_REMOVE_INPUT_BOOST_FREQ_PERF;
}

static u32 get_boost_state(struct boost_drv *b)
{
	return atomic_read(&b->state);
}

static void set_boost_bit(struct boost_drv *b, u32 state)
{
	atomic_or(state, &b->state);
}

static void clear_boost_bit(struct boost_drv *b, u32 state)
{
	atomic_andnot(state, &b->state);
}

static void update_online_cpu_policy(void)
{
	u32 cpu;

	/* Trigger cpufreq notifier for online CPUs */
	get_online_cpus();
	for_each_online_cpu(cpu)
		cpufreq_update_policy(cpu);
	put_online_cpus();
}

static void unboost_all_cpus(struct boost_drv *b)
{
	if (!cancel_delayed_work_sync(&b->input_unboost) &&
		!cancel_delayed_work_sync(&b->max_unboost))
		return;

	clear_boost_bit(b, INPUT_BOOST | WAKE_BOOST | MAX_BOOST);
	update_online_cpu_policy();
}

void cpu_input_boost_kick(void)
{
	struct boost_drv *b = boost_drv_g;

	if (!b)
		return;

	queue_work(b->wq, &b->input_boost);
}

static void __cpu_input_boost_kick_max(struct boost_drv *b,
	unsigned int duration_ms)
{
	unsigned long curr_expires, new_expires;

	do_hp_cpuset();

	do {
		curr_expires = atomic64_read(&b->max_boost_expires);
		new_expires = jiffies + msecs_to_jiffies(duration_ms);

		/* Skip this boost if there's a longer boost in effect */
		if (time_after(curr_expires, new_expires))
			return;
	} while (atomic64_cmpxchg(&b->max_boost_expires, curr_expires,
		new_expires) != curr_expires);

	atomic_set(&b->max_boost_dur, duration_ms);
	queue_work(b->wq, &b->max_boost);
}

void cpu_input_boost_kick_max(unsigned int duration_ms)
{
	struct boost_drv *b = boost_drv_g;

	if (!b)
		return;
	
	__cpu_input_boost_kick_max(b, duration_ms);
}

static void input_boost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), input_boost);

	if (!cancel_delayed_work_sync(&b->input_unboost)) {
		set_boost_bit(b, INPUT_BOOST);
		update_online_cpu_policy();
	}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	if (!do_stune_boost("top-app", dynamic_stune_boost, &boost_slot))
		stune_boost_active = true;
#endif
	queue_delayed_work(b->wq, &b->input_unboost,
		msecs_to_jiffies(input_boost_duration));

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	queue_delayed_work(b->wq, &b->dynamic_stune_unboost,
		msecs_to_jiffies(dynamic_stune_boost_duration));
#endif
}

static void input_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b =
		container_of(to_delayed_work(work), typeof(*b), input_unboost);

	clear_boost_bit(b, INPUT_BOOST);

	update_online_cpu_policy();
}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
static void dynamic_stune_unboost_worker(struct work_struct *work)
{
	if (stune_boost_active) {
		reset_stune_boost("top-app", boost_slot);
		stune_boost_active = false;
	}
}
#endif

static void max_boost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), max_boost);

	if (!cancel_delayed_work_sync(&b->max_unboost)) {
		set_boost_bit(b, MAX_BOOST);
		update_online_cpu_policy();
	}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	if (!do_stune_boost("top-app", dynamic_stune_boost, &boost_slot))
		stune_boost_active = true;
#endif
	queue_delayed_work(b->wq, &b->max_unboost,
		msecs_to_jiffies(atomic_read(&b->max_boost_dur)));
}

static void max_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b =
		container_of(to_delayed_work(work), typeof(*b), max_unboost);

	clear_boost_bit(b, WAKE_BOOST | MAX_BOOST);

	do_lp_cpuset();

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	if (stune_boost_active) {
		reset_stune_boost("top-app", boost_slot);
		stune_boost_active = false;
	}
#endif
	update_online_cpu_policy();
}

static int cpu_notifier_cb(struct notifier_block *nb,
	unsigned long action, void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), cpu_notif);
	struct cpufreq_policy *policy = data;
	u32 boost_freq, min_freq, state;

	if (action != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	state = get_boost_state(b);

	/* Boost CPU to max frequency for max boost */
	if (state & MAX_BOOST) {
		policy->min = policy->max;
		return NOTIFY_OK;
	}

	/*
	 * Boost to policy->max if the boost frequency is higher. When
	 * unboosting, set policy->min to the absolute min freq for the CPU.
	 */
	if (state & INPUT_BOOST) {
		boost_freq = get_boost_freq(b, policy->cpu);
		policy->min = min(policy->max, boost_freq);
	} else {
		min_freq = get_min_freq(b, policy->cpu);
		policy->min = max(policy->cpuinfo.min_freq, min_freq);
	}

	return NOTIFY_OK;
}

static int msm_drm_notifier_cb(struct notifier_block *nb,
			  unsigned long action, void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), msm_drm_notif);
	struct msm_drm_notifier *evdata = data;
	int *blank = evdata->data;

	/* Parse framebuffer blank events as soon as they occur */
	if (action != MSM_DRM_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	/* Boost when the screen turns on and unboost when it turns off */
	if (*blank == MSM_DRM_BLANK_UNBLANK) {
		set_boost_bit(b, SCREEN_AWAKE);
		__cpu_input_boost_kick_max(b, CONFIG_WAKE_BOOST_DURATION_MS);
	} else {
		clear_boost_bit(b, SCREEN_AWAKE);
		unboost_all_cpus(b);
	}

	return NOTIFY_OK;
}

static void cpu_input_boost_input_event(struct input_handle *handle,
	unsigned int type, unsigned int code, int value)
{
	struct boost_drv *b = handle->handler->private;
	u32 state;

	state = get_boost_state(b);

	if (!(state & SCREEN_AWAKE))
		return;

	queue_work(b->wq, &b->input_boost);
}

static int cpu_input_boost_input_connect(struct input_handler *handler,
	struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpu_input_boost_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;

	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
	kfree(handle);
	return ret;
}

static void cpu_input_boost_input_disconnect(struct input_handle *handle)
{
	do_lp_cpuset();

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	if (stune_boost_active) {
		reset_stune_boost("top-app", boost_slot);
		stune_boost_active = false;
	}
#endif
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpu_input_boost_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* Touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ }
};

static struct input_handler cpu_input_boost_input_handler = {
	.event		= cpu_input_boost_input_event,
	.connect	= cpu_input_boost_input_connect,
	.disconnect	= cpu_input_boost_input_disconnect,
	.name		= "cpu_input_boost_handler",
	.id_table	= cpu_input_boost_ids
};

static int __init cpu_input_boost_init(void)
{
	struct boost_drv *b;
	int ret;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	b->wq = alloc_workqueue("cpu_input_boost_wq", WQ_HIGHPRI, 0);
	if (!b->wq) {
		ret = -ENOMEM;
		goto free_b;
	}

	atomic64_set(&b->max_boost_expires, 0);
	INIT_WORK(&b->input_boost, input_boost_worker);
	INIT_DELAYED_WORK(&b->input_unboost, input_unboost_worker);
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	INIT_DELAYED_WORK(&b->dynamic_stune_unboost, dynamic_stune_unboost_worker);
#endif
	INIT_WORK(&b->max_boost, max_boost_worker);
	INIT_DELAYED_WORK(&b->max_unboost, max_unboost_worker);
	atomic_set(&b->state, SCREEN_AWAKE);

	b->cpu_notif.notifier_call = cpu_notifier_cb;
	ret = cpufreq_register_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("Failed to register cpufreq notifier, err: %d\n", ret);
		goto destroy_wq;
	}

	cpu_input_boost_input_handler.private = b;
	ret = input_register_handler(&cpu_input_boost_input_handler);
	if (ret) {
		pr_err("Failed to register input handler, err: %d\n", ret);
		goto unregister_cpu_notif;
	}

	b->msm_drm_notif.notifier_call = msm_drm_notifier_cb;
	b->msm_drm_notif.priority = INT_MAX;
	ret = msm_drm_register_client(&b->msm_drm_notif);
	if (ret) {
		pr_err("Failed to register dsi_panel_notifier, err: %d\n", ret);
		goto unregister_handler;
	}

	/* Allow global boost config access for external boosts */
	boost_drv_g = b;

	return 0;

unregister_handler:
	input_unregister_handler(&cpu_input_boost_input_handler);
unregister_cpu_notif:
	cpufreq_unregister_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
destroy_wq:
	destroy_workqueue(b->wq);
free_b:
	kfree(b);
	return ret;
}
late_initcall(cpu_input_boost_init);
