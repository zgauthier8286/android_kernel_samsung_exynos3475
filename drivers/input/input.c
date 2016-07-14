/*
 * The input core
 *
 * Copyright (c) 1999-2002 Vojtech Pavlik
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_BASENAME ": " fmt

#include <linux/init.h>
#include <linux/types.h>
#include <linux/idr.h>
#include <linux/input/mt.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/major.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/poll.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include "input-compat.h"

#if !defined(CONFIG_INPUT_BOOSTER) // Input Booster +
#include <linux/input/input.h>
#endif // Input Booster -

MODULE_AUTHOR("Vojtech Pavlik <vojtech@suse.cz>");
MODULE_DESCRIPTION("Input core");
MODULE_LICENSE("GPL");

#define INPUT_MAX_CHAR_DEVICES		1024
#define INPUT_FIRST_DYNAMIC_DEV		256
static DEFINE_IDA(input_ida);

static LIST_HEAD(input_dev_list);
static LIST_HEAD(input_handler_list);

/*
 * input_mutex protects access to both input_dev_list and input_handler_list.
 * This also causes input_[un]register_device and input_[un]register_handler
 * be mutually exclusive which simplifies locking in drivers implementing
 * input handlers.
 */
static DEFINE_MUTEX(input_mutex);

static const struct input_value input_value_sync = { EV_SYN, SYN_REPORT, 1 };

static inline int is_event_supported(unsigned int code,
				     unsigned long *bm, unsigned int max)
{
	return code <= max && test_bit(code, bm);
}

static int input_defuzz_abs_event(int value, int old_val, int fuzz)
{
	if (fuzz) {
		if (value > old_val - fuzz / 2 && value < old_val + fuzz / 2)
			return old_val;

		if (value > old_val - fuzz && value < old_val + fuzz)
			return (old_val * 3 + value) / 4;

		if (value > old_val - fuzz * 2 && value < old_val + fuzz * 2)
			return (old_val + value) / 2;
	}

	return value;
}

static void input_start_autorepeat(struct input_dev *dev, int code)
{
	if (test_bit(EV_REP, dev->evbit) &&
	    dev->rep[REP_PERIOD] && dev->rep[REP_DELAY] &&
	    dev->timer.data) {
		dev->repeat_key = code;
		mod_timer(&dev->timer,
			  jiffies + msecs_to_jiffies(dev->rep[REP_DELAY]));
	}
}

static void input_stop_autorepeat(struct input_dev *dev)
{
	del_timer(&dev->timer);
}

/*
 * Pass event first through all filters and then, if event has not been
 * filtered out, through all open handles. This function is called with
 * dev->event_lock held and interrupts disabled.
 */
static unsigned int input_to_handler(struct input_handle *handle,
			struct input_value *vals, unsigned int count)
{
	struct input_handler *handler = handle->handler;
	struct input_value *end = vals;
	struct input_value *v;

	for (v = vals; v != vals + count; v++) {
		if (handler->filter &&
		    handler->filter(handle, v->type, v->code, v->value))
			continue;
		if (end != v)
			*end = *v;
		end++;
	}

	count = end - vals;
	if (!count)
		return 0;

	if (handler->events)
		handler->events(handle, vals, count);
	else if (handler->event)
		for (v = vals; v != end; v++)
			handler->event(handle, v->type, v->code, v->value);

	return count;
}

/*
 * Pass values first through all filters and then, if event has not been
 * filtered out, through all open handles. This function is called with
 * dev->event_lock held and interrupts disabled.
 */
static void input_pass_values(struct input_dev *dev,
			      struct input_value *vals, unsigned int count)
{
	struct input_handle *handle;
	struct input_value *v;

	if (!count)
		return;

	rcu_read_lock();

	handle = rcu_dereference(dev->grab);
	if (handle) {
		count = input_to_handler(handle, vals, count);
	} else {
		list_for_each_entry_rcu(handle, &dev->h_list, d_node)
			if (handle->open)
				count = input_to_handler(handle, vals, count);
	}

	rcu_read_unlock();

	add_input_randomness(vals->type, vals->code, vals->value);

	/* trigger auto repeat for key events */
	for (v = vals; v != vals + count; v++) {
		if (v->type == EV_KEY && v->value != 2) {
			if (v->value)
				input_start_autorepeat(dev, v->code);
			else
				input_stop_autorepeat(dev);
		}
	}
}

static void input_pass_event(struct input_dev *dev,
			     unsigned int type, unsigned int code, int value)
{
	struct input_value vals[] = { { type, code, value } };

	input_pass_values(dev, vals, ARRAY_SIZE(vals));
}

/*
 * Generate software autorepeat event. Note that we take
 * dev->event_lock here to avoid racing with input_event
 * which may cause keys get "stuck".
 */
static void input_repeat_key(unsigned long data)
{
	struct input_dev *dev = (void *) data;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);

	if (test_bit(dev->repeat_key, dev->key) &&
	    is_event_supported(dev->repeat_key, dev->keybit, KEY_MAX)) {
		struct input_value vals[] =  {
			{ EV_KEY, dev->repeat_key, 2 },
			input_value_sync
		};

		input_pass_values(dev, vals, ARRAY_SIZE(vals));

		if (dev->rep[REP_PERIOD])
			mod_timer(&dev->timer, jiffies +
					msecs_to_jiffies(dev->rep[REP_PERIOD]));
	}

	spin_unlock_irqrestore(&dev->event_lock, flags);
}

#define INPUT_IGNORE_EVENT	0
#define INPUT_PASS_TO_HANDLERS	1
#define INPUT_PASS_TO_DEVICE	2
#define INPUT_SLOT		4
#define INPUT_FLUSH		8
#define INPUT_PASS_TO_ALL	(INPUT_PASS_TO_HANDLERS | INPUT_PASS_TO_DEVICE)

static int input_handle_abs_event(struct input_dev *dev,
				  unsigned int code, int *pval)
{
	struct input_mt *mt = dev->mt;
	bool is_mt_event;
	int *pold;

	if (code == ABS_MT_SLOT) {
		/*
		 * "Stage" the event; we'll flush it later, when we
		 * get actual touch data.
		 */
		if (mt && *pval >= 0 && *pval < mt->num_slots)
			mt->slot = *pval;

		return INPUT_IGNORE_EVENT;
	}

	is_mt_event = input_is_mt_value(code);

	if (!is_mt_event) {
		pold = &dev->absinfo[code].value;
	} else if (mt) {
		pold = &mt->slots[mt->slot].abs[code - ABS_MT_FIRST];
	} else {
		/*
		 * Bypass filtering for multi-touch events when
		 * not employing slots.
		 */
		pold = NULL;
	}

	if (pold) {
		*pval = input_defuzz_abs_event(*pval, *pold,
						dev->absinfo[code].fuzz);
		if (*pold == *pval)
			return INPUT_IGNORE_EVENT;

		*pold = *pval;
	}

	/* Flush pending "slot" event */
	if (is_mt_event && mt && mt->slot != input_abs_get_val(dev, ABS_MT_SLOT)) {
		input_abs_set_val(dev, ABS_MT_SLOT, mt->slot);
		return INPUT_PASS_TO_HANDLERS | INPUT_SLOT;
	}

	return INPUT_PASS_TO_HANDLERS;
}

static int input_get_disposition(struct input_dev *dev,
			  unsigned int type, unsigned int code, int value)
{
	int disposition = INPUT_IGNORE_EVENT;

	switch (type) {

	case EV_SYN:
		switch (code) {
		case SYN_CONFIG:
			disposition = INPUT_PASS_TO_ALL;
			break;

		case SYN_REPORT:
			disposition = INPUT_PASS_TO_HANDLERS | INPUT_FLUSH;
			break;
		case SYN_MT_REPORT:
			disposition = INPUT_PASS_TO_HANDLERS;
			break;
		}
		break;

	case EV_KEY:
		if (is_event_supported(code, dev->keybit, KEY_MAX)) {

			/* auto-repeat bypasses state updates */
			if (value == 2) {
				disposition = INPUT_PASS_TO_HANDLERS;
				break;
			}

			if (!!test_bit(code, dev->key) != !!value) {

				__change_bit(code, dev->key);
				disposition = INPUT_PASS_TO_HANDLERS;
			}
		}
		break;

	case EV_SW:
		if (is_event_supported(code, dev->swbit, SW_MAX) &&
		    !!test_bit(code, dev->sw) != !!value) {

			__change_bit(code, dev->sw);
			disposition = INPUT_PASS_TO_HANDLERS;
		}
		break;

	case EV_ABS:
		if (is_event_supported(code, dev->absbit, ABS_MAX))
			disposition = input_handle_abs_event(dev, code, &value);

		break;

	case EV_REL:
		if (is_event_supported(code, dev->relbit, REL_MAX) && value)
			disposition = INPUT_PASS_TO_HANDLERS;

		break;

	case EV_MSC:
		if (is_event_supported(code, dev->mscbit, MSC_MAX))
			disposition = INPUT_PASS_TO_ALL;

		break;

	case EV_LED:
		if (is_event_supported(code, dev->ledbit, LED_MAX) &&
		    !!test_bit(code, dev->led) != !!value) {

			__change_bit(code, dev->led);
			disposition = INPUT_PASS_TO_ALL;
		}
		break;

	case EV_SND:
		if (is_event_supported(code, dev->sndbit, SND_MAX)) {

			if (!!test_bit(code, dev->snd) != !!value)
				__change_bit(code, dev->snd);
			disposition = INPUT_PASS_TO_ALL;
		}
		break;

	case EV_REP:
		if (code <= REP_MAX && value >= 0 && dev->rep[code] != value) {
			dev->rep[code] = value;
			disposition = INPUT_PASS_TO_ALL;
		}
		break;

	case EV_FF:
		if (value >= 0)
			disposition = INPUT_PASS_TO_ALL;
		break;

	case EV_PWR:
		disposition = INPUT_PASS_TO_ALL;
		break;
	}

	return disposition;
}

static void input_handle_event(struct input_dev *dev,
			       unsigned int type, unsigned int code, int value)
{
	int disposition;

	disposition = input_get_disposition(dev, type, code, value);

	if ((disposition & INPUT_PASS_TO_DEVICE) && dev->event)
		dev->event(dev, type, code, value);

	if (!dev->vals)
		return;

	if (disposition & INPUT_PASS_TO_HANDLERS) {
		struct input_value *v;

		if (disposition & INPUT_SLOT) {
			v = &dev->vals[dev->num_vals++];
			v->type = EV_ABS;
			v->code = ABS_MT_SLOT;
			v->value = dev->mt->slot;
		}

		v = &dev->vals[dev->num_vals++];
		v->type = type;
		v->code = code;
		v->value = value;
	}

	if (disposition & INPUT_FLUSH) {
		if (dev->num_vals >= 2)
			input_pass_values(dev, dev->vals, dev->num_vals);
		dev->num_vals = 0;
	} else if (dev->num_vals >= dev->max_vals - 2) {
		dev->vals[dev->num_vals++] = input_value_sync;
		input_pass_values(dev, dev->vals, dev->num_vals);
		dev->num_vals = 0;
	}

}


#if !defined(CONFIG_INPUT_BOOSTER) // Input Booster +
// ********** Define Timeout Functions ********** //
DECLARE_TIMEOUT_FUNC(touch);
DECLARE_TIMEOUT_FUNC(multitouch);
DECLARE_TIMEOUT_FUNC(key);
DECLARE_TIMEOUT_FUNC(touchkey);
DECLARE_TIMEOUT_FUNC(keyboard);
DECLARE_TIMEOUT_FUNC(mouse);
DECLARE_TIMEOUT_FUNC(mouse_wheel);
DECLARE_TIMEOUT_FUNC(pen);
DECLARE_TIMEOUT_FUNC(hover);
DECLARE_TIMEOUT_FUNC(gamepad);

// ********** Define Set Booster Functions ********** //
DECLARE_SET_BOOSTER_FUNC(touch);
DECLARE_SET_BOOSTER_FUNC(multitouch);
DECLARE_SET_BOOSTER_FUNC(key);
DECLARE_SET_BOOSTER_FUNC(touchkey);
DECLARE_SET_BOOSTER_FUNC(keyboard);
DECLARE_SET_BOOSTER_FUNC(mouse);
DECLARE_SET_BOOSTER_FUNC(mouse_wheel);
DECLARE_SET_BOOSTER_FUNC(pen);
DECLARE_SET_BOOSTER_FUNC(hover);
DECLARE_SET_BOOSTER_FUNC(gamepad);

// ********** Define Reet Booster Functions ********** //
DECLARE_RESET_BOOSTER_FUNC(touch);
DECLARE_RESET_BOOSTER_FUNC(multitouch);
DECLARE_RESET_BOOSTER_FUNC(key);
DECLARE_RESET_BOOSTER_FUNC(touchkey);
DECLARE_RESET_BOOSTER_FUNC(keyboard);
DECLARE_RESET_BOOSTER_FUNC(mouse);
DECLARE_RESET_BOOSTER_FUNC(mouse_wheel);
DECLARE_RESET_BOOSTER_FUNC(pen);
DECLARE_RESET_BOOSTER_FUNC(hover);
DECLARE_RESET_BOOSTER_FUNC(gamepad);

// ********** Define State Functions ********** //
DECLARE_STATE_FUNC(idle)
{
	struct t_input_booster *_this = (struct t_input_booster *)(__this);
	glGage = HEADGAGE;
	if(input_booster_event == BOOSTER_ON) {
		int i;
		pr_debug("[Input Booster] %s      State0 : Idle  index : %d, hmp : %d, cpu : %d, time : %d, input_booster_event : %d\n", glGage, _this->index, _this->param[_this->index].hmp_boost, _this->param[_this->index].cpu_freq, _this->param[_this->index].time, input_booster_event);
		_this->index=0;
		for(i=0;i<2;i++) {
			if(delayed_work_pending(&_this->input_booster_timeout_work[i])) {
				pr_debug("[Input Booster] ****             cancel the pending workqueue\n");
				cancel_delayed_work(&_this->input_booster_timeout_work[i]);
			}
		}
		SET_BOOSTER;
		schedule_delayed_work(&_this->input_booster_timeout_work[_this->index], msecs_to_jiffies(_this->param[_this->index].time));
		_this->index++;
		CHANGE_STATE_TO(press);
	} else if(input_booster_event == BOOSTER_OFF) {
		pr_debug("[Input Booster] %s      Skipped  index : %d, hmp : %d, cpu : %d, input_booster_event : %d\n", glGage, _this->index, _this->param[_this->index].hmp_boost, _this->param[_this->index].cpu_freq, input_booster_event);
		pr_debug("\n");
	}
}

DECLARE_STATE_FUNC(press)
{
	struct t_input_booster *_this = (struct t_input_booster *)(__this);
	glGage = TAILGAGE;

	if(input_booster_event == BOOSTER_OFF) {
		pr_debug("[Input Booster] %s      State : Press  index : %d, time : %d\n", glGage, _this->index, _this->param[_this->index].time);
		if(_this->multi_events <= 0 && _this->index < 2) {
			if(delayed_work_pending(&_this->input_booster_timeout_work[(_this->index) ? _this->index-1 : 0]) || (_this->param[(_this->index) ? _this->index-1 : 0].time == 0)) {
				if(_this->change_on_release || (_this->param[(_this->index) ? _this->index-1 : 0].time == 0)) {
					pr_debug("[Input Booster] %s           cancel the pending workqueue\n", glGage);
					cancel_delayed_work(&_this->input_booster_timeout_work[(_this->index) ? _this->index-1 : 0]);
					SET_BOOSTER;
				}
				schedule_delayed_work(&_this->input_booster_timeout_work[_this->index], msecs_to_jiffies(_this->param[_this->index].time));
				pr_debug("[Input Booster] %s           schedule_delayed_work again  time : %d\n", glGage, _this->param[_this->index].time);
				if(!delayed_work_pending(&_this->input_booster_timeout_work[_this->index]) && _this->param[_this->index].time > 0) {
					pr_debug("[Input Booster] %s           schedule_delayed_work Re-again time : %d\n", glGage, _this->param[(_this->index > 0) ? _this->index-1 : _this->index].time);
					schedule_delayed_work(&_this->input_booster_timeout_work[(_this->index > 0) ? _this->index-1 : _this->index], msecs_to_jiffies(_this->param[(_this->index > 0) ? _this->index-1 : _this->index].time));
				}
			} else if(_this->param[_this->index].time > 0) {
				schedule_delayed_work(&_this->input_booster_timeout_work[_this->index], msecs_to_jiffies(_this->param[_this->index].time));
			} else {
				schedule_delayed_work(&_this->input_booster_timeout_work[(_this->index) ? _this->index-1 : 0], msecs_to_jiffies(_this->param[(_this->index > 0) ? _this->index-1 : _this->index].time));
			}
			_this->index++;
			_this->multi_events = (_this->multi_events > 0) ? 0 : _this->multi_events;
			CHANGE_STATE_TO(idle);
		}
	} else if(input_booster_event == BOOSTER_ON) {
		if(delayed_work_pending(&_this->input_booster_timeout_work[_this->index])) {
			pr_debug("[Input Booster] %s           cancel the pending workqueue for multi events\n", glGage);
			cancel_delayed_work(&_this->input_booster_timeout_work[_this->index]);
			schedule_delayed_work(&_this->input_booster_timeout_work[(_this->index) ? _this->index-1 : 0], msecs_to_jiffies(_this->param[(_this->index > 0) ? _this->index-1 : _this->index].time));
		} else {
			pr_debug("[Input Booster] %s      State : Press  index : %d, time : %d\n", glGage, _this->index, _this->param[_this->index].time);
		}
	}
}

void input_booster_disable(struct t_input_booster *_this)
{
	schedule_work(&_this->input_booster_reset_booster_work);
}

// ********** Detect Events ********** //
void input_booster(struct input_dev *dev)
{
	int i, j, DetectedCategory = false, iTouchID = -1, iTouchSlot = -1, gamepad_flag = 0;
#if defined(CONFIG_SOC_EXYNOS7420) // This code should be working properly in Exynos7420(Noble & Zero2) only.
	int lcdoffcounter = 0;
#endif
	for(i=0;i<input_count;i++) {
		if (DetectedCategory) {
			break;
		} else if (input_events[i].type == EV_KEY) {
			switch (input_events[i].code) {
				case BTN_TOUCH :
					if(input_events[i+1].type == EV_KEY && input_events[i+1].code == BTN_TOOL_PEN) {
						if(input_events[i].value && !pen_booster.multi_events) {
							pr_debug("[Input Booster] PEN EVENT - PRESS\n");
							RUN_BOOSTER(pen, BOOSTER_ON);
							DetectedCategory = true;
						} else if(!input_events[i].value && pen_booster.multi_events) {
							pr_debug("[Input Booster] PEN EVENT - RELEASE\n");
							RUN_BOOSTER(pen, BOOSTER_OFF);
							DetectedCategory = true;
						}
					} else if(iTouchID >= 0) { // ***************** Checking Touch Event's ID whethere it is same with previous ID.
						if(!input_events[i].value && input_events[iTouchID].value < 0) {  // If this event is 'Release'
							for(j=0;j<MAX_MULTI_TOUCH_EVENTS;j++) {
								TouchIDs[j] = -1;
							}
						}
					}
					break;
				case BTN_TOOL_PEN :
					if(input_events[i].value && !hover_booster.multi_events) {
						pr_debug("[Input Booster] PEN EVENT - HOVER ON\n");
						RUN_BOOSTER(hover, BOOSTER_ON);
						DetectedCategory = true;
					} else if(!input_events[i].value && hover_booster.multi_events) {
						pr_debug("[Input Booster] PEN EVENT - HOVER OFF\n");
						RUN_BOOSTER(hover, BOOSTER_OFF);
						DetectedCategory = true;
					}
					break;
				case KEY_BACK : // ***************** Checking Key & Touch key Event
					if(key_back != input_events[i].value) {
						key_back = input_events[i].value;
						pr_debug("[Input Booster] TOUCHKEY EVENT - %s\n", (input_events[i].value) ? "PRESS" : "RELEASE");
						RUN_BOOSTER(touchkey, (input_events[i].value) ? BOOSTER_ON : BOOSTER_OFF );
						DetectedCategory = true;
					}
					break;
				case KEY_HOMEPAGE :
					if(key_home != input_events[i].value) {
						key_home = input_events[i].value;
						pr_debug("[Input Booster] TOUCHKEY EVENT - %s\n", (input_events[i].value) ? "PRESS" : "RELEASE");
						RUN_BOOSTER(touchkey, (input_events[i].value) ? BOOSTER_ON : BOOSTER_OFF );
						DetectedCategory = true;
					}
					break;
				case KEY_RECENT :
					if(key_recent != input_events[i].value) {
						key_recent = input_events[i].value;
						pr_debug("[Input Booster] TOUCHKEY EVENT - %s\n", (input_events[i].value) ? "PRESS" : "RELEASE");
						RUN_BOOSTER(touchkey, (input_events[i].value) ? BOOSTER_ON : BOOSTER_OFF );
						DetectedCategory = true;
					}
					break;
				case KEY_VOLUMEUP :
				case KEY_VOLUMEDOWN :
				case KEY_POWER :
					pr_debug("[Input Booster] KEY EVENT - %s\n", (input_events[i].value) ? "PRESS" : "RELEASE");
					RUN_BOOSTER(key, (input_events[i].value) ? BOOSTER_ON : BOOSTER_OFF );
					DetectedCategory = true;
					break;
				default :
					break;
			}
		} else if (input_events[i].type == EV_ABS) {
			if (input_events[i].code == ABS_MT_TRACKING_ID) {
				iTouchID = i;
				if(input_events[iTouchSlot].value < MAX_MULTI_TOUCH_EVENTS && input_events[iTouchSlot].value >= 0 && iTouchID < MAX_EVENTS && iTouchSlot <= MAX_EVENTS) {
					if(TouchIDs[input_events[iTouchSlot].value] < 0 && input_events[iTouchID].value >= 0) {
						TouchIDs[input_events[iTouchSlot].value] = input_events[iTouchID].value;
						if(touch_booster.multi_events <= 0 || input_events[iTouchSlot].value == 0) {
							touch_booster.multi_events = 0;
							pr_debug("[Input Booster] TOUCH EVENT - PRESS - ID: 0x%x, Slot: 0x%x, multi : %d\n", input_events[iTouchID].value, input_events[iTouchSlot].value, touch_booster.multi_events);
							RUN_BOOSTER(touch, BOOSTER_ON );
						} else {
							pr_debug("[Input Booster] MULTI-TOUCH EVENT - PRESS - ID: 0x%x, Slot: 0x%x, multi : %d\n", input_events[iTouchID].value, input_events[iTouchSlot].value, touch_booster.multi_events);
							touch_booster.multi_events++;
							RUN_BOOSTER(multitouch, BOOSTER_ON );
							if(delayed_work_pending(&touch_booster.input_booster_timeout_work[0])) {
								int temp_hmp_boost = touch_booster.param[0].hmp_boost, temp_index = touch_booster.index;
								pr_debug("[Input Booster] ****             cancel the pending touch booster workqueue\n");
								cancel_delayed_work(&touch_booster.input_booster_timeout_work[0]);
								touch_booster.param[0].hmp_boost = multitouch_booster.param[0].hmp_boost;
								touch_booster.index = 1;
								TIMEOUT_FUNC(touch)(NULL);
								touch_booster.param[0].hmp_boost = temp_hmp_boost;
								touch_booster.index = ( temp_index >= 2 ? 1 : temp_index );
							}
						}
					} else if(TouchIDs[input_events[iTouchSlot].value] >= 0 && input_events[iTouchID].value < 0) {
						TouchIDs[input_events[iTouchSlot].value] = input_events[iTouchID].value;
						if(touch_booster.multi_events <= 1) {
							pr_debug("[Input Booster] TOUCH EVENT - RELEASE - ID: 0x%x, Slot: 0x%x, multi : %d\n", input_events[iTouchID].value, input_events[iTouchSlot].value, touch_booster.multi_events);
							RUN_BOOSTER(touch, BOOSTER_OFF );
						} else {
							pr_debug("[Input Booster] MULTI-TOUCH EVENT - RELEASE - ID: 0x%x, Slot: 0x%x, multi : %d\n", input_events[iTouchID].value, input_events[iTouchSlot].value, touch_booster.multi_events);
							touch_booster.multi_events--;
							RUN_BOOSTER(multitouch, BOOSTER_OFF );
						}
					}
				}
			} else if (input_events[i].code == ABS_MT_SLOT) {
				iTouchSlot = i;
#if defined(CONFIG_SOC_EXYNOS7420) // This code should be working properly in Exynos7420(Noble & Zero2) only.
				if(input_events[iTouchSlot + 1].value < 0) {
					lcdoffcounter++;
				}
				if(lcdoffcounter >= 10) {
					input_booster_disable(&touch_booster);
					input_booster_disable(&multitouch_booster);
					input_booster_disable(&key_booster);
					input_booster_disable(&touchkey_booster);
					input_booster_disable(&keyboard_booster);
					input_booster_disable(&mouse_booster);
					input_booster_disable(&mouse_wheel_booster);
					input_booster_disable(&pen_booster);
					input_booster_disable(&hover_booster);
					pr_debug("[Input Booster] *****************************\n[Input Booster] All boosters are reset  %d\n[Input Booster] *****************************\n", lcdoffcounter);
				}
#endif
			} else if (input_events[i].code >= ABS_X && input_events[i].code <= ABS_RZ) {
				if(input_events[i].value != 0x80) {
					gamepad_flag = 2;
				} else if(gamepad_flag == 0 && input_events[i].value == 80) {
					gamepad_flag = 1;
				}
			} else if (input_events[i].code >= ABS_HAT0X && input_events[i].code <= ABS_HAT3Y) {
				if(input_events[i].value != 0) {
					gamepad_flag = 2;
				} else if(gamepad_flag == 0 && input_events[i].value == 0) {
					gamepad_flag = 1;
				}
			}
		} else if (input_events[i].type == EV_MSC && input_events[i].code == MSC_SCAN) {
			if (input_events[i+1].type == EV_KEY) {
				switch (input_events[i+1].code) {
					case BTN_LEFT : // ***************** Checking Touch Button Event
					case BTN_RIGHT :
					case BTN_MIDDLE :
						pr_debug("[Input Booster] MOUSE EVENT - %s\n", (input_events[i+1].value) ? "PRESS" : "RELEASE");
						RUN_BOOSTER(mouse, (input_events[i+1].value) ? BOOSTER_ON : BOOSTER_OFF );
						DetectedCategory = true;
						break;
					default : // ***************** Checking Keyboard Event
						pr_debug("[Input Booster] KEYBOARD EVENT - %s (multi count %d )\n", (input_events[i+1].value) ? "PRESS" : "RELEASE", keyboard_booster.multi_events);
						RUN_BOOSTER(keyboard, (input_events[i+1].value) ? BOOSTER_ON : BOOSTER_OFF );
						//DetectedCategory = true; // keyboard event can be continue in a set.
						break;
				}
			}
		} else if (input_events[i].type == EV_REL && input_events[i].code == REL_WHEEL && input_events[i].value) { // ***************** Wheel Event for Mouse
			if (input_events[0].type == EV_KEY && input_events[0].code == BTN_LEFT) {
				pr_debug("[Input Booster] MOUSE EVENT - %s\n", "WHELL");
				RUN_BOOSTER(mouse_wheel, BOOSTER_ON);
				DetectedCategory = true;
			}
		}
	}

	if(gamepad_flag == 2 && gamepad_booster.multi_events <= 0) {
		pr_debug("[Input Booster] GAME PAD EVENT - ON\n");
		RUN_BOOSTER(gamepad, BOOSTER_ON );
	} else if(gamepad_flag == 1 && gamepad_booster.multi_events == 1) {
		pr_debug("[Input Booster] GAME PAD EVENT - OFF\n");
		RUN_BOOSTER(gamepad, BOOSTER_OFF );
	}
}

// ********** Init Booster ********** //
void input_booster_init()
{
	// ********** Load Frequncy data from DTSI **********
	struct device_node *np;
	int nlevels = 0, i;

	if(device_tree_infor != NULL){
		return;
	}

	np = of_find_compatible_node(NULL, NULL, "input_booster");

	if(np == NULL) {
		ndevice_in_dt = 0;
		return;
	}

	// Geting the count of devices.
	ndevice_in_dt = of_get_child_count(np);
	pr_debug("[Input Booster] %s   ndevice_in_dt : %d\n", __FUNCTION__, ndevice_in_dt);

	device_tree_infor = kcalloc(ABS_CNT, sizeof(struct t_input_booster_device_tree_infor) * ndevice_in_dt, GFP_KERNEL);
	if(device_tree_infor > 0) {
		struct device_node *cnp;
		int device_count = 0;

		for_each_child_of_node(np, cnp) {
			struct t_input_booster_device_tree_infor *dt_infor = (device_tree_infor + device_count);
			const u32 *plevels = NULL;

			// Geting label.
			dt_infor->label = of_get_property(cnp, "input_booster,label", NULL);
			pr_debug("[Input Booster] %s   dt_infor->label : %s\n", __FUNCTION__, dt_infor->label);

			if (of_property_read_u32(cnp, "input_booster,type", &dt_infor->type)) {
				pr_debug("Failed to get type property\n");
				break;
			}

			// Geting the count of levels.
			plevels = of_get_property(cnp, "input_booster,levels", &nlevels);

			if (plevels && nlevels) {
				dt_infor->nlevels = nlevels / sizeof(u32);
				pr_debug("[Input Booster] %s   dt_infor->nlevels : %d\n", __FUNCTION__, dt_infor->nlevels);
			} else {
				pr_debug("Failed to calculate number of frequency.\n");
				break;
			}

			// Allocation the param table.
			dt_infor->param_tables = kcalloc(ABS_CNT, sizeof(struct t_input_booster_device_tree_param) * dt_infor->nlevels, GFP_KERNEL);
			if (!dt_infor->param_tables) {
				pr_debug("Failed to allocate memory of freq_table\n");
				break;
			}

			// fill the param table
			pr_debug("[Input Booster] device_type:%d, label :%s, type: 0x%02x, num_level[%d]\n",
				dt_infor->type, dt_infor->label, dt_infor->type, dt_infor->nlevels);

			for (i = 0; i < dt_infor->nlevels; i++) {
				u32 temp;
				int err = 0;

				err = of_property_read_u32_index(cnp, "input_booster,levels", i, &temp);  dt_infor->param_tables[i].ilevels = (u8)temp;
				err |= of_property_read_u32_index(cnp, "input_booster,cpu_freqs", i, &dt_infor->param_tables[i].cpu_freq);
				err |= of_property_read_u32_index(cnp, "input_booster,kfc_freqs", i, &dt_infor->param_tables[i].kfc_freq);
				err |= of_property_read_u32_index(cnp, "input_booster,mif_freqs", i, &dt_infor->param_tables[i].mif_freq);
				err |= of_property_read_u32_index(cnp, "input_booster,int_freqs", i, &dt_infor->param_tables[i].int_freq);
				err |= of_property_read_u32_index(cnp, "input_booster,hmp_boost", i, &temp); dt_infor->param_tables[i].hmp_boost = (u8)temp;
				err |= of_property_read_u32_index(cnp, "input_booster,head_times", i, &temp); dt_infor->param_tables[i].head_time = (u16)temp;
				err |= of_property_read_u32_index(cnp, "input_booster,tail_times", i, &temp); dt_infor->param_tables[i].tail_time = (u16)temp;
				err |= of_property_read_u32_index(cnp, "input_booster,phase_times", i, &temp); dt_infor->param_tables[i].phase_time = (u16)temp;
				if (err) {
					pr_debug("Failed to get [%d] param table property\n", i);
				}

				pr_debug("[Input Booster] Level %d : frequency[%d,%d,%d,%d] hmp_boost[%d] times[%d,%d,%d]\n", i,
					dt_infor->param_tables[i].cpu_freq,
					dt_infor->param_tables[i].kfc_freq,
					dt_infor->param_tables[i].mif_freq,
					dt_infor->param_tables[i].int_freq,
					dt_infor->param_tables[i].hmp_boost,
					dt_infor->param_tables[i].head_time,
					dt_infor->param_tables[i].tail_time,
					dt_infor->param_tables[i].phase_time);
			}

			device_count++;
		}
	}

	// ********** Initialize Buffer for Touch **********
	for(i=0;i<MAX_MULTI_TOUCH_EVENTS;i++) {
		TouchIDs[i] = -1;
	}

	// ********** Initialize Booster **********
	INIT_BOOSTER(touch)
	INIT_BOOSTER(multitouch)
	INIT_BOOSTER(key)
	INIT_BOOSTER(touchkey)
	INIT_BOOSTER(keyboard)
	INIT_BOOSTER(mouse)
	INIT_BOOSTER(mouse_wheel)
	INIT_BOOSTER(pen)
	INIT_BOOSTER(hover)
	INIT_BOOSTER(gamepad)
	multitouch_booster.change_on_release = 1;

	// ********** Initialize Sysfs **********
	{
		struct class *sysfs_class;

		sysfs_class = class_create(THIS_MODULE, "input_booster");
		if (IS_ERR(sysfs_class)) {
			pr_debug("[Input Booster] Failed to create class\n");
			return;
		}

		INIT_SYSFS_CLASS(debug_level)
		INIT_SYSFS_CLASS(head)
		INIT_SYSFS_CLASS(tail)
		INIT_SYSFS_CLASS(level)

		INIT_SYSFS_DEVICE(touch)
		INIT_SYSFS_DEVICE(multitouch)
		INIT_SYSFS_DEVICE(key)
		INIT_SYSFS_DEVICE(touchkey)
		INIT_SYSFS_DEVICE(keyboard)
		INIT_SYSFS_DEVICE(mouse)
		INIT_SYSFS_DEVICE(mouse_wheel)
		INIT_SYSFS_DEVICE(pen)
		INIT_SYSFS_DEVICE(hover)
		INIT_SYSFS_DEVICE(gamepad)
	}
}
#endif  // Input Booster -

/**
 * input_event() - report new input event
 * @dev: device that generated the event
 * @type: type of the event
 * @code: event code
 * @value: value of the event
 *
 * This function should be used by drivers implementing various input
 * devices to report input events. See also input_inject_event().
 *
 * NOTE: input_event() may be safely used right after input device was
 * allocated with input_allocate_device(), even before it is registered
 * with input_register_device(), but the event will not reach any of the
 * input handlers. Such early invocation of input_event() may be used
 * to 'seed' initial state of a switch or initial position of absolute
 * axis, etc.
 */
void input_event(struct input_dev *dev,
		 unsigned int type, unsigned int code, int value)
{
	unsigned long flags;

	if (is_event_supported(type, dev->evbit, EV_MAX)) {

		spin_lock_irqsave(&dev->event_lock, flags);
		input_handle_event(dev, type, code, value);
		spin_unlock_irqrestore(&dev->event_lock, flags);

#if !defined(CONFIG_INPUT_BOOSTER) // Input Booster +
		if(device_tree_infor != NULL) {
			if (type == EV_SYN && input_count > 0) {
				pr_debug("[Input Booster1] ==============================================\n");
				input_booster(dev);
				input_count=0;
			} else {
				pr_debug("[Input Booster1] type = %x, code = %x, value =%x\n", type, code, value);
				input_events[input_count].type = type;
				input_events[input_count].code = code;
				input_events[input_count].value = value;
				if(input_count < MAX_EVENTS) {
					input_count++;
				}
			}
		}
#endif  // Input Booster -
	}
}
EXPORT_SYMBOL(input_event);

/**
 * input_inject_event() - send input event from input handler
 * @handle: input handle to send event through
 * @type: type of the event
 * @code: event code
 * @value: value of the event
 *
 * Similar to input_event() but will ignore event if device is
 * "grabbed" and handle injecting event is not the one that owns
 * the device.
 */
void input_inject_event(struct input_handle *handle,
			unsigned int type, unsigned int code, int value)
{
	struct input_dev *dev = handle->dev;
	struct input_handle *grab;
	unsigned long flags;

	if (is_event_supported(type, dev->evbit, EV_MAX)) {
		spin_lock_irqsave(&dev->event_lock, flags);

		rcu_read_lock();
		grab = rcu_dereference(dev->grab);
		if (!grab || grab == handle)
			input_handle_event(dev, type, code, value);
		rcu_read_unlock();

		spin_unlock_irqrestore(&dev->event_lock, flags);
	}
}
EXPORT_SYMBOL(input_inject_event);

/**
 * input_alloc_absinfo - allocates array of input_absinfo structs
 * @dev: the input device emitting absolute events
 *
 * If the absinfo struct the caller asked for is already allocated, this
 * functions will not do anything.
 */
void input_alloc_absinfo(struct input_dev *dev)
{
	if (!dev->absinfo)
		dev->absinfo = kcalloc(ABS_CNT, sizeof(struct input_absinfo),
					GFP_KERNEL);

	WARN(!dev->absinfo, "%s(): kcalloc() failed?\n", __func__);
}
EXPORT_SYMBOL(input_alloc_absinfo);

void input_set_abs_params(struct input_dev *dev, unsigned int axis,
			  int min, int max, int fuzz, int flat)
{
	struct input_absinfo *absinfo;

	input_alloc_absinfo(dev);
	if (!dev->absinfo)
		return;

	absinfo = &dev->absinfo[axis];
	absinfo->minimum = min;
	absinfo->maximum = max;
	absinfo->fuzz = fuzz;
	absinfo->flat = flat;

	dev->absbit[BIT_WORD(axis)] |= BIT_MASK(axis);
}
EXPORT_SYMBOL(input_set_abs_params);


/**
 * input_grab_device - grabs device for exclusive use
 * @handle: input handle that wants to own the device
 *
 * When a device is grabbed by an input handle all events generated by
 * the device are delivered only to this handle. Also events injected
 * by other input handles are ignored while device is grabbed.
 */
int input_grab_device(struct input_handle *handle)
{
	struct input_dev *dev = handle->dev;
	int retval;

	retval = mutex_lock_interruptible(&dev->mutex);
	if (retval)
		return retval;

	if (dev->grab) {
		retval = -EBUSY;
		goto out;
	}

	rcu_assign_pointer(dev->grab, handle);

 out:
	mutex_unlock(&dev->mutex);
	return retval;
}
EXPORT_SYMBOL(input_grab_device);

static void __input_release_device(struct input_handle *handle)
{
	struct input_dev *dev = handle->dev;
	struct input_handle *grabber;

	grabber = rcu_dereference_protected(dev->grab,
					    lockdep_is_held(&dev->mutex));
	if (grabber == handle) {
		rcu_assign_pointer(dev->grab, NULL);
		/* Make sure input_pass_event() notices that grab is gone */
		synchronize_rcu();

		list_for_each_entry(handle, &dev->h_list, d_node)
			if (handle->open && handle->handler->start)
				handle->handler->start(handle);
	}
}

/**
 * input_release_device - release previously grabbed device
 * @handle: input handle that owns the device
 *
 * Releases previously grabbed device so that other input handles can
 * start receiving input events. Upon release all handlers attached
 * to the device have their start() method called so they have a change
 * to synchronize device state with the rest of the system.
 */
void input_release_device(struct input_handle *handle)
{
	struct input_dev *dev = handle->dev;

	mutex_lock(&dev->mutex);
	__input_release_device(handle);
	mutex_unlock(&dev->mutex);
}
EXPORT_SYMBOL(input_release_device);

/**
 * input_open_device - open input device
 * @handle: handle through which device is being accessed
 *
 * This function should be called by input handlers when they
 * want to start receive events from given input device.
 */
int input_open_device(struct input_handle *handle)
{
	struct input_dev *dev = handle->dev;
	int retval;

	retval = mutex_lock_interruptible(&dev->mutex);
	if (retval)
		return retval;

	if (dev->going_away) {
		retval = -ENODEV;
		goto out;
	}

	handle->open++;

	dev->users_private++;
	if (!dev->disabled && !dev->users++ && dev->open)
		retval = dev->open(dev);

	if (retval) {
		dev->users_private--;
		if (!dev->disabled)
		dev->users--;
		if (!--handle->open) {
			/*
			 * Make sure we are not delivering any more events
			 * through this handle
			 */
			synchronize_rcu();
		}
	}

 out:
	mutex_unlock(&dev->mutex);
	return retval;
}
EXPORT_SYMBOL(input_open_device);

int input_flush_device(struct input_handle *handle, struct file *file)
{
	struct input_dev *dev = handle->dev;
	int retval;

	retval = mutex_lock_interruptible(&dev->mutex);
	if (retval)
		return retval;

	if (dev->flush)
		retval = dev->flush(dev, file);

	mutex_unlock(&dev->mutex);
	return retval;
}
EXPORT_SYMBOL(input_flush_device);

/**
 * input_close_device - close input device
 * @handle: handle through which device is being accessed
 *
 * This function should be called by input handlers when they
 * want to stop receive events from given input device.
 */
void input_close_device(struct input_handle *handle)
{
	struct input_dev *dev = handle->dev;

	mutex_lock(&dev->mutex);

	__input_release_device(handle);

	--dev->users_private;
	if (!dev->disabled && !--dev->users && dev->close)
		dev->close(dev);

	if (!--handle->open) {
		/*
		 * synchronize_rcu() makes sure that input_pass_event()
		 * completed and that no more input events are delivered
		 * through this handle
		 */
		synchronize_rcu();
	}

	mutex_unlock(&dev->mutex);
}
EXPORT_SYMBOL(input_close_device);

static int input_enable_device(struct input_dev *dev)
{
	int retval;

	retval = mutex_lock_interruptible(&dev->mutex);
	if (retval)
		return retval;

	if (!dev->disabled)
		goto out;

	if (dev->users_private && dev->open) {
		retval = dev->open(dev);
		if (retval)
			goto out;
	}
	dev->users = dev->users_private;
	dev->disabled = false;

out:
	mutex_unlock(&dev->mutex);

	return retval;
}

static int input_disable_device(struct input_dev *dev)
{
	int retval;

	retval = mutex_lock_interruptible(&dev->mutex);
	if (retval)
		return retval;

	if (!dev->disabled) {
		dev->disabled = true;
		if (dev->users && dev->close)
			dev->close(dev);
		dev->users = 0;
	}

	mutex_unlock(&dev->mutex);
	return 0;
}

/*
 * Simulate keyup events for all keys that are marked as pressed.
 * The function must be called with dev->event_lock held.
 */
static void input_dev_release_keys(struct input_dev *dev)
{
	int code;

	if (is_event_supported(EV_KEY, dev->evbit, EV_MAX)) {
		for (code = 0; code <= KEY_MAX; code++) {
			if (is_event_supported(code, dev->keybit, KEY_MAX) &&
			    __test_and_clear_bit(code, dev->key)) {
				input_pass_event(dev, EV_KEY, code, 0);
			}
		}
		input_pass_event(dev, EV_SYN, SYN_REPORT, 1);
	}
}

/*
 * Prepare device for unregistering
 */
static void input_disconnect_device(struct input_dev *dev)
{
	struct input_handle *handle;

	/*
	 * Mark device as going away. Note that we take dev->mutex here
	 * not to protect access to dev->going_away but rather to ensure
	 * that there are no threads in the middle of input_open_device()
	 */
	mutex_lock(&dev->mutex);
	dev->going_away = true;
	mutex_unlock(&dev->mutex);

	spin_lock_irq(&dev->event_lock);

	/*
	 * Simulate keyup events for all pressed keys so that handlers
	 * are not left with "stuck" keys. The driver may continue
	 * generate events even after we done here but they will not
	 * reach any handlers.
	 */
	input_dev_release_keys(dev);

	list_for_each_entry(handle, &dev->h_list, d_node)
		handle->open = 0;

	spin_unlock_irq(&dev->event_lock);
}

/**
 * input_scancode_to_scalar() - converts scancode in &struct input_keymap_entry
 * @ke: keymap entry containing scancode to be converted.
 * @scancode: pointer to the location where converted scancode should
 *	be stored.
 *
 * This function is used to convert scancode stored in &struct keymap_entry
 * into scalar form understood by legacy keymap handling methods. These
 * methods expect scancodes to be represented as 'unsigned int'.
 */
int input_scancode_to_scalar(const struct input_keymap_entry *ke,
			     unsigned int *scancode)
{
	switch (ke->len) {
	case 1:
		*scancode = *((u8 *)ke->scancode);
		break;

	case 2:
		*scancode = *((u16 *)ke->scancode);
		break;

	case 4:
		*scancode = *((u32 *)ke->scancode);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(input_scancode_to_scalar);

/*
 * Those routines handle the default case where no [gs]etkeycode() is
 * defined. In this case, an array indexed by the scancode is used.
 */

static unsigned int input_fetch_keycode(struct input_dev *dev,
					unsigned int index)
{
	switch (dev->keycodesize) {
	case 1:
		return ((u8 *)dev->keycode)[index];

	case 2:
		return ((u16 *)dev->keycode)[index];

	default:
		return ((u32 *)dev->keycode)[index];
	}
}

static int input_default_getkeycode(struct input_dev *dev,
				    struct input_keymap_entry *ke)
{
	unsigned int index;
	int error;

	if (!dev->keycodesize)
		return -EINVAL;

	if (ke->flags & INPUT_KEYMAP_BY_INDEX)
		index = ke->index;
	else {
		error = input_scancode_to_scalar(ke, &index);
		if (error)
			return error;
	}

	if (index >= dev->keycodemax)
		return -EINVAL;

	ke->keycode = input_fetch_keycode(dev, index);
	ke->index = index;
	ke->len = sizeof(index);
	memcpy(ke->scancode, &index, sizeof(index));

	return 0;
}

static int input_default_setkeycode(struct input_dev *dev,
				    const struct input_keymap_entry *ke,
				    unsigned int *old_keycode)
{
	unsigned int index;
	int error;
	int i;

	if (!dev->keycodesize)
		return -EINVAL;

	if (ke->flags & INPUT_KEYMAP_BY_INDEX) {
		index = ke->index;
	} else {
		error = input_scancode_to_scalar(ke, &index);
		if (error)
			return error;
	}

	if (index >= dev->keycodemax)
		return -EINVAL;

	if (dev->keycodesize < sizeof(ke->keycode) &&
			(ke->keycode >> (dev->keycodesize * 8)))
		return -EINVAL;

	switch (dev->keycodesize) {
		case 1: {
			u8 *k = (u8 *)dev->keycode;
			*old_keycode = k[index];
			k[index] = ke->keycode;
			break;
		}
		case 2: {
			u16 *k = (u16 *)dev->keycode;
			*old_keycode = k[index];
			k[index] = ke->keycode;
			break;
		}
		default: {
			u32 *k = (u32 *)dev->keycode;
			*old_keycode = k[index];
			k[index] = ke->keycode;
			break;
		}
	}

	__clear_bit(*old_keycode, dev->keybit);
	__set_bit(ke->keycode, dev->keybit);

	for (i = 0; i < dev->keycodemax; i++) {
		if (input_fetch_keycode(dev, i) == *old_keycode) {
			__set_bit(*old_keycode, dev->keybit);
			break; /* Setting the bit twice is useless, so break */
		}
	}

	return 0;
}

/**
 * input_get_keycode - retrieve keycode currently mapped to a given scancode
 * @dev: input device which keymap is being queried
 * @ke: keymap entry
 *
 * This function should be called by anyone interested in retrieving current
 * keymap. Presently evdev handlers use it.
 */
int input_get_keycode(struct input_dev *dev, struct input_keymap_entry *ke)
{
	unsigned long flags;
	int retval;

	spin_lock_irqsave(&dev->event_lock, flags);
	retval = dev->getkeycode(dev, ke);
	spin_unlock_irqrestore(&dev->event_lock, flags);

	return retval;
}
EXPORT_SYMBOL(input_get_keycode);

/**
 * input_set_keycode - attribute a keycode to a given scancode
 * @dev: input device which keymap is being updated
 * @ke: new keymap entry
 *
 * This function should be called by anyone needing to update current
 * keymap. Presently keyboard and evdev handlers use it.
 */
int input_set_keycode(struct input_dev *dev,
		      const struct input_keymap_entry *ke)
{
	unsigned long flags;
	unsigned int old_keycode;
	int retval;

	if (ke->keycode > KEY_MAX)
		return -EINVAL;

	spin_lock_irqsave(&dev->event_lock, flags);

	retval = dev->setkeycode(dev, ke, &old_keycode);
	if (retval)
		goto out;

	/* Make sure KEY_RESERVED did not get enabled. */
	__clear_bit(KEY_RESERVED, dev->keybit);

	/*
	 * Simulate keyup event if keycode is not present
	 * in the keymap anymore
	 */
	if (test_bit(EV_KEY, dev->evbit) &&
	    !is_event_supported(old_keycode, dev->keybit, KEY_MAX) &&
	    __test_and_clear_bit(old_keycode, dev->key)) {
		struct input_value vals[] =  {
			{ EV_KEY, old_keycode, 0 },
			input_value_sync
		};

		input_pass_values(dev, vals, ARRAY_SIZE(vals));
	}

 out:
	spin_unlock_irqrestore(&dev->event_lock, flags);

	return retval;
}
EXPORT_SYMBOL(input_set_keycode);

static const struct input_device_id *input_match_device(struct input_handler *handler,
							struct input_dev *dev)
{
	const struct input_device_id *id;

	for (id = handler->id_table; id->flags || id->driver_info; id++) {

		if (id->flags & INPUT_DEVICE_ID_MATCH_BUS)
			if (id->bustype != dev->id.bustype)
				continue;

		if (id->flags & INPUT_DEVICE_ID_MATCH_VENDOR)
			if (id->vendor != dev->id.vendor)
				continue;

		if (id->flags & INPUT_DEVICE_ID_MATCH_PRODUCT)
			if (id->product != dev->id.product)
				continue;

		if (id->flags & INPUT_DEVICE_ID_MATCH_VERSION)
			if (id->version != dev->id.version)
				continue;

		if (!bitmap_subset(id->evbit, dev->evbit, EV_MAX))
			continue;

		if (!bitmap_subset(id->keybit, dev->keybit, KEY_MAX))
			continue;

		if (!bitmap_subset(id->relbit, dev->relbit, REL_MAX))
			continue;

		if (!bitmap_subset(id->absbit, dev->absbit, ABS_MAX))
			continue;

		if (!bitmap_subset(id->mscbit, dev->mscbit, MSC_MAX))
			continue;

		if (!bitmap_subset(id->ledbit, dev->ledbit, LED_MAX))
			continue;

		if (!bitmap_subset(id->sndbit, dev->sndbit, SND_MAX))
			continue;

		if (!bitmap_subset(id->ffbit, dev->ffbit, FF_MAX))
			continue;

		if (!bitmap_subset(id->swbit, dev->swbit, SW_MAX))
			continue;

		if (!handler->match || handler->match(handler, dev))
			return id;
	}

	return NULL;
}

static int input_attach_handler(struct input_dev *dev, struct input_handler *handler)
{
	const struct input_device_id *id;
	int error;

	id = input_match_device(handler, dev);
	if (!id)
		return -ENODEV;

	error = handler->connect(handler, dev, id);
	if (error && error != -ENODEV)
		pr_err("failed to attach handler %s to device %s, error: %d\n",
		       handler->name, kobject_name(&dev->dev.kobj), error);

	return error;
}

#ifdef CONFIG_COMPAT

static int input_bits_to_string(char *buf, int buf_size,
				unsigned long bits, bool skip_empty)
{
	int len = 0;

	if (INPUT_COMPAT_TEST) {
		u32 dword = bits >> 32;
		if (dword || !skip_empty)
			len += snprintf(buf, buf_size, "%x ", dword);

		dword = bits & 0xffffffffUL;
		if (dword || !skip_empty || len)
			len += snprintf(buf + len, max(buf_size - len, 0),
					"%x", dword);
	} else {
		if (bits || !skip_empty)
			len += snprintf(buf, buf_size, "%lx", bits);
	}

	return len;
}

#else /* !CONFIG_COMPAT */

static int input_bits_to_string(char *buf, int buf_size,
				unsigned long bits, bool skip_empty)
{
	return bits || !skip_empty ?
		snprintf(buf, buf_size, "%lx", bits) : 0;
}

#endif

#ifdef CONFIG_PROC_FS

static struct proc_dir_entry *proc_bus_input_dir;
static DECLARE_WAIT_QUEUE_HEAD(input_devices_poll_wait);
static int input_devices_state;

static inline void input_wakeup_procfs_readers(void)
{
	input_devices_state++;
	wake_up(&input_devices_poll_wait);
}

static unsigned int input_proc_devices_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &input_devices_poll_wait, wait);
	if (file->f_version != input_devices_state) {
		file->f_version = input_devices_state;
		return POLLIN | POLLRDNORM;
	}

	return 0;
}

union input_seq_state {
	struct {
		unsigned short pos;
		bool mutex_acquired;
	};
	void *p;
};

static void *input_devices_seq_start(struct seq_file *seq, loff_t *pos)
{
	union input_seq_state *state = (union input_seq_state *)&seq->private;
	int error;

	/* We need to fit into seq->private pointer */
	BUILD_BUG_ON(sizeof(union input_seq_state) != sizeof(seq->private));

	error = mutex_lock_interruptible(&input_mutex);
	if (error) {
		state->mutex_acquired = false;
		return ERR_PTR(error);
	}

	state->mutex_acquired = true;

	return seq_list_start(&input_dev_list, *pos);
}

static void *input_devices_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	return seq_list_next(v, &input_dev_list, pos);
}

static void input_seq_stop(struct seq_file *seq, void *v)
{
	union input_seq_state *state = (union input_seq_state *)&seq->private;

	if (state->mutex_acquired)
		mutex_unlock(&input_mutex);
}

static void input_seq_print_bitmap(struct seq_file *seq, const char *name,
				   unsigned long *bitmap, int max)
{
	int i;
	bool skip_empty = true;
	char buf[18];

	seq_printf(seq, "B: %s=", name);

	for (i = BITS_TO_LONGS(max) - 1; i >= 0; i--) {
		if (input_bits_to_string(buf, sizeof(buf),
					 bitmap[i], skip_empty)) {
			skip_empty = false;
			seq_printf(seq, "%s%s", buf, i > 0 ? " " : "");
		}
	}

	/*
	 * If no output was produced print a single 0.
	 */
	if (skip_empty)
		seq_puts(seq, "0");

	seq_putc(seq, '\n');
}

static int input_devices_seq_show(struct seq_file *seq, void *v)
{
	struct input_dev *dev = container_of(v, struct input_dev, node);
	const char *path = kobject_get_path(&dev->dev.kobj, GFP_KERNEL);
	struct input_handle *handle;

	seq_printf(seq, "I: Bus=%04x Vendor=%04x Product=%04x Version=%04x\n",
		   dev->id.bustype, dev->id.vendor, dev->id.product, dev->id.version);

	seq_printf(seq, "N: Name=\"%s\"\n", dev->name ? dev->name : "");
	seq_printf(seq, "P: Phys=%s\n", dev->phys ? dev->phys : "");
	seq_printf(seq, "S: Sysfs=%s\n", path ? path : "");
	seq_printf(seq, "U: Uniq=%s\n", dev->uniq ? dev->uniq : "");
	seq_printf(seq, "H: Handlers=");

	list_for_each_entry(handle, &dev->h_list, d_node)
		seq_printf(seq, "%s ", handle->name);
	seq_putc(seq, '\n');

	input_seq_print_bitmap(seq, "PROP", dev->propbit, INPUT_PROP_MAX);

	input_seq_print_bitmap(seq, "EV", dev->evbit, EV_MAX);
	if (test_bit(EV_KEY, dev->evbit))
		input_seq_print_bitmap(seq, "KEY", dev->keybit, KEY_MAX);
	if (test_bit(EV_REL, dev->evbit))
		input_seq_print_bitmap(seq, "REL", dev->relbit, REL_MAX);
	if (test_bit(EV_ABS, dev->evbit))
		input_seq_print_bitmap(seq, "ABS", dev->absbit, ABS_MAX);
	if (test_bit(EV_MSC, dev->evbit))
		input_seq_print_bitmap(seq, "MSC", dev->mscbit, MSC_MAX);
	if (test_bit(EV_LED, dev->evbit))
		input_seq_print_bitmap(seq, "LED", dev->ledbit, LED_MAX);
	if (test_bit(EV_SND, dev->evbit))
		input_seq_print_bitmap(seq, "SND", dev->sndbit, SND_MAX);
	if (test_bit(EV_FF, dev->evbit))
		input_seq_print_bitmap(seq, "FF", dev->ffbit, FF_MAX);
	if (test_bit(EV_SW, dev->evbit))
		input_seq_print_bitmap(seq, "SW", dev->swbit, SW_MAX);

	seq_putc(seq, '\n');

	kfree(path);
	return 0;
}

static const struct seq_operations input_devices_seq_ops = {
	.start	= input_devices_seq_start,
	.next	= input_devices_seq_next,
	.stop	= input_seq_stop,
	.show	= input_devices_seq_show,
};

static int input_proc_devices_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &input_devices_seq_ops);
}

static const struct file_operations input_devices_fileops = {
	.owner		= THIS_MODULE,
	.open		= input_proc_devices_open,
	.poll		= input_proc_devices_poll,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static void *input_handlers_seq_start(struct seq_file *seq, loff_t *pos)
{
	union input_seq_state *state = (union input_seq_state *)&seq->private;
	int error;

	/* We need to fit into seq->private pointer */
	BUILD_BUG_ON(sizeof(union input_seq_state) != sizeof(seq->private));

	error = mutex_lock_interruptible(&input_mutex);
	if (error) {
		state->mutex_acquired = false;
		return ERR_PTR(error);
	}

	state->mutex_acquired = true;
	state->pos = *pos;

	return seq_list_start(&input_handler_list, *pos);
}

static void *input_handlers_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	union input_seq_state *state = (union input_seq_state *)&seq->private;

	state->pos = *pos + 1;
	return seq_list_next(v, &input_handler_list, pos);
}

static int input_handlers_seq_show(struct seq_file *seq, void *v)
{
	struct input_handler *handler = container_of(v, struct input_handler, node);
	union input_seq_state *state = (union input_seq_state *)&seq->private;

	seq_printf(seq, "N: Number=%u Name=%s", state->pos, handler->name);
	if (handler->filter)
		seq_puts(seq, " (filter)");
	if (handler->legacy_minors)
		seq_printf(seq, " Minor=%d", handler->minor);
	seq_putc(seq, '\n');

	return 0;
}

static const struct seq_operations input_handlers_seq_ops = {
	.start	= input_handlers_seq_start,
	.next	= input_handlers_seq_next,
	.stop	= input_seq_stop,
	.show	= input_handlers_seq_show,
};

static int input_proc_handlers_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &input_handlers_seq_ops);
}

static const struct file_operations input_handlers_fileops = {
	.owner		= THIS_MODULE,
	.open		= input_proc_handlers_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static int __init input_proc_init(void)
{
	struct proc_dir_entry *entry;

	proc_bus_input_dir = proc_mkdir("bus/input", NULL);
	if (!proc_bus_input_dir)
		return -ENOMEM;

	entry = proc_create("devices", 0, proc_bus_input_dir,
			    &input_devices_fileops);
	if (!entry)
		goto fail1;

	entry = proc_create("handlers", 0, proc_bus_input_dir,
			    &input_handlers_fileops);
	if (!entry)
		goto fail2;

	return 0;

 fail2:	remove_proc_entry("devices", proc_bus_input_dir);
 fail1: remove_proc_entry("bus/input", NULL);
	return -ENOMEM;
}

static void input_proc_exit(void)
{
	remove_proc_entry("devices", proc_bus_input_dir);
	remove_proc_entry("handlers", proc_bus_input_dir);
	remove_proc_entry("bus/input", NULL);
}

#else /* !CONFIG_PROC_FS */
static inline void input_wakeup_procfs_readers(void) { }
static inline int input_proc_init(void) { return 0; }
static inline void input_proc_exit(void) { }
#endif

#define INPUT_DEV_STRING_ATTR_SHOW(name)				\
static ssize_t input_dev_show_##name(struct device *dev,		\
				     struct device_attribute *attr,	\
				     char *buf)				\
{									\
	struct input_dev *input_dev = to_input_dev(dev);		\
									\
	return scnprintf(buf, PAGE_SIZE, "%s\n",			\
			 input_dev->name ? input_dev->name : "");	\
}									\
static DEVICE_ATTR(name, S_IRUGO, input_dev_show_##name, NULL)

INPUT_DEV_STRING_ATTR_SHOW(name);
INPUT_DEV_STRING_ATTR_SHOW(phys);
INPUT_DEV_STRING_ATTR_SHOW(uniq);

static int input_print_modalias_bits(char *buf, int size,
				     char name, unsigned long *bm,
				     unsigned int min_bit, unsigned int max_bit)
{
	int len = 0, i;

	len += snprintf(buf, max(size, 0), "%c", name);
	for (i = min_bit; i < max_bit; i++)
		if (bm[BIT_WORD(i)] & BIT_MASK(i))
			len += snprintf(buf + len, max(size - len, 0), "%X,", i);
	return len;
}

static int input_print_modalias(char *buf, int size, struct input_dev *id,
				int add_cr)
{
	int len;

	len = snprintf(buf, max(size, 0),
		       "input:b%04Xv%04Xp%04Xe%04X-",
		       id->id.bustype, id->id.vendor,
		       id->id.product, id->id.version);

	len += input_print_modalias_bits(buf + len, size - len,
				'e', id->evbit, 0, EV_MAX);
	len += input_print_modalias_bits(buf + len, size - len,
				'k', id->keybit, KEY_MIN_INTERESTING, KEY_MAX);
	len += input_print_modalias_bits(buf + len, size - len,
				'r', id->relbit, 0, REL_MAX);
	len += input_print_modalias_bits(buf + len, size - len,
				'a', id->absbit, 0, ABS_MAX);
	len += input_print_modalias_bits(buf + len, size - len,
				'm', id->mscbit, 0, MSC_MAX);
	len += input_print_modalias_bits(buf + len, size - len,
				'l', id->ledbit, 0, LED_MAX);
	len += input_print_modalias_bits(buf + len, size - len,
				's', id->sndbit, 0, SND_MAX);
	len += input_print_modalias_bits(buf + len, size - len,
				'f', id->ffbit, 0, FF_MAX);
	len += input_print_modalias_bits(buf + len, size - len,
				'w', id->swbit, 0, SW_MAX);

	if (add_cr)
		len += snprintf(buf + len, max(size - len, 0), "\n");

	return len;
}

static ssize_t input_dev_show_modalias(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct input_dev *id = to_input_dev(dev);
	ssize_t len;

	len = input_print_modalias(buf, PAGE_SIZE, id, 1);

	return min_t(int, len, PAGE_SIZE);
}
static DEVICE_ATTR(modalias, S_IRUGO, input_dev_show_modalias, NULL);

static int input_print_bitmap(char *buf, int buf_size, unsigned long *bitmap,
			      int max, int add_cr);

static ssize_t input_dev_show_properties(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct input_dev *input_dev = to_input_dev(dev);
	int len = input_print_bitmap(buf, PAGE_SIZE, input_dev->propbit,
				     INPUT_PROP_MAX, true);
	return min_t(int, len, PAGE_SIZE);
}
static DEVICE_ATTR(properties, S_IRUGO, input_dev_show_properties, NULL);

static ssize_t input_dev_show_enabled(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct input_dev *input_dev = to_input_dev(dev);
	return scnprintf(buf, PAGE_SIZE, "%d\n", !input_dev->disabled);
}

static ssize_t input_dev_store_enabled(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t size)
{
	int ret;
	bool enable;
	struct input_dev *input_dev = to_input_dev(dev);

	ret = strtobool(buf, &enable);
	if (ret)
		return ret;

	if (enable)
		ret = input_enable_device(input_dev);
	else
		ret = input_disable_device(input_dev);
	if (ret)
		return ret;

	return size;
}

static DEVICE_ATTR(enabled, S_IRUGO | S_IWUSR,
		   input_dev_show_enabled, input_dev_store_enabled);

static struct attribute *input_dev_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_phys.attr,
	&dev_attr_uniq.attr,
	&dev_attr_modalias.attr,
	&dev_attr_properties.attr,
	&dev_attr_enabled.attr,
	NULL
};

static struct attribute_group input_dev_attr_group = {
	.attrs	= input_dev_attrs,
};

#define INPUT_DEV_ID_ATTR(name)						\
static ssize_t input_dev_show_id_##name(struct device *dev,		\
					struct device_attribute *attr,	\
					char *buf)			\
{									\
	struct input_dev *input_dev = to_input_dev(dev);		\
	return scnprintf(buf, PAGE_SIZE, "%04x\n", input_dev->id.name);	\
}									\
static DEVICE_ATTR(name, S_IRUGO, input_dev_show_id_##name, NULL)

INPUT_DEV_ID_ATTR(bustype);
INPUT_DEV_ID_ATTR(vendor);
INPUT_DEV_ID_ATTR(product);
INPUT_DEV_ID_ATTR(version);

static struct attribute *input_dev_id_attrs[] = {
	&dev_attr_bustype.attr,
	&dev_attr_vendor.attr,
	&dev_attr_product.attr,
	&dev_attr_version.attr,
	NULL
};

static struct attribute_group input_dev_id_attr_group = {
	.name	= "id",
	.attrs	= input_dev_id_attrs,
};

static int input_print_bitmap(char *buf, int buf_size, unsigned long *bitmap,
			      int max, int add_cr)
{
	int i;
	int len = 0;
	bool skip_empty = true;

	for (i = BITS_TO_LONGS(max) - 1; i >= 0; i--) {
		len += input_bits_to_string(buf + len, max(buf_size - len, 0),
					    bitmap[i], skip_empty);
		if (len) {
			skip_empty = false;
			if (i > 0)
				len += snprintf(buf + len, max(buf_size - len, 0), " ");
		}
	}

	/*
	 * If no output was produced print a single 0.
	 */
	if (len == 0)
		len = snprintf(buf, buf_size, "%d", 0);

	if (add_cr)
		len += snprintf(buf + len, max(buf_size - len, 0), "\n");

	return len;
}

#define INPUT_DEV_CAP_ATTR(ev, bm)					\
static ssize_t input_dev_show_cap_##bm(struct device *dev,		\
				       struct device_attribute *attr,	\
				       char *buf)			\
{									\
	struct input_dev *input_dev = to_input_dev(dev);		\
	int len = input_print_bitmap(buf, PAGE_SIZE,			\
				     input_dev->bm##bit, ev##_MAX,	\
				     true);				\
	return min_t(int, len, PAGE_SIZE);				\
}									\
static DEVICE_ATTR(bm, S_IRUGO, input_dev_show_cap_##bm, NULL)

INPUT_DEV_CAP_ATTR(EV, ev);
INPUT_DEV_CAP_ATTR(KEY, key);
INPUT_DEV_CAP_ATTR(REL, rel);
INPUT_DEV_CAP_ATTR(ABS, abs);
INPUT_DEV_CAP_ATTR(MSC, msc);
INPUT_DEV_CAP_ATTR(LED, led);
INPUT_DEV_CAP_ATTR(SND, snd);
INPUT_DEV_CAP_ATTR(FF, ff);
INPUT_DEV_CAP_ATTR(SW, sw);

static struct attribute *input_dev_caps_attrs[] = {
	&dev_attr_ev.attr,
	&dev_attr_key.attr,
	&dev_attr_rel.attr,
	&dev_attr_abs.attr,
	&dev_attr_msc.attr,
	&dev_attr_led.attr,
	&dev_attr_snd.attr,
	&dev_attr_ff.attr,
	&dev_attr_sw.attr,
	NULL
};

static struct attribute_group input_dev_caps_attr_group = {
	.name	= "capabilities",
	.attrs	= input_dev_caps_attrs,
};

static const struct attribute_group *input_dev_attr_groups[] = {
	&input_dev_attr_group,
	&input_dev_id_attr_group,
	&input_dev_caps_attr_group,
	NULL
};

static void input_dev_release(struct device *device)
{
	struct input_dev *dev = to_input_dev(device);

	input_ff_destroy(dev);
	input_mt_destroy_slots(dev);
	kfree(dev->absinfo);
	kfree(dev->vals);
	kfree(dev);

	module_put(THIS_MODULE);
}

/*
 * Input uevent interface - loading event handlers based on
 * device bitfields.
 */
static int input_add_uevent_bm_var(struct kobj_uevent_env *env,
				   const char *name, unsigned long *bitmap, int max)
{
	int len;

	if (add_uevent_var(env, "%s", name))
		return -ENOMEM;

	len = input_print_bitmap(&env->buf[env->buflen - 1],
				 sizeof(env->buf) - env->buflen,
				 bitmap, max, false);
	if (len >= (sizeof(env->buf) - env->buflen))
		return -ENOMEM;

	env->buflen += len;
	return 0;
}

static int input_add_uevent_modalias_var(struct kobj_uevent_env *env,
					 struct input_dev *dev)
{
	int len;

	if (add_uevent_var(env, "MODALIAS="))
		return -ENOMEM;

	len = input_print_modalias(&env->buf[env->buflen - 1],
				   sizeof(env->buf) - env->buflen,
				   dev, 0);
	if (len >= (sizeof(env->buf) - env->buflen))
		return -ENOMEM;

	env->buflen += len;
	return 0;
}

#define INPUT_ADD_HOTPLUG_VAR(fmt, val...)				\
	do {								\
		int err = add_uevent_var(env, fmt, val);		\
		if (err)						\
			return err;					\
	} while (0)

#define INPUT_ADD_HOTPLUG_BM_VAR(name, bm, max)				\
	do {								\
		int err = input_add_uevent_bm_var(env, name, bm, max);	\
		if (err)						\
			return err;					\
	} while (0)

#define INPUT_ADD_HOTPLUG_MODALIAS_VAR(dev)				\
	do {								\
		int err = input_add_uevent_modalias_var(env, dev);	\
		if (err)						\
			return err;					\
	} while (0)

static int input_dev_uevent(struct device *device, struct kobj_uevent_env *env)
{
	struct input_dev *dev = to_input_dev(device);

	INPUT_ADD_HOTPLUG_VAR("PRODUCT=%x/%x/%x/%x",
				dev->id.bustype, dev->id.vendor,
				dev->id.product, dev->id.version);
	if (dev->name)
		INPUT_ADD_HOTPLUG_VAR("NAME=\"%s\"", dev->name);
	if (dev->phys)
		INPUT_ADD_HOTPLUG_VAR("PHYS=\"%s\"", dev->phys);
	if (dev->uniq)
		INPUT_ADD_HOTPLUG_VAR("UNIQ=\"%s\"", dev->uniq);

	INPUT_ADD_HOTPLUG_BM_VAR("PROP=", dev->propbit, INPUT_PROP_MAX);

	INPUT_ADD_HOTPLUG_BM_VAR("EV=", dev->evbit, EV_MAX);
	if (test_bit(EV_KEY, dev->evbit))
		INPUT_ADD_HOTPLUG_BM_VAR("KEY=", dev->keybit, KEY_MAX);
	if (test_bit(EV_REL, dev->evbit))
		INPUT_ADD_HOTPLUG_BM_VAR("REL=", dev->relbit, REL_MAX);
	if (test_bit(EV_ABS, dev->evbit))
		INPUT_ADD_HOTPLUG_BM_VAR("ABS=", dev->absbit, ABS_MAX);
	if (test_bit(EV_MSC, dev->evbit))
		INPUT_ADD_HOTPLUG_BM_VAR("MSC=", dev->mscbit, MSC_MAX);
	if (test_bit(EV_LED, dev->evbit))
		INPUT_ADD_HOTPLUG_BM_VAR("LED=", dev->ledbit, LED_MAX);
	if (test_bit(EV_SND, dev->evbit))
		INPUT_ADD_HOTPLUG_BM_VAR("SND=", dev->sndbit, SND_MAX);
	if (test_bit(EV_FF, dev->evbit))
		INPUT_ADD_HOTPLUG_BM_VAR("FF=", dev->ffbit, FF_MAX);
	if (test_bit(EV_SW, dev->evbit))
		INPUT_ADD_HOTPLUG_BM_VAR("SW=", dev->swbit, SW_MAX);

	INPUT_ADD_HOTPLUG_MODALIAS_VAR(dev);

	return 0;
}

#define INPUT_DO_TOGGLE(dev, type, bits, on)				\
	do {								\
		int i;							\
		bool active;						\
									\
		if (!test_bit(EV_##type, dev->evbit))			\
			break;						\
									\
		for (i = 0; i < type##_MAX; i++) {			\
			if (!test_bit(i, dev->bits##bit))		\
				continue;				\
									\
			active = test_bit(i, dev->bits);		\
			if (!active && !on)				\
				continue;				\
									\
			dev->event(dev, EV_##type, i, on ? active : 0);	\
		}							\
	} while (0)

static void input_dev_toggle(struct input_dev *dev, bool activate)
{
	if (!dev->event)
		return;

	INPUT_DO_TOGGLE(dev, LED, led, activate);
	INPUT_DO_TOGGLE(dev, SND, snd, activate);

	if (activate && test_bit(EV_REP, dev->evbit)) {
		dev->event(dev, EV_REP, REP_PERIOD, dev->rep[REP_PERIOD]);
		dev->event(dev, EV_REP, REP_DELAY, dev->rep[REP_DELAY]);
	}
}

/**
 * input_reset_device() - reset/restore the state of input device
 * @dev: input device whose state needs to be reset
 *
 * This function tries to reset the state of an opened input device and
 * bring internal state and state if the hardware in sync with each other.
 * We mark all keys as released, restore LED state, repeat rate, etc.
 */
void input_reset_device(struct input_dev *dev)
{
	mutex_lock(&dev->mutex);

	if (dev->users) {
		input_dev_toggle(dev, true);

		/*
		 * Keys that have been pressed at suspend time are unlikely
		 * to be still pressed when we resume.
		 */
/*		spin_lock_irq(&dev->event_lock);
		input_dev_release_keys(dev);
		spin_unlock_irq(&dev->event_lock);*/
	}

	mutex_unlock(&dev->mutex);
}
EXPORT_SYMBOL(input_reset_device);

#ifdef CONFIG_PM
static int input_dev_suspend(struct device *dev)
{
	struct input_dev *input_dev = to_input_dev(dev);

	mutex_lock(&input_dev->mutex);

	if (input_dev->users)
		input_dev_toggle(input_dev, false);

	mutex_unlock(&input_dev->mutex);

	return 0;
}

static int input_dev_resume(struct device *dev)
{
	struct input_dev *input_dev = to_input_dev(dev);

	input_reset_device(input_dev);

	return 0;
}

static const struct dev_pm_ops input_dev_pm_ops = {
	.suspend	= input_dev_suspend,
	.resume		= input_dev_resume,
	.poweroff	= input_dev_suspend,
	.restore	= input_dev_resume,
};
#endif /* CONFIG_PM */

static struct device_type input_dev_type = {
	.groups		= input_dev_attr_groups,
	.release	= input_dev_release,
	.uevent		= input_dev_uevent,
#ifdef CONFIG_PM
	.pm		= &input_dev_pm_ops,
#endif
};

static char *input_devnode(struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "input/%s", dev_name(dev));
}

struct class input_class = {
	.name		= "input",
	.devnode	= input_devnode,
};
EXPORT_SYMBOL_GPL(input_class);

/**
 * input_allocate_device - allocate memory for new input device
 *
 * Returns prepared struct input_dev or %NULL.
 *
 * NOTE: Use input_free_device() to free devices that have not been
 * registered; input_unregister_device() should be used for already
 * registered devices.
 */
struct input_dev *input_allocate_device(void)
{
	struct input_dev *dev;

	dev = kzalloc(sizeof(struct input_dev), GFP_KERNEL);
	if (dev) {
		dev->dev.type = &input_dev_type;
		dev->dev.class = &input_class;
		device_initialize(&dev->dev);
		mutex_init(&dev->mutex);
		spin_lock_init(&dev->event_lock);
		INIT_LIST_HEAD(&dev->h_list);
		INIT_LIST_HEAD(&dev->node);

		__module_get(THIS_MODULE);
	}

	return dev;
}
EXPORT_SYMBOL(input_allocate_device);

struct input_devres {
	struct input_dev *input;
};

static int devm_input_device_match(struct device *dev, void *res, void *data)
{
	struct input_devres *devres = res;

	return devres->input == data;
}

static void devm_input_device_release(struct device *dev, void *res)
{
	struct input_devres *devres = res;
	struct input_dev *input = devres->input;

	dev_dbg(dev, "%s: dropping reference to %s\n",
		__func__, dev_name(&input->dev));
	input_put_device(input);
}

/**
 * devm_input_allocate_device - allocate managed input device
 * @dev: device owning the input device being created
 *
 * Returns prepared struct input_dev or %NULL.
 *
 * Managed input devices do not need to be explicitly unregistered or
 * freed as it will be done automatically when owner device unbinds from
 * its driver (or binding fails). Once managed input device is allocated,
 * it is ready to be set up and registered in the same fashion as regular
 * input device. There are no special devm_input_device_[un]register()
 * variants, regular ones work with both managed and unmanaged devices,
 * should you need them. In most cases however, managed input device need
 * not be explicitly unregistered or freed.
 *
 * NOTE: the owner device is set up as parent of input device and users
 * should not override it.
 */
struct input_dev *devm_input_allocate_device(struct device *dev)
{
	struct input_dev *input;
	struct input_devres *devres;

	devres = devres_alloc(devm_input_device_release,
			      sizeof(struct input_devres), GFP_KERNEL);
	if (!devres)
		return NULL;

	input = input_allocate_device();
	if (!input) {
		devres_free(devres);
		return NULL;
	}

	input->dev.parent = dev;
	input->devres_managed = true;

	devres->input = input;
	devres_add(dev, devres);

	return input;
}
EXPORT_SYMBOL(devm_input_allocate_device);

/**
 * input_free_device - free memory occupied by input_dev structure
 * @dev: input device to free
 *
 * This function should only be used if input_register_device()
 * was not called yet or if it failed. Once device was registered
 * use input_unregister_device() and memory will be freed once last
 * reference to the device is dropped.
 *
 * Device should be allocated by input_allocate_device().
 *
 * NOTE: If there are references to the input device then memory
 * will not be freed until last reference is dropped.
 */
void input_free_device(struct input_dev *dev)
{
	if (dev) {
		if (dev->devres_managed)
			WARN_ON(devres_destroy(dev->dev.parent,
						devm_input_device_release,
						devm_input_device_match,
						dev));
		input_put_device(dev);
	}
}
EXPORT_SYMBOL(input_free_device);

/**
 * input_set_capability - mark device as capable of a certain event
 * @dev: device that is capable of emitting or accepting event
 * @type: type of the event (EV_KEY, EV_REL, etc...)
 * @code: event code
 *
 * In addition to setting up corresponding bit in appropriate capability
 * bitmap the function also adjusts dev->evbit.
 */
void input_set_capability(struct input_dev *dev, unsigned int type, unsigned int code)
{
	switch (type) {
	case EV_KEY:
		__set_bit(code, dev->keybit);
		break;

	case EV_REL:
		__set_bit(code, dev->relbit);
		break;

	case EV_ABS:
		__set_bit(code, dev->absbit);
		break;

	case EV_MSC:
		__set_bit(code, dev->mscbit);
		break;

	case EV_SW:
		__set_bit(code, dev->swbit);
		break;

	case EV_LED:
		__set_bit(code, dev->ledbit);
		break;

	case EV_SND:
		__set_bit(code, dev->sndbit);
		break;

	case EV_FF:
		__set_bit(code, dev->ffbit);
		break;

	case EV_PWR:
		/* do nothing */
		break;

	default:
		pr_err("input_set_capability: unknown type %u (code %u)\n",
		       type, code);
		dump_stack();
		return;
	}

	__set_bit(type, dev->evbit);
}
EXPORT_SYMBOL(input_set_capability);

static unsigned int input_estimate_events_per_packet(struct input_dev *dev)
{
	int mt_slots;
	int i;
	unsigned int events;

	if (dev->mt) {
		mt_slots = dev->mt->num_slots;
	} else if (test_bit(ABS_MT_TRACKING_ID, dev->absbit)) {
		mt_slots = dev->absinfo[ABS_MT_TRACKING_ID].maximum -
			   dev->absinfo[ABS_MT_TRACKING_ID].minimum + 1,
		mt_slots = clamp(mt_slots, 2, 32);
	} else if (test_bit(ABS_MT_POSITION_X, dev->absbit)) {
		mt_slots = 2;
	} else {
		mt_slots = 0;
	}

	events = mt_slots + 1; /* count SYN_MT_REPORT and SYN_REPORT */

	for (i = 0; i < ABS_CNT; i++) {
		if (test_bit(i, dev->absbit)) {
			if (input_is_mt_axis(i))
				events += mt_slots;
			else
				events++;
		}
	}

	for (i = 0; i < REL_CNT; i++)
		if (test_bit(i, dev->relbit))
			events++;

	/* Make room for KEY and MSC events */
	events += 7;

	return events;
}

#define INPUT_CLEANSE_BITMASK(dev, type, bits)				\
	do {								\
		if (!test_bit(EV_##type, dev->evbit))			\
			memset(dev->bits##bit, 0,			\
				sizeof(dev->bits##bit));		\
	} while (0)

static void input_cleanse_bitmasks(struct input_dev *dev)
{
	INPUT_CLEANSE_BITMASK(dev, KEY, key);
	INPUT_CLEANSE_BITMASK(dev, REL, rel);
	INPUT_CLEANSE_BITMASK(dev, ABS, abs);
	INPUT_CLEANSE_BITMASK(dev, MSC, msc);
	INPUT_CLEANSE_BITMASK(dev, LED, led);
	INPUT_CLEANSE_BITMASK(dev, SND, snd);
	INPUT_CLEANSE_BITMASK(dev, FF, ff);
	INPUT_CLEANSE_BITMASK(dev, SW, sw);
}

static void __input_unregister_device(struct input_dev *dev)
{
	struct input_handle *handle, *next;

	input_disconnect_device(dev);

	mutex_lock(&input_mutex);

	list_for_each_entry_safe(handle, next, &dev->h_list, d_node)
		handle->handler->disconnect(handle);
	WARN_ON(!list_empty(&dev->h_list));

	del_timer_sync(&dev->timer);
	list_del_init(&dev->node);

	input_wakeup_procfs_readers();

	mutex_unlock(&input_mutex);

	device_del(&dev->dev);
}

static void devm_input_device_unregister(struct device *dev, void *res)
{
	struct input_devres *devres = res;
	struct input_dev *input = devres->input;

	dev_dbg(dev, "%s: unregistering device %s\n",
		__func__, dev_name(&input->dev));
	__input_unregister_device(input);
}

/**
 * input_register_device - register device with input core
 * @dev: device to be registered
 *
 * This function registers device with input core. The device must be
 * allocated with input_allocate_device() and all it's capabilities
 * set up before registering.
 * If function fails the device must be freed with input_free_device().
 * Once device has been successfully registered it can be unregistered
 * with input_unregister_device(); input_free_device() should not be
 * called in this case.
 *
 * Note that this function is also used to register managed input devices
 * (ones allocated with devm_input_allocate_device()). Such managed input
 * devices need not be explicitly unregistered or freed, their tear down
 * is controlled by the devres infrastructure. It is also worth noting
 * that tear down of managed input devices is internally a 2-step process:
 * registered managed input device is first unregistered, but stays in
 * memory and can still handle input_event() calls (although events will
 * not be delivered anywhere). The freeing of managed input device will
 * happen later, when devres stack is unwound to the point where device
 * allocation was made.
 */
int input_register_device(struct input_dev *dev)
{
	static atomic_t input_no = ATOMIC_INIT(0);
	struct input_devres *devres = NULL;
	struct input_handler *handler;
	unsigned int packet_size;
	const char *path;
	int error;

	if (dev->devres_managed) {
		devres = devres_alloc(devm_input_device_unregister,
				      sizeof(struct input_devres), GFP_KERNEL);
		if (!devres)
			return -ENOMEM;

		devres->input = dev;
	}

	/* Every input device generates EV_SYN/SYN_REPORT events. */
	__set_bit(EV_SYN, dev->evbit);

	/* KEY_RESERVED is not supposed to be transmitted to userspace. */
	__clear_bit(KEY_RESERVED, dev->keybit);

	/* Make sure that bitmasks not mentioned in dev->evbit are clean. */
	input_cleanse_bitmasks(dev);

	packet_size = input_estimate_events_per_packet(dev);
	if (dev->hint_events_per_packet < packet_size)
		dev->hint_events_per_packet = packet_size;

	dev->max_vals = max(dev->hint_events_per_packet, packet_size) + 2;
	dev->vals = kcalloc(dev->max_vals, sizeof(*dev->vals), GFP_KERNEL);
	if (!dev->vals) {
		error = -ENOMEM;
		goto err_devres_free;
	}

	/*
	 * If delay and period are pre-set by the driver, then autorepeating
	 * is handled by the driver itself and we don't do it in input.c.
	 */
	init_timer(&dev->timer);
	if (!dev->rep[REP_DELAY] && !dev->rep[REP_PERIOD]) {
		dev->timer.data = (long) dev;
		dev->timer.function = input_repeat_key;
		dev->rep[REP_DELAY] = 250;
		dev->rep[REP_PERIOD] = 33;
	}

	if (!dev->getkeycode)
		dev->getkeycode = input_default_getkeycode;

	if (!dev->setkeycode)
		dev->setkeycode = input_default_setkeycode;

	dev_set_name(&dev->dev, "input%ld",
		     (unsigned long) atomic_inc_return(&input_no) - 1);

	error = device_add(&dev->dev);
	if (error)
		goto err_free_vals;

	path = kobject_get_path(&dev->dev.kobj, GFP_KERNEL);
	pr_info("%s as %s\n",
		dev->name ? dev->name : "Unspecified device",
		path ? path : "N/A");
	kfree(path);

	error = mutex_lock_interruptible(&input_mutex);
	if (error)
		goto err_device_del;

	list_add_tail(&dev->node, &input_dev_list);

	list_for_each_entry(handler, &input_handler_list, node)
		input_attach_handler(dev, handler);

	input_wakeup_procfs_readers();

	mutex_unlock(&input_mutex);

	if (dev->devres_managed) {
		dev_dbg(dev->dev.parent, "%s: registering %s with devres.\n",
			__func__, dev_name(&dev->dev));
		devres_add(dev->dev.parent, devres);
	}
	return 0;

err_device_del:
	device_del(&dev->dev);
err_free_vals:
	kfree(dev->vals);
	dev->vals = NULL;
err_devres_free:
	devres_free(devres);
	return error;
}
EXPORT_SYMBOL(input_register_device);

/**
 * input_unregister_device - unregister previously registered device
 * @dev: device to be unregistered
 *
 * This function unregisters an input device. Once device is unregistered
 * the caller should not try to access it as it may get freed at any moment.
 */
void input_unregister_device(struct input_dev *dev)
{
	if (dev->devres_managed) {
		WARN_ON(devres_destroy(dev->dev.parent,
					devm_input_device_unregister,
					devm_input_device_match,
					dev));
		__input_unregister_device(dev);
		/*
		 * We do not do input_put_device() here because it will be done
		 * when 2nd devres fires up.
		 */
	} else {
		__input_unregister_device(dev);
		input_put_device(dev);
	}
}
EXPORT_SYMBOL(input_unregister_device);

/**
 * input_register_handler - register a new input handler
 * @handler: handler to be registered
 *
 * This function registers a new input handler (interface) for input
 * devices in the system and attaches it to all input devices that
 * are compatible with the handler.
 */
int input_register_handler(struct input_handler *handler)
{
	struct input_dev *dev;
	int error;

	error = mutex_lock_interruptible(&input_mutex);
	if (error)
		return error;

	INIT_LIST_HEAD(&handler->h_list);

	list_add_tail(&handler->node, &input_handler_list);

	list_for_each_entry(dev, &input_dev_list, node)
		input_attach_handler(dev, handler);

	input_wakeup_procfs_readers();

	mutex_unlock(&input_mutex);
	return 0;
}
EXPORT_SYMBOL(input_register_handler);

/**
 * input_unregister_handler - unregisters an input handler
 * @handler: handler to be unregistered
 *
 * This function disconnects a handler from its input devices and
 * removes it from lists of known handlers.
 */
void input_unregister_handler(struct input_handler *handler)
{
	struct input_handle *handle, *next;

	mutex_lock(&input_mutex);

	list_for_each_entry_safe(handle, next, &handler->h_list, h_node)
		handler->disconnect(handle);
	WARN_ON(!list_empty(&handler->h_list));

	list_del_init(&handler->node);

	input_wakeup_procfs_readers();

	mutex_unlock(&input_mutex);
}
EXPORT_SYMBOL(input_unregister_handler);

/**
 * input_handler_for_each_handle - handle iterator
 * @handler: input handler to iterate
 * @data: data for the callback
 * @fn: function to be called for each handle
 *
 * Iterate over @bus's list of devices, and call @fn for each, passing
 * it @data and stop when @fn returns a non-zero value. The function is
 * using RCU to traverse the list and therefore may be usind in atonic
 * contexts. The @fn callback is invoked from RCU critical section and
 * thus must not sleep.
 */
int input_handler_for_each_handle(struct input_handler *handler, void *data,
				  int (*fn)(struct input_handle *, void *))
{
	struct input_handle *handle;
	int retval = 0;

	rcu_read_lock();

	list_for_each_entry_rcu(handle, &handler->h_list, h_node) {
		retval = fn(handle, data);
		if (retval)
			break;
	}

	rcu_read_unlock();

	return retval;
}
EXPORT_SYMBOL(input_handler_for_each_handle);

/**
 * input_register_handle - register a new input handle
 * @handle: handle to register
 *
 * This function puts a new input handle onto device's
 * and handler's lists so that events can flow through
 * it once it is opened using input_open_device().
 *
 * This function is supposed to be called from handler's
 * connect() method.
 */
int input_register_handle(struct input_handle *handle)
{
	struct input_handler *handler = handle->handler;
	struct input_dev *dev = handle->dev;
	int error;

	/*
	 * We take dev->mutex here to prevent race with
	 * input_release_device().
	 */
	error = mutex_lock_interruptible(&dev->mutex);
	if (error)
		return error;

	/*
	 * Filters go to the head of the list, normal handlers
	 * to the tail.
	 */
	if (handler->filter)
		list_add_rcu(&handle->d_node, &dev->h_list);
	else
		list_add_tail_rcu(&handle->d_node, &dev->h_list);

	mutex_unlock(&dev->mutex);

	/*
	 * Since we are supposed to be called from ->connect()
	 * which is mutually exclusive with ->disconnect()
	 * we can't be racing with input_unregister_handle()
	 * and so separate lock is not needed here.
	 */
	list_add_tail_rcu(&handle->h_node, &handler->h_list);

	if (handler->start)
		handler->start(handle);

	return 0;
}
EXPORT_SYMBOL(input_register_handle);

/**
 * input_unregister_handle - unregister an input handle
 * @handle: handle to unregister
 *
 * This function removes input handle from device's
 * and handler's lists.
 *
 * This function is supposed to be called from handler's
 * disconnect() method.
 */
void input_unregister_handle(struct input_handle *handle)
{
	struct input_dev *dev = handle->dev;

	list_del_rcu(&handle->h_node);

	/*
	 * Take dev->mutex to prevent race with input_release_device().
	 */
	mutex_lock(&dev->mutex);
	list_del_rcu(&handle->d_node);
	mutex_unlock(&dev->mutex);

	synchronize_rcu();
}
EXPORT_SYMBOL(input_unregister_handle);

/**
 * input_get_new_minor - allocates a new input minor number
 * @legacy_base: beginning or the legacy range to be searched
 * @legacy_num: size of legacy range
 * @allow_dynamic: whether we can also take ID from the dynamic range
 *
 * This function allocates a new device minor for from input major namespace.
 * Caller can request legacy minor by specifying @legacy_base and @legacy_num
 * parameters and whether ID can be allocated from dynamic range if there are
 * no free IDs in legacy range.
 */
int input_get_new_minor(int legacy_base, unsigned int legacy_num,
			bool allow_dynamic)
{
	/*
	 * This function should be called from input handler's ->connect()
	 * methods, which are serialized with input_mutex, so no additional
	 * locking is needed here.
	 */
	if (legacy_base >= 0) {
		int minor = ida_simple_get(&input_ida,
					   legacy_base,
					   legacy_base + legacy_num,
					   GFP_KERNEL);
		if (minor >= 0 || !allow_dynamic)
			return minor;
	}

	return ida_simple_get(&input_ida,
			      INPUT_FIRST_DYNAMIC_DEV, INPUT_MAX_CHAR_DEVICES,
			      GFP_KERNEL);
}
EXPORT_SYMBOL(input_get_new_minor);

/**
 * input_free_minor - release previously allocated minor
 * @minor: minor to be released
 *
 * This function releases previously allocated input minor so that it can be
 * reused later.
 */
void input_free_minor(unsigned int minor)
{
	ida_simple_remove(&input_ida, minor);
}
EXPORT_SYMBOL(input_free_minor);

static int __init input_init(void)
{
	int err;

	err = class_register(&input_class);
	if (err) {
		pr_err("unable to register input_dev class\n");
		return err;
	}

	err = input_proc_init();
	if (err)
		goto fail1;

	err = register_chrdev_region(MKDEV(INPUT_MAJOR, 0),
				     INPUT_MAX_CHAR_DEVICES, "input");
	if (err) {
		pr_err("unable to register char major %d", INPUT_MAJOR);
		goto fail2;
	}

#if !defined(CONFIG_INPUT_BOOSTER) // Input Booster +
	input_booster_init();
#endif  // Input Booster -

	return 0;

 fail2:	input_proc_exit();
 fail1:	class_unregister(&input_class);
	return err;
}

static void __exit input_exit(void)
{
	input_proc_exit();
	unregister_chrdev_region(MKDEV(INPUT_MAJOR, 0),
				 INPUT_MAX_CHAR_DEVICES);
	class_unregister(&input_class);
}

subsys_initcall(input_init);
module_exit(input_exit);
