/*
 *  s2mu005_fuelgauge.c
 *  Samsung S2MU005 Fuel Gauge Driver
 *
 *  Copyright (C) 2012 Samsung Electronics
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define DEBUG

#define SINGLE_BYTE	1
#define REGISTER_DEBUG 0

#include <linux/battery/fuelgauge/s2mu005_fuelgauge.h>
#include <linux/of_gpio.h>

static enum power_supply_property s2mu005_fuelgauge_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TEMP_AMBIENT,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
};


static int s2mu005_write_reg_byte(struct i2c_client *client, int reg, u8 data)
{
	int ret, i = 0;

	ret = i2c_smbus_write_byte_data(client, reg,  data);
	if (ret < 0) {
		for (i = 0; i < 3; i++) {
			ret = i2c_smbus_write_byte_data(client, reg,  data);
			if (ret >= 0)
				break;
		}

		if (i >= 3)
			dev_err(&client->dev, "%s: Error(%d)\n", __func__, ret);
	}

	return ret;
}



static int s2mu005_write_reg(struct i2c_client *client, int reg, u8 *buf)
{
#if SINGLE_BYTE
	int ret = 0 ;
	s2mu005_write_reg_byte(client, reg, buf[0]);
	s2mu005_write_reg_byte(client, reg+1, buf[1]);
#else
	int ret, i = 0;

	ret = i2c_smbus_write_i2c_block_data(client, reg, 2, buf);
	if (ret < 0) {
		for (i = 0; i < 3; i++) {
			ret = i2c_smbus_write_i2c_block_data(client, reg, 2, buf);
			if (ret >= 0)
				break;
		}

		if (i >= 3)
			dev_err(&client->dev, "%s: Error(%d)\n", __func__, ret);
	}
#endif
	return ret;
}

static int s2mu005_read_reg_byte(struct i2c_client *client, int reg, void *data)
{
	int ret = 0;

	ret = i2c_smbus_read_byte_data(client, reg);
	if (ret < 0)
		return ret;
		*(u8 *)data = (u8)ret;

	return ret;
}

static int s2mu005_read_reg(struct i2c_client *client, int reg, u8 *buf)
{

#if SINGLE_BYTE
	int ret =0;
	u8 data1 = 0 , data2 = 0;
	s2mu005_read_reg_byte(client, reg, &data1);
	s2mu005_read_reg_byte(client, reg+1, &data2);
	buf[0] = data1;
	buf[1] = data2; 
#else
	int ret = 0, i = 0;

	ret = i2c_smbus_read_i2c_block_data(client, reg, 2, buf);
	if (ret < 0) {
		for (i = 0; i < 3; i++) {
			ret = i2c_smbus_read_i2c_block_data(client, reg, 2, buf);
			if (ret >= 0)
				break;
		}

		if (i >= 3)
			dev_err(&client->dev, "%s: Error(%d)\n", __func__, ret);
	}
#endif
	return ret;
}

#if REGISTER_DEBUG
static int b_first_enter=0;
static int c_restart=0;

static void u005register_work(struct work_struct *work)
{
	struct s2mu005_fuelgauge_data *fuelgauge =
		container_of(work, struct s2mu005_fuelgauge_data , register_work.work);
	u8 data[2];

	if(b_first_enter == 0 ) {
		if(c_restart > 15) {
//			s2mu005_write_reg_byte(fuelgauge->i2c, 0x1e, 0x0f);
			b_first_enter = 1;
		} else {
			c_restart++;
		}
	}

	s2mu005_read_reg(fuelgauge->i2c, S2MU005_REG_RSOC, data);
	dev_err(&fuelgauge->i2c->dev, "[DEBUG]%s: rsoc data0 (%02x) data1 (%02x) \n", __func__, data[0], data[1]);

	queue_delayed_work(fuelgauge->register_wq,&fuelgauge->register_work, 1000);
	return;
}
#endif

static int s2mu005_init_regs(struct s2mu005_fuelgauge_data *fuelgauge)
{
	int ret = 0;

#if REGISTER_DEBUG
	queue_delayed_work(fuelgauge->register_wq,&fuelgauge->register_work, 100);
#endif

	pr_err("%s: s2mu005 fuelgauge initialize\n", __func__);
	pr_err("%s: ==========================================\n", __func__);

	return ret;
}

static void s2mu005_alert_init(struct s2mu005_fuelgauge_data *fuelgauge)
{
	u8 data[2];

	/* VBAT Threshold setting */
	data[0] = 0x00 & 0x0f;

	/* SOC Threshold setting */
	data[0] = data[0] | (fuelgauge->pdata->fuel_alert_soc << 4);

	data[1] = 0x00;
	s2mu005_write_reg(fuelgauge->i2c, S2MU005_REG_IRQ_LVL, data);
}

static bool s2mu005_check_status(struct i2c_client *client)
{
	u8 data[2];
	bool ret = false;

	/* check if Smn was generated */
	if (s2mu005_read_reg(client, S2MU005_REG_STATUS, data) < 0)
		return ret;

	dev_dbg(&client->dev, "%s: status to (%02x%02x)\n",
		__func__, data[1], data[0]);

	if (data[1] & (0x1 << 1))
		return true;
	else
		return false;
}

static int s2mu005_set_temperature(struct s2mu005_fuelgauge_data *fuelgauge,
			int temperature)
{
	/*
	 * s5mu005 include temperature sensor so,
	 * do not need to set temperature value.
	 */
	return temperature;
}

static int s2mu005_get_temperature(struct s2mu005_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	s32 temperature = 0;

	/*
	 *  use monitor regiser.
	 *  monitor register default setting is temperature
	 */
	if (s2mu005_read_reg(fuelgauge->i2c, S2MU005_REG_MONOUT, data) < 0)
		return -ERANGE;

	/* data[] store 2's compliment format number */
	if (data[0] & (0x1 << 7)) {
		/* Negative */
		temperature = ((~(data[0])) & 0xFF) + 1;
		temperature *= -10;
	} else {
		temperature = data[0] & 0x7F;
		temperature *= 10;
	}

	dev_dbg(&fuelgauge->i2c->dev, "%s: temperature (%d)\n",
		__func__, temperature);

	return temperature;
}

/* soc should be 0.01% unit */
static int s2mu005_get_soc(struct s2mu005_fuelgauge_data *fuelgauge)
{
	u8 data[2], check_data[2];
	u16 compliment;
	int rsoc, i;

	for (i = 0; i < 50; i++) {
		if (s2mu005_read_reg(fuelgauge->i2c, S2MU005_REG_RSOC, data) < 0)
			return -EINVAL;
		if (s2mu005_read_reg(fuelgauge->i2c, S2MU005_REG_RSOC, check_data) < 0)
			return -EINVAL;
	dev_err(&fuelgauge->i2c->dev, "[DEBUG]%s: data0 (%d) data1 (%d) \n", __func__, data[0], data[1]);

	if ((data[0] == check_data[0]) && (data[1] == check_data[1]))
			break;
	}

	dev_err(&fuelgauge->i2c->dev, "[DEBUG]%s: data0 (%d) data1 (%d) \n", __func__, data[0], data[1]);
	compliment = (data[1] << 8) | (data[0]);

	/* data[] store 2's compliment format number */
	if (compliment & (0x1 << 15)) {
		/* Negative */
		rsoc = ((~compliment) & 0xFFFF) + 1;
		rsoc = (rsoc * (-10000)) / (0x1 << 14);
	} else {
		rsoc = compliment & 0x7FFF;
		rsoc = ((rsoc * 10000) / (0x1 << 14));
	}

	dev_err(&fuelgauge->i2c->dev, "[DEBUG]%s: raw capacity (0x%x:%d)\n", __func__,
		compliment, rsoc);

	// if fuelgauge data move to bootloader, then remove this code
	if(rsoc < 2000)
		rsoc = 2000;

	return (min(rsoc, 10000) / 10);
}

static int s2mu005_get_rawsoc(struct s2mu005_fuelgauge_data *fuelgauge)
{
	u8 data[2], check_data[2];
	u16 compliment;
	int rsoc, i;

	for (i = 0; i < 50; i++) {
		if (s2mu005_read_reg(fuelgauge->i2c, S2MU005_REG_RSOC, data) < 0)
			return -EINVAL;
		if (s2mu005_read_reg(fuelgauge->i2c, S2MU005_REG_RSOC, check_data) < 0)
			return -EINVAL;
		if ((data[0] == check_data[0]) && (data[1] == check_data[1]))
			break;
	}

	compliment = (data[1] << 8) | (data[0]);

	/* data[] store 2's compliment format number */
	if (compliment & (0x1 << 15)) {
		/* Negative */
		rsoc = ((~compliment) & 0xFFFF) + 1;
		rsoc = (rsoc * (-10000)) / (0x1 << 14);
	} else {
		rsoc = compliment & 0x7FFF;
		rsoc = ((rsoc * 10000) / (0x1 << 14));
	}

	dev_err(&fuelgauge->i2c->dev, "[DEBUG]%s: raw capacity (0x%x:%d)\n", __func__,
			compliment, rsoc);
	return 50;
//	return min(rsoc, 10000) ;
}

static int s2mu005_get_ocv(struct s2mu005_fuelgauge_data *fuelgauge)
{
	u32 rocv = 0;
	/*
	 * s5mu005 does not have ocv.
	 */

	return rocv;
}

static int s2mu005_get_vbat(struct s2mu005_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	u32 vbat = 0;

	if (s2mu005_read_reg(fuelgauge->i2c, S2MU005_REG_RVBAT, data) < 0)
		return -EINVAL;

	dev_err(&fuelgauge->i2c->dev, "%s: data0 (%d) data1 (%d) \n", __func__, data[0], data[1]);
	vbat = ((data[0] + (data[1] << 8)) * 1000) >> 13;

	dev_err(&fuelgauge->i2c->dev, "%s: vbat (%d)\n", __func__, vbat);

	return vbat;
}

static int s2mu005_get_avgvbat(struct s2mu005_fuelgauge_data *fuelgauge)
{
	u8 data[2];
	u32 new_vbat, old_vbat = 0;
	int cnt;

	for (cnt = 0; cnt < 5; cnt++) {
		if (s2mu005_read_reg(fuelgauge->i2c, S2MU005_REG_RVBAT, data) < 0)
			return -EINVAL;

		new_vbat = ((data[0] + (data[1] << 8)) * 1000) >> 13;

		if (cnt == 0)
			old_vbat = new_vbat;
		else
			old_vbat = new_vbat / 2 + old_vbat / 2;
	}

	dev_dbg(&fuelgauge->i2c->dev, "%s: avgvbat (%d)\n", __func__, old_vbat);

	return old_vbat;
}

/* capacity is  0.1% unit */
static void s2mu005_fg_get_scaled_capacity(
		struct s2mu005_fuelgauge_data *fuelgauge,
		union power_supply_propval *val)
{
	val->intval = (val->intval < fuelgauge->pdata->capacity_min) ?
		0 : ((val->intval - fuelgauge->pdata->capacity_min) * 1000 /
		(fuelgauge->capacity_max - fuelgauge->pdata->capacity_min));

	dev_dbg(&fuelgauge->i2c->dev,
			"%s: scaled capacity (%d.%d)\n",
			__func__, val->intval/10, val->intval%10);
}

/* capacity is integer */
static void s2mu005_fg_get_atomic_capacity(
		struct s2mu005_fuelgauge_data *fuelgauge,
		union power_supply_propval *val)
{
	if (fuelgauge->pdata->capacity_calculation_type &
			SEC_FUELGAUGE_CAPACITY_TYPE_ATOMIC) {
		if (fuelgauge->capacity_old < val->intval)
			val->intval = fuelgauge->capacity_old + 1;
		else if (fuelgauge->capacity_old > val->intval)
			val->intval = fuelgauge->capacity_old - 1;
	}

	/* keep SOC stable in abnormal status */
	if (fuelgauge->pdata->capacity_calculation_type &
			SEC_FUELGAUGE_CAPACITY_TYPE_SKIP_ABNORMAL) {
		if (!fuelgauge->is_charging &&
				fuelgauge->capacity_old < val->intval) {
			dev_err(&fuelgauge->i2c->dev,
					"%s: capacity (old %d : new %d)\n",
					__func__, fuelgauge->capacity_old, val->intval);
			val->intval = fuelgauge->capacity_old;
		}
	}

	/* updated old capacity */
	fuelgauge->capacity_old = val->intval;
}

static int s2mu005_fg_check_capacity_max(
		struct s2mu005_fuelgauge_data *fuelgauge, int capacity_max)
{
	int new_capacity_max = capacity_max;

	if (new_capacity_max < (fuelgauge->pdata->capacity_max -
				fuelgauge->pdata->capacity_max_margin - 10)) {
		new_capacity_max =
			(fuelgauge->pdata->capacity_max -
			 fuelgauge->pdata->capacity_max_margin);

		dev_info(&fuelgauge->i2c->dev, "%s: set capacity max(%d --> %d)\n",
				__func__, capacity_max, new_capacity_max);
	} else if (new_capacity_max > (fuelgauge->pdata->capacity_max +
				fuelgauge->pdata->capacity_max_margin)) {
		new_capacity_max =
			(fuelgauge->pdata->capacity_max +
			 fuelgauge->pdata->capacity_max_margin);

		dev_info(&fuelgauge->i2c->dev, "%s: set capacity max(%d --> %d)\n",
				__func__, capacity_max, new_capacity_max);
	}

	return new_capacity_max;
}

static int s2mu005_fg_calculate_dynamic_scale(
		struct s2mu005_fuelgauge_data *fuelgauge, int capacity)
{
	union power_supply_propval raw_soc_val;
	raw_soc_val.intval = s2mu005_get_rawsoc(fuelgauge) / 10;

	if (raw_soc_val.intval <
			fuelgauge->pdata->capacity_max -
			fuelgauge->pdata->capacity_max_margin) {
		fuelgauge->capacity_max =
			fuelgauge->pdata->capacity_max -
			fuelgauge->pdata->capacity_max_margin;
		dev_dbg(&fuelgauge->i2c->dev, "%s: capacity_max (%d)",
				__func__, fuelgauge->capacity_max);
	} else {
		fuelgauge->capacity_max =
			(raw_soc_val.intval >
			 fuelgauge->pdata->capacity_max +
			 fuelgauge->pdata->capacity_max_margin) ?
			(fuelgauge->pdata->capacity_max +
			 fuelgauge->pdata->capacity_max_margin) :
			raw_soc_val.intval;
		dev_dbg(&fuelgauge->i2c->dev, "%s: raw soc (%d)",
				__func__, fuelgauge->capacity_max);
	}

	if (capacity != 100) {
		fuelgauge->capacity_max = s2mu005_fg_check_capacity_max(
			fuelgauge, (fuelgauge->capacity_max * 100 / capacity));
	} else  {
		fuelgauge->capacity_max =
			(fuelgauge->capacity_max * 99 / 100);
	}

	/* update capacity_old for sec_fg_get_atomic_capacity algorithm */
	fuelgauge->capacity_old = capacity;

	dev_info(&fuelgauge->i2c->dev, "%s: %d is used for capacity_max\n",
			__func__, fuelgauge->capacity_max);

	return fuelgauge->capacity_max;
}

bool s2mu005_fuelgauge_fuelalert_init(struct i2c_client *client, int soc)
{
	struct s2mu005_fuelgauge_data *fuelgauge = i2c_get_clientdata(client);
	u8 data[2];

	/* 1. Set s2mu005 alert configuration. */
	s2mu005_alert_init(fuelgauge);

	if (s2mu005_read_reg(client, S2MU005_REG_IRQ, data) < 0)
		return -1;

	/*Enable VBAT, SOC */
	data[1] &= 0xfc;

	/*Disable IDLE_ST, INIT)ST */
	data[1] |= 0x0c;

	s2mu005_write_reg(client, S2MU005_REG_IRQ, data);

	dev_dbg(&client->dev, "%s: irq_reg(%02x%02x) irq(%d)\n",
			__func__, data[1], data[0], fuelgauge->pdata->fg_irq);

	return true;
}

bool s2mu005_fuelgauge_is_fuelalerted(struct s2mu005_fuelgauge_data *fuelgauge)
{
	return s2mu005_check_status(fuelgauge->i2c);
}

bool s2mu005_hal_fg_fuelalert_process(void *irq_data, bool is_fuel_alerted)
{
	struct s2mu005_fuelgauge_data *fuelgauge = irq_data;
	int ret;

	ret = i2c_smbus_write_byte_data(fuelgauge->i2c, S2MU005_REG_IRQ, 0x00);
	if (ret < 0)
		dev_err(&fuelgauge->i2c->dev, "%s: Error(%d)\n", __func__, ret);

	return ret;
}

bool s2mu005_hal_fg_full_charged(struct i2c_client *client)
{
	return true;
}

static int s2mu005_fg_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct s2mu005_fuelgauge_data *fuelgauge =
		container_of(psy, struct s2mu005_fuelgauge_data, psy_fg);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_CHARGE_FULL:
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		return -ENODATA;
		/* Cell voltage (VCELL, mV) */
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = s2mu005_get_vbat(fuelgauge);
		break;
		/* Additional Voltage Information (mV) */
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		switch (val->intval) {
			case SEC_BATTERY_VOLTAGE_AVERAGE:
				val->intval = s2mu005_get_avgvbat(fuelgauge);
				break;
			case SEC_BATTERY_VOLTAGE_OCV:
				val->intval = s2mu005_get_ocv(fuelgauge);
				break;
		}
		break;
		/* Current (mA) */
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = 0;
		break;
		/* Average Current (mA) */
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (val->intval == SEC_FUELGAUGE_CAPACITY_TYPE_RAW) {
			val->intval = s2mu005_get_rawsoc(fuelgauge);
		} else {
			val->intval = s2mu005_get_soc(fuelgauge);

			if (fuelgauge->pdata->capacity_calculation_type &
				(SEC_FUELGAUGE_CAPACITY_TYPE_SCALE |
					SEC_FUELGAUGE_CAPACITY_TYPE_DYNAMIC_SCALE))
				s2mu005_fg_get_scaled_capacity(fuelgauge, val);

			/* capacity should be between 0% and 100%
			 * (0.1% degree)
			 */
			if (val->intval > 1000)
				val->intval = 1000;
			if (val->intval < 0)
				val->intval = 0;

			/* get only integer part */
			val->intval /= 10;

			/* check whether doing the wake_unlock */
			if ((val->intval > fuelgauge->pdata->fuel_alert_soc) &&
					fuelgauge->is_fuel_alerted) {
				wake_unlock(&fuelgauge->fuel_alert_wake_lock);
				s2mu005_fuelgauge_fuelalert_init(fuelgauge->i2c,
						fuelgauge->pdata->fuel_alert_soc);
			}

			/* (Only for atomic capacity)
			 * In initial time, capacity_old is 0.
			 * and in resume from sleep,
			 * capacity_old is too different from actual soc.
			 * should update capacity_old
			 * by val->intval in booting or resume.
			 */
			if (fuelgauge->initial_update_of_soc) {
				/* updated old capacity */
				fuelgauge->capacity_old = val->intval;
				fuelgauge->initial_update_of_soc = false;
				break;
			}

			if (fuelgauge->pdata->capacity_calculation_type &
				(SEC_FUELGAUGE_CAPACITY_TYPE_ATOMIC |
					 SEC_FUELGAUGE_CAPACITY_TYPE_SKIP_ABNORMAL))
				s2mu005_fg_get_atomic_capacity(fuelgauge, val);
		}

		break;
	/* Battery Temperature */
	case POWER_SUPPLY_PROP_TEMP:
	/* Target Temperature */
	case POWER_SUPPLY_PROP_TEMP_AMBIENT:
		val->intval = s2mu005_get_temperature(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		val->intval = fuelgauge->capacity_max;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int s2mu005_fg_set_property(struct power_supply *psy,
                            enum power_supply_property psp,
                            const union power_supply_propval *val)
{
	struct s2mu005_fuelgauge_data *fuelgauge =
		container_of(psy, struct s2mu005_fuelgauge_data, psy_fg);

	switch (psp) {
		case POWER_SUPPLY_PROP_STATUS:
			break;
		case POWER_SUPPLY_PROP_CHARGE_FULL:
			if (fuelgauge->pdata->capacity_calculation_type &
					SEC_FUELGAUGE_CAPACITY_TYPE_DYNAMIC_SCALE) {
#if defined(CONFIG_PREVENT_SOC_JUMP)
				s2mu005_fg_calculate_dynamic_scale(fuelgauge, val->intval);
#else
				s2mu005_fg_calculate_dynamic_scale(fuelgauge, 100);
#endif
			}
			break;
		case POWER_SUPPLY_PROP_ONLINE:
			fuelgauge->cable_type = val->intval;
			if (val->intval == POWER_SUPPLY_TYPE_BATTERY)
				fuelgauge->is_charging = false;
			else
				fuelgauge->is_charging = true;
		case POWER_SUPPLY_PROP_CAPACITY:
			if (val->intval == SEC_FUELGAUGE_CAPACITY_TYPE_RESET) {
				fuelgauge->initial_update_of_soc = true;
#if 0
				if (!sec_hal_fg_reset(fuelgauge->i2c))
					return -EINVAL;
				else
					break;
#endif
			}
			break;
		case POWER_SUPPLY_PROP_TEMP:
		case POWER_SUPPLY_PROP_TEMP_AMBIENT:
			s2mu005_set_temperature(fuelgauge, val->intval);
			break;
		case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
			dev_info(&fuelgauge->i2c->dev,
				"%s: capacity_max changed, %d -> %d\n",
				__func__, fuelgauge->capacity_max, val->intval);
			fuelgauge->capacity_max = s2mu005_fg_check_capacity_max(fuelgauge, val->intval);
			fuelgauge->initial_update_of_soc = true;
			break;
		case POWER_SUPPLY_PROP_CHARGE_TYPE:
			/* rt5033_fg_reset_capacity_by_jig_connection(fuelgauge->i2c); */
			break;
		default:
			return -EINVAL;
	}

	return 0;
}

#ifdef CONFIG_OF
static int s2mu005_fuelgauge_parse_dt(struct s2mu005_fuelgauge_data *fuelgauge)
{
	struct device_node *np = of_find_node_by_name(NULL, "s2mu005-fuelgauge");
	int ret;
	int i, len;
	const u32 *p;

	        /* reset, irq gpio info */
	if (np == NULL) {
		pr_err("%s np NULL\n", __func__);
	} else {
		ret = of_property_read_u32(np, "fuelgauge,capacity_max",
				&fuelgauge->pdata->capacity_max);
		if (ret < 0)
			pr_err("%s error reading capacity_max %d\n", __func__, ret);

		ret = of_property_read_u32(np, "fuelgauge,capacity_max_margin",
				&fuelgauge->pdata->capacity_max_margin);
		if (ret < 0)
			pr_err("%s error reading capacity_max_margin %d\n", __func__, ret);

		ret = of_property_read_u32(np, "fuelgauge,capacity_min",
				&fuelgauge->pdata->capacity_min);
		if (ret < 0)
			pr_err("%s error reading capacity_min %d\n", __func__, ret);

		ret = of_property_read_u32(np, "fuelgauge,capacity_calculation_type",
				&fuelgauge->pdata->capacity_calculation_type);
		if (ret < 0)
			pr_err("%s error reading capacity_calculation_type %d\n",
					__func__, ret);

		ret = of_property_read_u32(np, "fuelgauge,fuel_alert_soc",
				&fuelgauge->pdata->fuel_alert_soc);
		if (ret < 0)
			pr_err("%s error reading pdata->fuel_alert_soc %d\n",
					__func__, ret);
		fuelgauge->pdata->repeated_fuelalert = of_property_read_bool(np,
				"fuelgauge,repeated_fuelalert");

		np = of_find_node_by_name(NULL, "battery");
		if (!np) {
			pr_err("%s np NULL\n", __func__);
		} else {
			ret = of_property_read_string(np,
				"battery,fuelgauge_name",
				(char const **)&fuelgauge->pdata->fuelgauge_name);
			p = of_get_property(np,
					"battery,input_current_limit", &len);
			if (!p)
				return 1;

			len = len / sizeof(u32);
			fuelgauge->pdata->charging_current =
					kzalloc(sizeof(struct sec_charging_current) * len,
					GFP_KERNEL);

			for(i = 0; i < len; i++) {
				ret = of_property_read_u32_index(np,
					"battery,input_current_limit", i,
					&fuelgauge->pdata->charging_current[i].input_current_limit);
				ret = of_property_read_u32_index(np,
					"battery,fast_charging_current", i,
					&fuelgauge->pdata->charging_current[i].fast_charging_current);
				ret = of_property_read_u32_index(np,
					"battery,full_check_current_1st", i,
					&fuelgauge->pdata->charging_current[i].full_check_current_1st);
				ret = of_property_read_u32_index(np,
					"battery,full_check_current_2nd", i,
					&fuelgauge->pdata->charging_current[i].full_check_current_2nd);
			}
		}
	}

	return 0;
}

static struct of_device_id s2mu005_fuelgauge_match_table[] = {
        { .compatible = "samsung,s2mu005-fuelgauge",},
        {},
};
#else
static int s2mu005_fuelgauge_parse_dt(struct s2mu005_fuelgauge_data *fuelgauge)
{
    return -ENOSYS;
}

#define s2mu005_fuelgauge_match_table NULL
#endif /* CONFIG_OF */

static int s2mu005_fuelgauge_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct s2mu005_fuelgauge_data *fuelgauge;
	union power_supply_propval raw_soc_val;
	int ret = 0;

	pr_info("%s: S2MU005 Fuelgauge Driver Loading\n", __func__);

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE))
		return -EIO;

	fuelgauge = kzalloc(sizeof(*fuelgauge), GFP_KERNEL);
	if (!fuelgauge)
		return -ENOMEM;

	mutex_init(&fuelgauge->fg_lock);

	fuelgauge->i2c = client;

	if (client->dev.of_node) {
		fuelgauge->pdata = devm_kzalloc(&client->dev, sizeof(*(fuelgauge->pdata)),
				GFP_KERNEL);
		if (!fuelgauge->pdata) {
			dev_err(&client->dev, "Failed to allocate memory\n");
			ret = -ENOMEM;
			goto err_parse_dt_nomem;
		}
		ret = s2mu005_fuelgauge_parse_dt(fuelgauge);
		if (ret < 0)
			goto err_parse_dt;
	} else {
		fuelgauge->pdata = client->dev.platform_data;
	}

	i2c_set_clientdata(client, fuelgauge);

	if (fuelgauge->pdata->fuelgauge_name == NULL)
		fuelgauge->pdata->fuelgauge_name = "sec-fuelgauge";

	fuelgauge->psy_fg.name          = fuelgauge->pdata->fuelgauge_name;
	fuelgauge->psy_fg.type          = POWER_SUPPLY_TYPE_UNKNOWN;
	fuelgauge->psy_fg.get_property  = s2mu005_fg_get_property;
	fuelgauge->psy_fg.set_property  = s2mu005_fg_set_property;
	fuelgauge->psy_fg.properties    = s2mu005_fuelgauge_props;
	fuelgauge->psy_fg.num_properties =
			ARRAY_SIZE(s2mu005_fuelgauge_props);

	fuelgauge->capacity_max = fuelgauge->pdata->capacity_max;
	raw_soc_val.intval = s2mu005_get_rawsoc(fuelgauge) / 10;

	if (raw_soc_val.intval > fuelgauge->capacity_max)
		s2mu005_fg_calculate_dynamic_scale(fuelgauge, 100);

#if REGISTER_DEBUG
	INIT_DELAYED_WORK(&fuelgauge->register_work, u005register_work);
	fuelgauge->register_wq = create_singlethread_workqueue("register_wq");
#endif

	ret = s2mu005_init_regs(fuelgauge);
	if (ret < 0) {
		pr_err("%s: Failed to Initialize Fuelgauge\n", __func__);
		/* goto err_data_free; */
	}

	ret = power_supply_register(&client->dev, &fuelgauge->psy_fg);
	if (ret) {
		pr_err("%s: Failed to Register psy_fg\n", __func__);
		goto err_data_free;
	}

	fuelgauge->initial_update_of_soc = true;

	pr_info("%s: S2MU005 Fuelgauge Driver Loaded\n", __func__);
	return 0;

err_data_free:
	if (client->dev.of_node)
		kfree(fuelgauge->pdata);

err_parse_dt:
err_parse_dt_nomem:
	mutex_destroy(&fuelgauge->fg_lock);
	kfree(fuelgauge);

	return ret;
}

static const struct i2c_device_id s2mu005_fuelgauge_id[] = {
	{"s2mu005-fuelgauge", 0},
	{}
};

static void s2mu005_fuelgauge_shutdown(struct i2c_client *client)
{
}

static int s2mu005_fuelgauge_remove(struct i2c_client *client)
{
	struct s2mu005_fuelgauge_data *fuelgauge = i2c_get_clientdata(client);

	if (fuelgauge->pdata->fuel_alert_soc >= 0)
		wake_lock_destroy(&fuelgauge->fuel_alert_wake_lock);

	return 0;
}

#if defined CONFIG_PM
static int s2mu005_fuelgauge_suspend(struct device *dev)
{
        return 0;
}

static int s2mu005_fuelgauge_resume(struct device *dev)
{
        return 0;
}
#else
#define s2mu005_fuelgauge_suspend NULL
#define s2mu005_fuelgauge_resume NULL
#endif

static SIMPLE_DEV_PM_OPS(s2mu005_fuelgauge_pm_ops, s2mu005_fuelgauge_suspend,
		s2mu005_fuelgauge_resume);

static struct i2c_driver s2mu005_fuelgauge_driver = {
	.driver = {
		.name = "s2mu005-fuelgauge",
		.owner = THIS_MODULE,
		.pm = &s2mu005_fuelgauge_pm_ops,
		.of_match_table = s2mu005_fuelgauge_match_table,
	},
	.probe  = s2mu005_fuelgauge_probe,
	.remove = s2mu005_fuelgauge_remove,
	.shutdown   = s2mu005_fuelgauge_shutdown,
	.id_table   = s2mu005_fuelgauge_id,
};

static int __init s2mu005_fuelgauge_init(void)
{
	pr_info("%s: S2MU005 Fuelgauge Init\n", __func__);
	return i2c_add_driver(&s2mu005_fuelgauge_driver);
}

static void __exit s2mu005_fuelgauge_exit(void)
{
	i2c_del_driver(&s2mu005_fuelgauge_driver);
}
module_init(s2mu005_fuelgauge_init);
module_exit(s2mu005_fuelgauge_exit);

MODULE_DESCRIPTION("Samsung S2MU005 Fuel Gauge Driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
