/***************************************************************************
 *   Copyright (C) 2007-2008 by unsik Kim <donari75@gmail.com>             *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mflash.h"
#include "time_support.h"
#include "fileio.h"
#include "log.h"


static int s3c2440_set_gpio_to_output (mflash_gpio_num_t gpio);
static int s3c2440_set_gpio_output_val (mflash_gpio_num_t gpio, uint8_t val);
static int pxa270_set_gpio_to_output (mflash_gpio_num_t gpio);
static int pxa270_set_gpio_output_val (mflash_gpio_num_t gpio, uint8_t val);

static command_t *mflash_cmd;

static mflash_bank_t *mflash_bank;

static mflash_gpio_drv_t pxa270_gpio = {
	.name = "pxa270",
	.set_gpio_to_output = pxa270_set_gpio_to_output,
	.set_gpio_output_val = pxa270_set_gpio_output_val
};

static mflash_gpio_drv_t s3c2440_gpio = {
	.name = "s3c2440",
	.set_gpio_to_output = s3c2440_set_gpio_to_output,
	.set_gpio_output_val = s3c2440_set_gpio_output_val
};

static mflash_gpio_drv_t *mflash_gpio[] =
{
		&pxa270_gpio,
		&s3c2440_gpio,
		NULL
};

#define PXA270_GAFR0_L 0x40E00054
#define PXA270_GAFR3_U 0x40E00070
#define PXA270_GAFR3_U_RESERVED_BITS  0xfffc0000u
#define PXA270_GPDR0 0x40E0000C
#define PXA270_GPDR3 0x40E0010C
#define PXA270_GPDR3_RESERVED_BITS  0xfe000000u
#define PXA270_GPSR0 0x40E00018
#define PXA270_GPCR0 0x40E00024

static int pxa270_set_gpio_to_output (mflash_gpio_num_t gpio)
{
	u32 addr, value, mask;
	target_t *target = mflash_bank->target;
	int ret;

	/* remove alternate function. */
	mask = 0x3u << (gpio.num & 0xF)*2;

	addr = PXA270_GAFR0_L + (gpio.num >> 4) * 4;

	if ((ret = target_read_u32(target, addr, &value)) != ERROR_OK)
		return ret;

	value &= ~mask;
	if (addr == PXA270_GAFR3_U)
		value &= ~PXA270_GAFR3_U_RESERVED_BITS;

	if ((ret = target_write_u32(target, addr, value)) != ERROR_OK)
		return ret;

	/* set direction to output */
	mask = 0x1u << (gpio.num & 0x1F);

	addr = PXA270_GPDR0 + (gpio.num >> 5) * 4;

	if ((ret = target_read_u32(target, addr, &value)) != ERROR_OK)
		return ret;

	value |= mask;
	if (addr == PXA270_GPDR3)
		value &= ~PXA270_GPDR3_RESERVED_BITS;

	ret = target_write_u32(target, addr, value);
	return ret;
}

static int pxa270_set_gpio_output_val (mflash_gpio_num_t gpio, uint8_t val)
{
	u32 addr, value, mask;
	target_t *target = mflash_bank->target;
	int ret;

	mask = 0x1u << (gpio.num & 0x1F);

	if (val) {
		addr = PXA270_GPSR0 + (gpio.num >> 5) * 4;
	} else {
		addr = PXA270_GPCR0 + (gpio.num >> 5) * 4;
	}

	if ((ret = target_read_u32(target, addr, &value)) != ERROR_OK)
		return ret;

	value |= mask;

	ret = target_write_u32(target, addr, value);

	return ret;
}

#define S3C2440_GPACON 0x56000000
#define S3C2440_GPADAT 0x56000004
#define S3C2440_GPJCON 0x560000d0
#define S3C2440_GPJDAT 0x560000d4

static int s3c2440_set_gpio_to_output (mflash_gpio_num_t gpio)
{
	u32 data, mask, gpio_con;
	target_t *target = mflash_bank->target;
	int ret;

	if (gpio.port[0] >= 'a' && gpio.port[0] <= 'h') {
		gpio_con = S3C2440_GPACON + (gpio.port[0] - 'a') * 0x10;
	} else if (gpio.port[0] == 'j') {
		gpio_con = S3C2440_GPJCON;
	} else {
		LOG_ERROR("mflash: invalid port %d%s", gpio.num, gpio.port);
		return ERROR_INVALID_ARGUMENTS;
	}

	ret = target_read_u32(target, gpio_con, &data);

	if (ret == ERROR_OK) {
		if (gpio.port[0] == 'a') {
			mask = 1 << gpio.num;
			data &= ~mask;
		} else {
			mask = 3 << gpio.num * 2;
			data &= ~mask;
			data |= (1 << gpio.num * 2);
		}

		ret = target_write_u32(target, gpio_con, data);
	}
	return ret;
}

static int s3c2440_set_gpio_output_val (mflash_gpio_num_t gpio, uint8_t val)
{
	u32 data, mask, gpio_dat;
	target_t *target = mflash_bank->target;
	int ret;

	if (gpio.port[0] >= 'a' && gpio.port[0] <= 'h') {
		gpio_dat = S3C2440_GPADAT + (gpio.port[0] - 'a') * 0x10;
	} else if (gpio.port[0] == 'j') {
		gpio_dat = S3C2440_GPJDAT;
	} else {
		LOG_ERROR("mflash: invalid port %d%s", gpio.num, gpio.port);
		return ERROR_INVALID_ARGUMENTS;
	}

	ret = target_read_u32(target, gpio_dat, &data);

	if (ret == ERROR_OK) {
		mask = 1 << gpio.num;
		if (val)
			data |= mask;
		else
			data &= ~mask;

		ret = target_write_u32(target, gpio_dat, data);
	}
	return ret;
}

static int mg_hdrst(uint8_t level)
{
	return mflash_bank->gpio_drv->set_gpio_output_val(mflash_bank->rst_pin, level);
}

static int mg_init_gpio (void)
{
	int ret;
	mflash_gpio_drv_t *gpio_drv = mflash_bank->gpio_drv;

	ret = gpio_drv->set_gpio_to_output(mflash_bank->rst_pin);
	if (ret != ERROR_OK)
		return ret;

	ret = gpio_drv->set_gpio_output_val(mflash_bank->rst_pin, 1);

	return ret;
}

static int mg_dsk_wait(mg_io_type_wait wait, u32 time)
{
	uint8_t status, error;
	target_t *target = mflash_bank->target;
	u32 mg_task_reg = mflash_bank->base + MG_REG_OFFSET;
	duration_t duration;
	int ret;
	long long t=0;

	duration_start_measure(&duration);

	while (time) {

		ret = target_read_u8(target, mg_task_reg + MG_REG_STATUS, &status);
		if (ret != ERROR_OK)
			return ret;

		if (status & mg_io_rbit_status_busy)
		{
			if (wait == mg_io_wait_bsy)
				return ERROR_OK;
		} else {
			switch(wait)
			{
				case mg_io_wait_not_bsy:
					return ERROR_OK;
				case mg_io_wait_rdy_noerr:
					if (status & mg_io_rbit_status_ready)
						return ERROR_OK;
					break;
				case mg_io_wait_drq_noerr:
					if (status & mg_io_rbit_status_data_req)
						return ERROR_OK;
					break;
				default:
					break;
			}

			/* Now we check the error condition! */
			if (status & mg_io_rbit_status_error)
			{
				ret = target_read_u8(target, mg_task_reg + MG_REG_ERROR, &error);
				if (ret != ERROR_OK)
					return ret;

				LOG_ERROR("mflash: io error 0x%02x", error);

				return ERROR_MG_IO;
			}

			switch (wait)
			{
				case mg_io_wait_rdy:
					if (status & mg_io_rbit_status_ready)
						return ERROR_OK;

				case mg_io_wait_drq:
					if (status & mg_io_rbit_status_data_req)
						return ERROR_OK;

				default:
					break;
			}
		}

		duration_stop_measure(&duration, NULL);

		t=duration.duration.tv_usec/1000;
		t+=duration.duration.tv_sec*1000;

		if (t > time)
			break;
	}

	LOG_ERROR("mflash: timeout occured");
	return ERROR_MG_TIMEOUT;
}

static int mg_dsk_srst(uint8_t on)
{
	target_t *target = mflash_bank->target;
	u32 mg_task_reg = mflash_bank->base + MG_REG_OFFSET;
	uint8_t value;
	int ret;

	if ((ret = target_read_u8(target, mg_task_reg + MG_REG_DRV_CTRL, &value)) != ERROR_OK)
		return ret;

	if(on) {
		value |= (mg_io_rbit_devc_srst);
	} else {
		value &= ~mg_io_rbit_devc_srst;
	}

	ret = target_write_u8(target, mg_task_reg + MG_REG_DRV_CTRL, value);
	return ret;
}

static int mg_dsk_io_cmd(u32 sect_num, u32 cnt, uint8_t cmd)
{
	target_t *target = mflash_bank->target;
	u32 mg_task_reg = mflash_bank->base + MG_REG_OFFSET;
	uint8_t value;
	int ret;

	ret = mg_dsk_wait(mg_io_wait_rdy_noerr, MG_OEM_DISK_WAIT_TIME_NORMAL);
	if (ret != ERROR_OK)
		return ret;

	value = mg_io_rval_dev_drv_master | mg_io_rval_dev_lba_mode |((sect_num >> 24) & 0xf);

	ret = target_write_u8(target, mg_task_reg + MG_REG_DRV_HEAD, value);
	ret |= target_write_u8(target, mg_task_reg + MG_REG_SECT_CNT, (uint8_t)cnt);
	ret |= target_write_u8(target, mg_task_reg + MG_REG_SECT_NUM, (uint8_t)sect_num);
	ret |= target_write_u8(target, mg_task_reg + MG_REG_CYL_LOW, (uint8_t)(sect_num >> 8));
	ret |= target_write_u8(target, mg_task_reg + MG_REG_CYL_HIGH, (uint8_t)(sect_num >> 16));

	if (ret != ERROR_OK)
		return ret;

	return target_write_u8(target, mg_task_reg + MG_REG_COMMAND, cmd);
}

static int mg_dsk_drv_info(void)
{
	target_t *target = mflash_bank->target;
	u32 mg_buff = mflash_bank->base + MG_BUFFER_OFFSET;
	int ret;

	if ((ret =  mg_dsk_io_cmd(0, 1, mg_io_cmd_identify)) != ERROR_OK)
		return ret;

	if ((ret = mg_dsk_wait(mg_io_wait_drq, MG_OEM_DISK_WAIT_TIME_NORMAL)) != ERROR_OK)
		return ret;

	LOG_INFO("mflash: read drive info");

	if (! mflash_bank->drv_info)
		mflash_bank->drv_info = malloc(sizeof(mg_drv_info_t));

	target_read_memory(target, mg_buff, 2, sizeof(mg_io_type_drv_info) >> 1,
			(uint8_t *)&mflash_bank->drv_info->drv_id);
	if (ret != ERROR_OK)
		return ret;

	mflash_bank->drv_info->tot_sects = (u32)(mflash_bank->drv_info->drv_id.total_user_addressable_sectors_hi << 16)
									+ mflash_bank->drv_info->drv_id.total_user_addressable_sectors_lo;

	return target_write_u8(target, mflash_bank->base + MG_REG_OFFSET + MG_REG_COMMAND, mg_io_cmd_confirm_read);
}

static int mg_mflash_rst(void)
{
	int ret;

	if ((ret = mg_init_gpio()) != ERROR_OK)
		return ret;

	if ((ret = mg_hdrst(0)) != ERROR_OK)
		return ret;

	if ((ret = mg_dsk_wait(mg_io_wait_bsy, MG_OEM_DISK_WAIT_TIME_LONG)) != ERROR_OK)
		return ret;

	if ((ret = mg_hdrst(1)) != ERROR_OK)
		return ret;

	if ((ret = mg_dsk_wait(mg_io_wait_not_bsy, MG_OEM_DISK_WAIT_TIME_LONG)) != ERROR_OK)
		return ret;

	if ((ret = mg_dsk_srst(1)) != ERROR_OK)
		return ret;

	if ((ret = mg_dsk_wait(mg_io_wait_bsy, MG_OEM_DISK_WAIT_TIME_LONG)) != ERROR_OK)
		return ret;

	if ((ret = mg_dsk_srst(0)) != ERROR_OK)
		return ret;

	if ((ret = mg_dsk_wait(mg_io_wait_not_bsy, MG_OEM_DISK_WAIT_TIME_LONG)) != ERROR_OK)
		return ret;

	LOG_INFO("mflash: reset ok");

	return ERROR_OK;
}

static int mg_mflash_probe(void)
{
	int ret;

	if ((ret = mg_mflash_rst()) != ERROR_OK)
		return ret;

	return mg_dsk_drv_info();
}

static int mg_probe_cmd(struct command_context_s *cmd_ctx, char *cmd, char **args, int argc)
{
	int ret;

	ret = mg_mflash_probe();

	if (ret == ERROR_OK) {
		command_print(cmd_ctx, "mflash (total %u sectors) found at 0x%8.8x",
				mflash_bank->drv_info->tot_sects, mflash_bank->base );
	}

	return ret;
}

static int mg_mflash_do_read_sects(void *buff, u32 sect_num, u32 sect_cnt)
{
	u32 i, address;
	int ret;
	target_t *target = mflash_bank->target;
	uint8_t *buff_ptr = buff;
	duration_t duration;

	if ((ret = mg_dsk_io_cmd(sect_num, sect_cnt, mg_io_cmd_read)) != ERROR_OK )
		return ret;

	address = mflash_bank->base + MG_BUFFER_OFFSET;

	duration_start_measure(&duration);

	for (i = 0; i < sect_cnt; i++) {
		ret = mg_dsk_wait(mg_io_wait_drq, MG_OEM_DISK_WAIT_TIME_NORMAL);
		if (ret != ERROR_OK)
			return ret;

		ret = target_read_memory(target, address, 2, MG_MFLASH_SECTOR_SIZE / 2, buff_ptr);
		if (ret != ERROR_OK)
			return ret;

		buff_ptr += MG_MFLASH_SECTOR_SIZE;

		ret = target_write_u8(target, mflash_bank->base + MG_REG_OFFSET + MG_REG_COMMAND, mg_io_cmd_confirm_read);
		if (ret != ERROR_OK)
			return ret;

		LOG_DEBUG("mflash: %u (0x%8.8x) sector read", sect_num + i, (sect_num + i) * MG_MFLASH_SECTOR_SIZE);

		duration_stop_measure(&duration, NULL);

		if ((duration.duration.tv_sec * 1000 + duration.duration.tv_usec / 1000) > 3000) {
			LOG_INFO("mflash: read %u'th sectors", sect_num + i);
			duration_start_measure(&duration);
		}
	}

	return mg_dsk_wait(mg_io_wait_rdy, MG_OEM_DISK_WAIT_TIME_NORMAL);
}

static int mg_mflash_read_sects(void *buff, u32 sect_num, u32 sect_cnt)
{
	u32 quotient, residue, i;
	uint8_t *buff_ptr = buff;
	int ret = ERROR_OK;

	quotient = sect_cnt >> 8;
	residue = sect_cnt % 256;

	for (i = 0; i < quotient; i++) {
		LOG_DEBUG("mflash: sect num : %u buff : 0x%0lx", sect_num, 
			(unsigned long)buff_ptr);
		ret = mg_mflash_do_read_sects(buff_ptr, sect_num, 256);
		if (ret != ERROR_OK)
			return ret;

		sect_num += 256;
		buff_ptr += 256 * MG_MFLASH_SECTOR_SIZE;
	}

	if (residue) {
		LOG_DEBUG("mflash: sect num : %u buff : %0lx", sect_num, 
			(unsigned long)buff_ptr);
		return mg_mflash_do_read_sects(buff_ptr, sect_num, residue);
	}

	return ret;
}

static int mg_mflash_do_write_sects(void *buff, u32 sect_num, u32 sect_cnt,
		mg_io_type_cmd cmd)
{
	u32 i, address;
	int ret;
	target_t *target = mflash_bank->target;
	uint8_t *buff_ptr = buff;
	duration_t duration;

	if ((ret = mg_dsk_io_cmd(sect_num, sect_cnt, cmd)) != ERROR_OK )
		return ret;

	address = mflash_bank->base + MG_BUFFER_OFFSET;

	duration_start_measure(&duration);

	for (i = 0; i < sect_cnt; i++) {
		ret = mg_dsk_wait(mg_io_wait_drq, MG_OEM_DISK_WAIT_TIME_NORMAL);
		if (ret != ERROR_OK)
			return ret;

		ret = target_write_memory(target, address, 2, MG_MFLASH_SECTOR_SIZE / 2, buff_ptr);
		if (ret != ERROR_OK)
			return ret;
		
		buff_ptr += MG_MFLASH_SECTOR_SIZE;

		ret = target_write_u8(target, mflash_bank->base + MG_REG_OFFSET + MG_REG_COMMAND, mg_io_cmd_confirm_write);
		if (ret != ERROR_OK)
			return ret;

		LOG_DEBUG("mflash: %u (0x%8.8x) sector write", sect_num + i, (sect_num + i) * MG_MFLASH_SECTOR_SIZE);

		duration_stop_measure(&duration, NULL);

		if ((duration.duration.tv_sec * 1000 + duration.duration.tv_usec / 1000) > 3000) {
			LOG_INFO("mflash: wrote %u'th sectors", sect_num + i);
			duration_start_measure(&duration);
		}
	}

	if (cmd == mg_io_cmd_write)
		ret = mg_dsk_wait(mg_io_wait_rdy, MG_OEM_DISK_WAIT_TIME_NORMAL);
	else
		ret = mg_dsk_wait(mg_io_wait_rdy, MG_OEM_DISK_WAIT_TIME_LONG);

	return ret;
}

static int mg_mflash_write_sects(void *buff, u32 sect_num, u32 sect_cnt)
{
	u32 quotient, residue, i;
	uint8_t *buff_ptr = buff;
	int ret = ERROR_OK;

	quotient = sect_cnt >> 8;
	residue = sect_cnt % 256;

	for (i = 0; i < quotient; i++) {
		LOG_DEBUG("mflash: sect num : %u buff : %0lx", sect_num, 
			(unsigned long)buff_ptr);
		ret = mg_mflash_do_write_sects(buff_ptr, sect_num, 256, mg_io_cmd_write);
		if (ret != ERROR_OK)
			return ret;

		sect_num += 256;
		buff_ptr += 256 * MG_MFLASH_SECTOR_SIZE;
	}

	if (residue) {
		LOG_DEBUG("mflash: sect num : %u buff : %0lx", sect_num, 
			(unsigned long)buff_ptr);
		return mg_mflash_do_write_sects(buff_ptr, sect_num, residue, mg_io_cmd_write);
	}

	return ret;
}

static int mg_mflash_read (u32 addr, uint8_t *buff, u32 len)
{
	uint8_t *buff_ptr = buff;
	uint8_t sect_buff[MG_MFLASH_SECTOR_SIZE];
	u32 cur_addr, next_sec_addr, end_addr, cnt, sect_num;
	int ret = ERROR_OK;

	cnt = 0;
	cur_addr = addr;
	end_addr = addr + len;

	if (cur_addr & MG_MFLASH_SECTOR_SIZE_MASK) {

		next_sec_addr = (cur_addr + MG_MFLASH_SECTOR_SIZE) & ~MG_MFLASH_SECTOR_SIZE_MASK;
		sect_num = cur_addr >> MG_MFLASH_SECTOR_SIZE_SHIFT;
		ret = mg_mflash_read_sects(sect_buff, sect_num, 1);
		if (ret != ERROR_OK)
			return ret;

		if (end_addr < next_sec_addr) {
			memcpy(buff_ptr, sect_buff + (cur_addr & MG_MFLASH_SECTOR_SIZE_MASK), end_addr - cur_addr);
			LOG_DEBUG("mflash: copies %u byte from sector offset 0x%8.8x", end_addr - cur_addr, cur_addr);
			cur_addr = end_addr;
		} else {
			memcpy(buff_ptr, sect_buff + (cur_addr & MG_MFLASH_SECTOR_SIZE_MASK), next_sec_addr - cur_addr);
			LOG_DEBUG("mflash: copies %u byte from sector offset 0x%8.8x", next_sec_addr - cur_addr, cur_addr);
			buff_ptr += (next_sec_addr - cur_addr);
			cur_addr = next_sec_addr;
		}
	}

	if (cur_addr < end_addr) {

		sect_num = cur_addr >> MG_MFLASH_SECTOR_SIZE_SHIFT;
		next_sec_addr = cur_addr + MG_MFLASH_SECTOR_SIZE;

		while (next_sec_addr <= end_addr) {
			cnt++;
			next_sec_addr += MG_MFLASH_SECTOR_SIZE;
		}

		if (cnt)
			if ((ret = mg_mflash_read_sects(buff_ptr, sect_num, cnt)) != ERROR_OK)
				return ret;

		buff_ptr += cnt * MG_MFLASH_SECTOR_SIZE;
		cur_addr += cnt * MG_MFLASH_SECTOR_SIZE;

		if (cur_addr < end_addr) {

			sect_num = cur_addr >> MG_MFLASH_SECTOR_SIZE_SHIFT;
			ret = mg_mflash_read_sects(sect_buff, sect_num, 1);
			if (ret != ERROR_OK)
				return ret;

			memcpy(buff_ptr, sect_buff, end_addr - cur_addr);
			LOG_DEBUG("mflash: copies %u byte", end_addr - cur_addr);

		}
	}

	return ret;
}

static int mg_mflash_write(u32 addr, uint8_t *buff, u32 len)
{
	uint8_t *buff_ptr = buff;
	uint8_t sect_buff[MG_MFLASH_SECTOR_SIZE];
	u32 cur_addr, next_sec_addr, end_addr, cnt, sect_num;
	int ret = ERROR_OK;

	cnt = 0;
	cur_addr = addr;
	end_addr = addr + len;

	if (cur_addr & MG_MFLASH_SECTOR_SIZE_MASK) {

		next_sec_addr = (cur_addr + MG_MFLASH_SECTOR_SIZE) & ~MG_MFLASH_SECTOR_SIZE_MASK;
		sect_num = cur_addr >> MG_MFLASH_SECTOR_SIZE_SHIFT;
		ret = mg_mflash_read_sects(sect_buff, sect_num, 1);
		if (ret != ERROR_OK)
			return ret;

		if (end_addr < next_sec_addr) {
			memcpy(sect_buff + (cur_addr & MG_MFLASH_SECTOR_SIZE_MASK), buff_ptr, end_addr - cur_addr);
			LOG_DEBUG("mflash: copies %u byte to sector offset 0x%8.8x", end_addr - cur_addr, cur_addr);
			cur_addr = end_addr;
		} else {
			memcpy(sect_buff + (cur_addr & MG_MFLASH_SECTOR_SIZE_MASK), buff_ptr, next_sec_addr - cur_addr);
			LOG_DEBUG("mflash: copies %u byte to sector offset 0x%8.8x", next_sec_addr - cur_addr, cur_addr);
			buff_ptr += (next_sec_addr - cur_addr);
			cur_addr = next_sec_addr;
		}

		ret = mg_mflash_write_sects(sect_buff, sect_num, 1);
		if (ret != ERROR_OK)
			return ret;
	}

	if (cur_addr < end_addr) {

		sect_num = cur_addr >> MG_MFLASH_SECTOR_SIZE_SHIFT;
		next_sec_addr = cur_addr + MG_MFLASH_SECTOR_SIZE;

		while (next_sec_addr <= end_addr) {
			cnt++;
			next_sec_addr += MG_MFLASH_SECTOR_SIZE;
		}

		if (cnt)
			if ((ret = mg_mflash_write_sects(buff_ptr, sect_num, cnt)) != ERROR_OK)
				return ret;

		buff_ptr += cnt * MG_MFLASH_SECTOR_SIZE;
		cur_addr += cnt * MG_MFLASH_SECTOR_SIZE;

		if (cur_addr < end_addr) {

			sect_num = cur_addr >> MG_MFLASH_SECTOR_SIZE_SHIFT;
			ret = mg_mflash_read_sects(sect_buff, sect_num, 1);
			if (ret != ERROR_OK)
				return ret;

			memcpy(sect_buff, buff_ptr, end_addr - cur_addr);
			LOG_DEBUG("mflash: copies %u byte", end_addr - cur_addr);
			ret = mg_mflash_write_sects(sect_buff, sect_num, 1);
		}
	}

	return ret;
}

static int mg_write_cmd(struct command_context_s *cmd_ctx, char *cmd, char **args, int argc)
{
	u32 address, buf_cnt, cnt, res, i;
	uint8_t *buffer;
	fileio_t fileio;
	duration_t duration;
	char *duration_text;
	int ret;

	if (argc != 3) {
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	address = strtoul(args[2], NULL, 0);

	ret = fileio_open(&fileio, args[1], FILEIO_READ, FILEIO_BINARY);
	if (ret != ERROR_OK)
		return ret;

	buffer = malloc(MG_FILEIO_CHUNK);
	if (!buffer) {
		fileio_close(&fileio);
		return ERROR_FAIL;
	}

	cnt = fileio.size / MG_FILEIO_CHUNK;
	res = fileio.size % MG_FILEIO_CHUNK;

	duration_start_measure(&duration);

	for (i = 0; i < cnt; i++) {
		if ((ret = fileio_read(&fileio, MG_FILEIO_CHUNK, buffer, &buf_cnt)) !=
				ERROR_OK)
			goto mg_write_cmd_err;
		if ((ret = mg_mflash_write(address, buffer, MG_FILEIO_CHUNK)) != ERROR_OK)
			goto mg_write_cmd_err;
		address += MG_FILEIO_CHUNK;
	}
 
	if (res) {
		if ((ret = fileio_read(&fileio, res, buffer, &buf_cnt)) != ERROR_OK)
			goto mg_write_cmd_err;			
		if ((ret = mg_mflash_write(address, buffer, res)) != ERROR_OK)
			goto mg_write_cmd_err;
	}

	duration_stop_measure(&duration, &duration_text);

	command_print(cmd_ctx, "wrote %lli byte from file %s in %s (%f kB/s)",
		fileio.size, args[1], duration_text,
		(float)fileio.size / 1024.0 / ((float)duration.duration.tv_sec + ((float)duration.duration.tv_usec / 1000000.0)));

	free(duration_text);
	free(buffer);
	fileio_close(&fileio);

	return ERROR_OK;

mg_write_cmd_err:
	duration_stop_measure(&duration, &duration_text);
	free(duration_text);
 	free(buffer);
	fileio_close(&fileio);

	return ret;
}

static int mg_dump_cmd(struct command_context_s *cmd_ctx, char *cmd, char **args, int argc)
{
	u32 address, size_written, size, cnt, res, i;
	uint8_t *buffer;
	fileio_t fileio;
	duration_t duration;
	char *duration_text;
	int ret;

	if (argc != 4) {
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	address = strtoul(args[2], NULL, 0);
	size = strtoul(args[3], NULL, 0);

	ret = fileio_open(&fileio, args[1], FILEIO_WRITE, FILEIO_BINARY);
	if (ret != ERROR_OK)
		return ret;
 
	buffer = malloc(MG_FILEIO_CHUNK);
	if (!buffer) {
		fileio_close(&fileio);
		return ERROR_FAIL;
	}

	cnt = size / MG_FILEIO_CHUNK;
	res = size % MG_FILEIO_CHUNK;
 
	duration_start_measure(&duration);

	for (i = 0; i < cnt; i++) {
		if ((ret = mg_mflash_read(address, buffer, MG_FILEIO_CHUNK)) != ERROR_OK)
			goto mg_dump_cmd_err;
		if ((ret = fileio_write(&fileio, MG_FILEIO_CHUNK, buffer, &size_written))
				!= ERROR_OK)
			goto mg_dump_cmd_err;
		address += MG_FILEIO_CHUNK;
	}
 
	if (res) {
		if ((ret = mg_mflash_read(address, buffer, res)) != ERROR_OK)
			goto mg_dump_cmd_err;
		if ((ret = fileio_write(&fileio, res, buffer, &size_written)) != ERROR_OK)
			goto mg_dump_cmd_err;
	}

	duration_stop_measure(&duration, &duration_text);

	command_print(cmd_ctx, "dump image (address 0x%8.8x size %u) to file %s in %s (%f kB/s)",
				address, size, args[1], duration_text,
				(float)size / 1024.0 / ((float)duration.duration.tv_sec + ((float)duration.duration.tv_usec / 1000000.0)));

	free(duration_text);
	free(buffer);
	fileio_close(&fileio);

	return ERROR_OK;

mg_dump_cmd_err:
	duration_stop_measure(&duration, &duration_text);
	free(duration_text);
 	free(buffer);
	fileio_close(&fileio);
 
	return ret;	
}

static int mg_set_feature(mg_feature_id feature, mg_feature_val config)
{
	target_t *target = mflash_bank->target;
	u32 mg_task_reg = mflash_bank->base + MG_REG_OFFSET;
	int ret;

	if ((ret = mg_dsk_wait(mg_io_wait_rdy_noerr, MG_OEM_DISK_WAIT_TIME_NORMAL))
			!= ERROR_OK)
		return ret;

	ret = target_write_u8(target, mg_task_reg + MG_REG_FEATURE, feature);
	ret |= target_write_u8(target, mg_task_reg + MG_REG_SECT_CNT, config);
	ret |= target_write_u8(target, mg_task_reg + MG_REG_COMMAND,
			mg_io_cmd_set_feature);

	return ret;
}

static int mg_is_valid_pll(double XIN, int N, double CLK_OUT, int NO)
{
	double v1 = XIN / N;
	double v2 = CLK_OUT * NO;

	if (v1 <1000000 || v1 > 15000000 || v2 < 100000000 || v2 > 500000000)
		return ERROR_MG_INVALID_PLL;

	return ERROR_OK;
}

static int mg_pll_get_M(unsigned short feedback_div)
{
	int i, M;

	for (i = 1, M=0; i < 512; i <<= 1, feedback_div >>= 1)
		M += (feedback_div & 1) * i;

	return M + 2;
}

static int mg_pll_get_N(unsigned char input_div)
{
	int i, N;

	for (i = 1, N = 0; i < 32; i <<= 1, input_div >>= 1)
		N += (input_div & 1) * i;

	return N + 2;
}

static int mg_pll_get_NO(unsigned char  output_div)
{
	int i, NO;

	for (i = 0, NO = 1; i < 2; ++i, output_div >>= 1)
		if(output_div & 1)
			NO = NO << 1;

	return NO;
}

static double mg_do_calc_pll(double XIN, mg_pll_t * p_pll_val, int is_approximate)
{
	unsigned short i;
	unsigned char  j, k;
	int            M, N, NO;
	double CLK_OUT;
	double DIV = 1;
	double ROUND = 0;

	if (is_approximate) {
		DIV   = 1000000;
		ROUND = 500000;
	}

	for (i = 0; i < MG_PLL_MAX_FEEDBACKDIV_VAL ; ++i) {
		M  = mg_pll_get_M(i);

		for (j = 0; j < MG_PLL_MAX_INPUTDIV_VAL ; ++j) {
			N  = mg_pll_get_N(j);

			for (k = 0; k < MG_PLL_MAX_OUTPUTDIV_VAL ; ++k) {
				NO = mg_pll_get_NO(k);

				CLK_OUT = XIN * ((double)M / N) / NO;

				if ((int)((CLK_OUT+ROUND) / DIV)
						== (int)(MG_PLL_CLK_OUT / DIV))	{
					if (mg_is_valid_pll(XIN, N, CLK_OUT, NO) == ERROR_OK)
					{
						p_pll_val->lock_cyc = (int)(XIN * MG_PLL_STD_LOCKCYCLE / MG_PLL_STD_INPUTCLK);
						p_pll_val->feedback_div = i;
						p_pll_val->input_div = j;
						p_pll_val->output_div = k;

						return CLK_OUT;
					}
				}
			}
		}
	}

	return 0;
}

static double mg_calc_pll(double XIN, mg_pll_t *p_pll_val)
{
	double CLK_OUT;

	CLK_OUT = mg_do_calc_pll(XIN, p_pll_val, 0);

	if (!CLK_OUT)
		return mg_do_calc_pll(XIN, p_pll_val, 1);
	else
		return CLK_OUT;
}

static int mg_verify_interface(void)
{
	u16 buff[MG_MFLASH_SECTOR_SIZE >> 1];
	u16 i, j;
	u32 address = mflash_bank->base + MG_BUFFER_OFFSET;
	target_t *target = mflash_bank->target;
	int ret;

	for (j = 0; j < 10; j++) {
		for (i = 0; i < MG_MFLASH_SECTOR_SIZE >> 1; i++)
			buff[i] = i;

		ret = target_write_memory(target, address, 2,
				MG_MFLASH_SECTOR_SIZE / 2, (uint8_t *)buff);
		if (ret != ERROR_OK)
			return ret;

		memset(buff, 0xff, MG_MFLASH_SECTOR_SIZE);

		ret = target_read_memory(target, address, 2,
				MG_MFLASH_SECTOR_SIZE / 2, (uint8_t *)buff);
		if (ret != ERROR_OK)
			return ret;

		for (i = 0; i < MG_MFLASH_SECTOR_SIZE >> 1; i++) {
			if (buff[i] != i) {
				LOG_ERROR("mflash: verify interface fail");
				return ERROR_MG_INTERFACE;
			}
		}
	}

	LOG_INFO("mflash: verify interface ok");
	return ret;
}

static const char g_strSEG_SerialNum[20] = {
	'G','m','n','i','-','e','e','S','g','a','e','l',
	0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20
};

static const char g_strSEG_FWRev[8] = {
	'F','X','L','T','2','v','0','.'
};

static const char g_strSEG_ModelNum[40] = {
	'F','X','A','L','H','S','2',0x20,'0','0','s','7',
	0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
	0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
	0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20
};

static void mg_gen_ataid(mg_io_type_drv_info *pSegIdDrvInfo)
{
	/* b15 is ATA device(0)	, b7 is Removable Media Device */
	pSegIdDrvInfo->general_configuration		= 0x045A;
	/* 128MB :   Cylinder=> 977 , Heads=> 8 ,  Sectors=> 32
	 * 256MB :   Cylinder=> 980 , Heads=> 16 , Sectors=> 32
	 * 384MB :   Cylinder=> 745 , Heads=> 16 , Sectors=> 63
	 */
	pSegIdDrvInfo->number_of_cylinders		= 0x02E9;
	pSegIdDrvInfo->reserved1			= 0x0;
	pSegIdDrvInfo->number_of_heads			= 0x10;
	pSegIdDrvInfo->unformatted_bytes_per_track	= 0x0;
	pSegIdDrvInfo->unformatted_bytes_per_sector	= 0x0;
	pSegIdDrvInfo->sectors_per_track		= 0x3F;
	pSegIdDrvInfo->vendor_unique1[0]		= 0x000B;
	pSegIdDrvInfo->vendor_unique1[1]		= 0x7570;
	pSegIdDrvInfo->vendor_unique1[2]		= 0x8888;

	memcpy(pSegIdDrvInfo->serial_number, (void *)g_strSEG_SerialNum,20);
	/* 0x2 : dual buffer */
	pSegIdDrvInfo->buffer_type			= 0x2;
	/* buffer size : 2KB */
	pSegIdDrvInfo->buffer_sector_size		= 0x800;
	pSegIdDrvInfo->number_of_ecc_bytes		= 0;

	memcpy(pSegIdDrvInfo->firmware_revision, (void *)g_strSEG_FWRev,8);

	memcpy(pSegIdDrvInfo->model_number, (void *)g_strSEG_ModelNum,40);

	pSegIdDrvInfo->maximum_block_transfer		= 0x4;
	pSegIdDrvInfo->vendor_unique2   		= 0x0;
	pSegIdDrvInfo->dword_io				= 0x00;
	/* b11 : IORDY support(PIO Mode 4), b10 : Disable/Enbale IORDY
	 * b9  : LBA support, b8 : DMA mode support
	 */
	pSegIdDrvInfo->capabilities			= 0x1 << 9;

	pSegIdDrvInfo->reserved2			= 0x4000;
	pSegIdDrvInfo->vendor_unique3			= 0x00;
	/* PIOMode-2 support */
	pSegIdDrvInfo->pio_cycle_timing_mode		= 0x02;
	pSegIdDrvInfo->vendor_unique4			= 0x00;
	/* MultiWord-2 support */
	pSegIdDrvInfo->dma_cycle_timing_mode		= 0x00;
	/* b1 : word64~70 is valid
	 * b0 : word54~58 are valid and reflect the current numofcyls,heads,sectors
	 * b2 : If device supports Ultra DMA , set to one to vaildate word88
	 */
	pSegIdDrvInfo->translation_fields_valid		= (0x1 << 1) | (0x1 << 0);
	pSegIdDrvInfo->number_of_current_cylinders	= 0x02E9;
	pSegIdDrvInfo->number_of_current_heads		= 0x10;
	pSegIdDrvInfo->current_sectors_per_track	= 0x3F;
	pSegIdDrvInfo->current_sector_capacity_lo	= 0x7570;
	pSegIdDrvInfo->current_sector_capacity_hi	= 0x000B;

	pSegIdDrvInfo->multi_sector_count			= 0x04;
	/* b8 : Multiple secotr setting valid , b[7:0] num of secotrs per block */
	pSegIdDrvInfo->multi_sector_setting_valid		= 0x01;
	pSegIdDrvInfo->total_user_addressable_sectors_lo	= 0x7570;
	pSegIdDrvInfo->total_user_addressable_sectors_hi	= 0x000B;
	pSegIdDrvInfo->single_dma_modes_supported		= 0x00;
	pSegIdDrvInfo->single_dma_transfer_active		= 0x00;
	/* b2 :Multi-word DMA mode 2, b1 : Multi-word DMA mode 1 */
	pSegIdDrvInfo->multi_dma_modes_supported		= (0x1 << 0);
	/* b2 :Multi-word DMA mode 2, b1 : Multi-word DMA mode 1 */
	pSegIdDrvInfo->multi_dma_transfer_active		= (0x1 << 0);
	/* b0 : PIO Mode-3 support, b1 : PIO Mode-4 support */
	pSegIdDrvInfo->adv_pio_mode				= 0x00;
	/* 480(0x1E0)nsec for Multi-word DMA mode0
	 * 150(0x96) nsec for Multi-word DMA mode1
	 * 120(0x78) nsec for Multi-word DMA mode2
	 */
	pSegIdDrvInfo->min_dma_cyc			= 0x1E0;
	pSegIdDrvInfo->recommend_dma_cyc		= 0x1E0;
	pSegIdDrvInfo->min_pio_cyc_no_iordy		= 0x1E0;
	pSegIdDrvInfo->min_pio_cyc_with_iordy		= 0x1E0;
	memset((void *)pSegIdDrvInfo->reserved3, 0x00, 22);
	/* b7 : ATA/ATAPI-7 ,b6 : ATA/ATAPI-6 ,b5 : ATA/ATAPI-5,b4 : ATA/ATAPI-4 */
	pSegIdDrvInfo->major_ver_num			= 0x7E;
	/* 0x1C : ATA/ATAPI-6 T13 1532D revision1 */
	pSegIdDrvInfo->minor_ver_num			= 0x19;
	/* NOP/READ BUFFER/WRITE BUFFER/Power management feature set support */
	pSegIdDrvInfo->feature_cmd_set_suprt0		= 0x7068;
	/* Features/command set is valid/Advanced Pwr management/CFA feature set
	 * not support
	 */
	pSegIdDrvInfo->feature_cmd_set_suprt1		= 0x400C;
	pSegIdDrvInfo->feature_cmd_set_suprt2		= 0x4000;
	/* READ/WRITE BUFFER/PWR Management enable */
	pSegIdDrvInfo->feature_cmd_set_en0		= 0x7000;
	/* CFA feature is disabled / Advancde power management disable */
	pSegIdDrvInfo->feature_cmd_set_en1		= 0x0;
	pSegIdDrvInfo->feature_cmd_set_en2		= 0x4000;
	pSegIdDrvInfo->reserved4			= 0x0;
	/* 0x1 * 2minutes */
	pSegIdDrvInfo->req_time_for_security_er_done	= 0x19;
	pSegIdDrvInfo->req_time_for_enhan_security_er_done	= 0x19;
	/* Advanced power management level 1 */
	pSegIdDrvInfo->adv_pwr_mgm_lvl_val			= 0x0;
	pSegIdDrvInfo->reserved5			= 0x0;
	memset((void *)pSegIdDrvInfo->reserved6, 0x00, 68);
	/* Security mode feature is disabled */
	pSegIdDrvInfo->security_stas			= 0x0;
	memset((void *)pSegIdDrvInfo->vendor_uniq_bytes, 0x00, 62);
	/* CFA power mode 1 support in maximum 200mA */
	pSegIdDrvInfo->cfa_pwr_mode			= 0x0100;
	memset((void *)pSegIdDrvInfo->reserved7, 0x00, 190);
}

static int mg_storage_config(void)
{
	uint8_t buff[512];
	int ret;

	if ((ret = mg_set_feature(mg_feature_id_transmode, mg_feature_val_trans_vcmd))
			!= ERROR_OK)
		return ret;

	mg_gen_ataid((mg_io_type_drv_info *)buff);

	if ((ret = mg_mflash_do_write_sects(buff, 0, 1, mg_vcmd_update_stgdrvinfo))
			!= ERROR_OK)
		return ret;

	if ((ret = mg_set_feature(mg_feature_id_transmode, mg_feature_val_trans_default))
			!= ERROR_OK)
		return ret;

	LOG_INFO("mflash: storage config ok");
	return ret;
}

static int mg_boot_config(void)
{
	uint8_t buff[512];
	int ret;

	if ((ret = mg_set_feature(mg_feature_id_transmode, mg_feature_val_trans_vcmd))
			!= ERROR_OK)
		return ret;

	memset(buff, 0xff, 512);

	buff[0] = mg_op_mode_snd;		/* operation mode */
	buff[1] = MG_UNLOCK_OTP_AREA;
	buff[2] = 4;				/* boot size */
	*((u32 *)(buff + 4)) = 0;		/* XIP size */

	if ((ret = mg_mflash_do_write_sects(buff, 0, 1, mg_vcmd_update_xipinfo))
			!= ERROR_OK)
		return ret;

	if ((ret = mg_set_feature(mg_feature_id_transmode, mg_feature_val_trans_default))
			!= ERROR_OK)
		return ret;

	LOG_INFO("mflash: boot config ok");
	return ret;
}

static int mg_set_pll(mg_pll_t *pll)
{
	uint8_t buff[512];
	int ret;

	memset(buff, 0xff, 512);
	/* PLL Lock cycle and Feedback 9bit Divider */
	memcpy(buff, &pll->lock_cyc, sizeof(u32));
	memcpy(buff + 4, &pll->feedback_div, sizeof(u16));
	buff[6] = pll->input_div;		/* PLL Input 5bit Divider */
	buff[7] = pll->output_div;		/* PLL Output Divider */

	if ((ret = mg_set_feature(mg_feature_id_transmode, mg_feature_val_trans_vcmd))
			!= ERROR_OK)
		return ret;

	if ((ret = mg_mflash_do_write_sects(buff, 0, 1, mg_vcmd_wr_pll))
			!= ERROR_OK)
		return ret;

	if ((ret = mg_set_feature(mg_feature_id_transmode, mg_feature_val_trans_default))
			!= ERROR_OK)
		return ret;

	LOG_INFO("mflash: set pll ok");
	return ret;
}

static int mg_erase_nand(void)
{
	int ret;

	if ((ret = mg_set_feature(mg_feature_id_transmode, mg_feature_val_trans_vcmd))
			!= ERROR_OK)
		return ret;

	if ((ret = mg_mflash_do_write_sects(NULL, 0, 0, mg_vcmd_purge_nand))
			!= ERROR_OK)
		return ret;

	if ((ret = mg_set_feature(mg_feature_id_transmode, mg_feature_val_trans_default))
			!= ERROR_OK)
		return ret;

	LOG_INFO("mflash: erase nand ok");
	return ret;
}

int mg_config_cmd(struct command_context_s *cmd_ctx, char *cmd,
		char **args, int argc)
{
	double fin, fout;
	mg_pll_t pll;
	int ret;

	if ((ret = mg_verify_interface()) != ERROR_OK)
		return ret;

	if ((ret = mg_mflash_rst()) != ERROR_OK)
		return ret;

	switch (argc) {
		case 2:
			if (!strcmp(args[1], "boot")) 
				return mg_boot_config();
			else if (!strcmp(args[1], "storage"))
				return mg_storage_config();
			else
				return ERROR_COMMAND_NOTFOUND;
			break;
		case 3:
			if (!strcmp(args[1], "pll")) {
				fin = strtoul(args[2], NULL, 0);

				if (fin > MG_PLL_CLK_OUT) {
					LOG_ERROR("mflash: input freq. is too large");
					return ERROR_MG_INVALID_OSC;
				}

				fout = mg_calc_pll(fin, &pll);

				if (!fout) {
					LOG_ERROR("mflash: cannot generate valid pll");
					return ERROR_MG_INVALID_PLL;
				}

				LOG_INFO("mflash: Fout=%u Hz, feedback=%u," 
						"indiv=%u, outdiv=%u, lock=%u",
						(u32)fout, pll.feedback_div,
						pll.input_div, pll.output_div,
						pll.lock_cyc);

				if ((ret = mg_erase_nand()) != ERROR_OK)
					return ret;

				return mg_set_pll(&pll);
			} else
				return ERROR_COMMAND_NOTFOUND;
			break;
		default:
			return ERROR_COMMAND_SYNTAX_ERROR;
	}
}

int mflash_init_drivers(struct command_context_s *cmd_ctx)
{
	if (mflash_bank) {
		register_command(cmd_ctx, mflash_cmd, "probe", mg_probe_cmd, COMMAND_EXEC, NULL);
		register_command(cmd_ctx, mflash_cmd, "write", mg_write_cmd, COMMAND_EXEC,
				"mflash write <num> <file> <address>");
		register_command(cmd_ctx, mflash_cmd, "dump", mg_dump_cmd, COMMAND_EXEC,
						"mflash dump <num> <file> <address> <size>");
		register_command(cmd_ctx, mflash_cmd, "config", mg_config_cmd,
				COMMAND_EXEC, "mflash config <num> <stage>");
	}

	return ERROR_OK;
}

static int mg_bank_cmd(struct command_context_s *cmd_ctx, char *cmd, char **args, int argc)
{
	target_t *target;
	char *str;
	int i;

	if (argc < 4)
	{
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	if ((target = get_target(args[3])) == NULL)
	{
		LOG_ERROR("target '%s' not defined", args[3]);
		return ERROR_FAIL;
	}

	mflash_bank = calloc(sizeof(mflash_bank_t), 1);
	mflash_bank->base = strtoul(args[1], NULL, 0);
	mflash_bank->rst_pin.num = strtoul(args[2], &str, 0);
	if (*str)
		mflash_bank->rst_pin.port[0] = (u16)tolower(str[0]);

	mflash_bank->target = target;

	for (i = 0; mflash_gpio[i] ; i++) {
		if (! strcmp(mflash_gpio[i]->name, args[0])) {
			mflash_bank->gpio_drv = mflash_gpio[i];
		}
	}

	if (! mflash_bank->gpio_drv) {
		LOG_ERROR("%s is unsupported soc", args[0]);
		return ERROR_MG_UNSUPPORTED_SOC;
	}

	return ERROR_OK;
}

int mflash_register_commands(struct command_context_s *cmd_ctx)
{
	mflash_cmd = register_command(cmd_ctx, NULL, "mflash", NULL, COMMAND_ANY, NULL);
	register_command(cmd_ctx, mflash_cmd, "bank", mg_bank_cmd, COMMAND_CONFIG,
			"mflash bank <soc> <base> <RST pin> <target #>");
	return ERROR_OK;
}
