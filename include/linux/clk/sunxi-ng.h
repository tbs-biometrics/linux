/*
 * Copyright (c) 2017 Chen-Yu Tsai. All rights reserved.
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

#ifndef _LINUX_CLK_SUNXI_NG_H_
#define _LINUX_CLK_SUNXI_NG_H_

int sunxi_ccu_set_mmc_timing_mode(struct clk *clk, bool new_mode);
int sunxi_ccu_get_mmc_timing_mode(struct clk *clk);

#endif
