/*
 * Copyright (C) 2016 Maxime Ripard
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <linux/clk-provider.h>

#include "ccu_gate.h"
#include "ccu_mmc_nts.h"

static void ccu_mmc_find_best(unsigned long parent, unsigned long rate,
			      unsigned int max_m, unsigned int max_p,
			      unsigned int *m, unsigned int *p)
{
	unsigned long best_rate = 0;
	unsigned int best_m = 0, best_p = 0;
	unsigned int _m, _p;

	for (_p = 1; _p <= max_p; _p <<= 1) {
		for (_m = 1; _m <= max_m; _m++) {
			unsigned long tmp_rate = parent / _p / _m;

			if (tmp_rate > rate)
				continue;

			if ((rate - tmp_rate) < (rate - best_rate)) {
				best_rate = tmp_rate;
				best_m = _m;
				best_p = _p;
			}
		}
	}

	*m = best_m;
	*p = best_p;
}

static unsigned long ccu_mmc_round_rate(struct ccu_mux_internal *mux,
				       struct clk_hw *hw,
				       unsigned long *parent_rate,
				       unsigned long rate,
				       void *data)
{
	struct ccu_mmc *mmc = data;
	unsigned int max_m, max_p;
	unsigned int m, p;

	max_m = mmc->m.max ?: 1 << mmc->m.width;
	max_p = mmc->p.max ?: 1 << ((1 << mmc->p.width) - 1);

	ccu_mmc_find_best(*parent_rate, rate, max_m, max_p, &m, &p);

	printk("%s %d: parent %lu M %d P %d\n", __func__, __LINE__,
	       *parent_rate, m, p);

	return *parent_rate / p / m;
}

static void ccu_mmc_disable(struct clk_hw *hw)
{
	struct ccu_mmc *mmc = hw_to_ccu_mmc(hw);

	return ccu_gate_helper_disable(&mmc->common, mmc->enable);
}

static int ccu_mmc_enable(struct clk_hw *hw)
{
	struct ccu_mmc *mmc = hw_to_ccu_mmc(hw);

	return ccu_gate_helper_enable(&mmc->common, mmc->enable);
}

static int ccu_mmc_is_enabled(struct clk_hw *hw)
{
	struct ccu_mmc *mmc = hw_to_ccu_mmc(hw);

	return ccu_gate_helper_is_enabled(&mmc->common, mmc->enable);
}

static unsigned long ccu_mmc_recalc_rate(struct clk_hw *hw,
					unsigned long parent_rate)
{
	struct ccu_mmc *mmc = hw_to_ccu_mmc(hw);
	unsigned long rate;
	unsigned int m, p;
	u32 reg;

	reg = readl(mmc->common.base + mmc->common.reg);

	m = reg >> mmc->m.shift;
	m &= (1 << mmc->m.width) - 1;
	m += mmc->m.offset;
	if (!m)
		m++;

	p = reg >> mmc->p.shift;
	p &= (1 << mmc->p.width) - 1;

	rate = (parent_rate >> p) / m;
	if (reg & mmc->nts)
		rate = rate / 2;

	return rate;
}

static int ccu_mmc_determine_rate(struct clk_hw *hw,
				 struct clk_rate_request *req)
{
	struct ccu_mmc *mmc = hw_to_ccu_mmc(hw);
	u32 reg;
	int ret;

	printk("%s %d: rate %lu parent %lu\n",
	       __func__, __LINE__, req->rate, req->best_parent_rate);

	reg = readl(mmc->common.base + mmc->common.reg);
	if (reg & mmc->nts) {
		req->rate = req->rate * 2;
		printk("%s %d: new mode detected, rate %lu\n",
		       __func__, __LINE__, req->rate);
	}

	ret = ccu_mux_helper_determine_rate(&mmc->common, &mmc->mux,
					    req, ccu_mmc_round_rate, mmc);
	if (reg & mmc->nts) {
		req->rate = req->rate / 2;
		printk("%s %d: new mode detected, rate %lu\n",
		       __func__, __LINE__, req->rate);
	}

	return ret;
}

static int ccu_mmc_set_rate(struct clk_hw *hw, unsigned long rate,
			   unsigned long parent_rate)
{
	struct ccu_mmc *mmc = hw_to_ccu_mmc(hw);
	unsigned long flags;
	unsigned int max_m, max_p;
	unsigned int m, p;
	u32 reg;

	max_m = mmc->m.max ?: 1 << mmc->m.width;
	max_p = mmc->p.max ?: 1 << ((1 << mmc->p.width) - 1);

	printk("%s %d: rate %lu parent %lu\n",
	       __func__, __LINE__, rate, parent_rate);

	spin_lock_irqsave(mmc->common.lock, flags);

	reg = readl(mmc->common.base + mmc->common.reg);
	if (reg & mmc->nts) {
		rate = rate * 2;
		printk("%s %d: new mode detected, rate %lu\n",
		       __func__, __LINE__, rate);
	}

	ccu_mmc_find_best(parent_rate, rate, max_m, max_p, &m, &p);

	reg = readl(mmc->common.base + mmc->common.reg);
	reg &= ~GENMASK(mmc->m.width + mmc->m.shift - 1, mmc->m.shift);
	reg &= ~GENMASK(mmc->p.width + mmc->p.shift - 1, mmc->p.shift);
	reg |= (m - mmc->m.offset) << mmc->m.shift;
	reg |= ilog2(p) << mmc->p.shift;

	printk("%s %d: M %d P %d\n", __func__, __LINE__, m, p);

	writel(reg, mmc->common.base + mmc->common.reg);

	spin_unlock_irqrestore(mmc->common.lock, flags);

	return 0;
}

static u8 ccu_mmc_get_parent(struct clk_hw *hw)
{
	struct ccu_mmc *mmc = hw_to_ccu_mmc(hw);

	return ccu_mux_helper_get_parent(&mmc->common, &mmc->mux);
}

static int ccu_mmc_set_parent(struct clk_hw *hw, u8 index)
{
	struct ccu_mmc *mmc = hw_to_ccu_mmc(hw);

	return ccu_mux_helper_set_parent(&mmc->common, &mmc->mux, index);
}

const struct clk_ops ccu_mmc_ops = {
	.disable	= ccu_mmc_disable,
	.enable		= ccu_mmc_enable,
	.is_enabled	= ccu_mmc_is_enabled,

	.get_parent	= ccu_mmc_get_parent,
	.set_parent	= ccu_mmc_set_parent,

	.determine_rate	= ccu_mmc_determine_rate,
	.recalc_rate	= ccu_mmc_recalc_rate,
	.set_rate	= ccu_mmc_set_rate,
};
