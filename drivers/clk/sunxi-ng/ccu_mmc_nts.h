/*
 * Copyright (c) 2016 Maxime Ripard. All rights reserved.
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

#ifndef _CCU_MMC_H_
#define _CCU_MMC_H_

#include <linux/clk-provider.h>

#include "ccu_common.h"
#include "ccu_div.h"
#include "ccu_mult.h"
#include "ccu_mux.h"

/*
 * struct ccu_mmc_nts - Definition of an MMC clock, with a new timing switch
 *
 * Clocks based on the formula parent >> P / M, plus handles the new
 * timing switch
 */
struct ccu_mmc {
	u32				enable;
	u32				nts;

	struct ccu_div_internal		m;
	struct ccu_div_internal		p;
	struct ccu_mux_internal		mux;
	struct ccu_common		common;
};

#define SUNXI_CCU_MMC_NTS(_struct, _name, _parents, _reg, _flags)	\
	struct ccu_mmc _struct = {					\
		.enable	= BIT(31),					\
		.nts	= BIT(30),					\
		.m	= _SUNXI_CCU_DIV(0, 4),				\
		.p	= _SUNXI_CCU_DIV(16, 2),			\
		.mux	= _SUNXI_CCU_MUX(24, 2),			\
		.common	= {						\
			.reg		= _reg,				\
			.hw.init	= CLK_HW_INIT_PARENTS(_name,	\
							      _parents, \
							      &ccu_mmc_ops, \
							      _flags),	\
			.features	= CCU_FEATURE_MMC_TIMING_SWITCH, \
		}							\
	}

static inline struct ccu_mmc *hw_to_ccu_mmc(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);

	return container_of(common, struct ccu_mmc, common);
}

extern const struct clk_ops ccu_mmc_ops;

#endif /* _CCU_MMC_H_ */
