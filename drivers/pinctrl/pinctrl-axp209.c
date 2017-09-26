/*
 * AXP20x pinctrl and GPIO driver
 *
 * Copyright (C) 2016 Maxime Ripard <maxime.ripard@free-electrons.com>
 * Copyright (C) 201 Quentin Schulz <quentin.schulz@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under  the terms of the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/gpio/driver.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mfd/axp20x.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/pinconf-generic.h>

#define AXP20X_GPIO_FUNCTIONS		0x7
#define AXP20X_GPIO_FUNCTION_OUT_LOW	0
#define AXP20X_GPIO_FUNCTION_OUT_HIGH	1
#define AXP20X_GPIO_FUNCTION_INPUT	2

#define AXP20X_PINCTRL_PIN(_pin_num, _pin, _regs)		\
	{							\
		.number = _pin_num,				\
		.name = _pin,					\
		.drv_data = _regs,				\
	}

#define AXP20X_PIN(_pin, ...)					\
	{							\
		.pin = _pin,					\
		.functions = (struct axp20x_desc_function[]) {	\
			      __VA_ARGS__, { } },		\
	}

#define AXP20X_FUNCTION(_val, _name)				\
	{							\
		.name = _name,					\
		.muxval = _val,					\
	}

struct axp20x_desc_function {
	const char	*name;
	u8		muxval;
};

struct axp20x_desc_pin {
	struct pinctrl_pin_desc		pin;
	struct axp20x_desc_function	*functions;
};

struct axp20x_pinctrl_desc {
	const struct axp20x_desc_pin	*pins;
	int				npins;
};

struct axp20x_pinctrl_group {
	const char	*name;
	unsigned long	config;
	unsigned int	pin;
};

struct axp20x_pinctrl_function {
	const char	*name;
	const char	**groups;
	unsigned int	ngroups;
};

struct axp20x_pctl {
	struct gpio_chip	chip;
	struct regmap		*regmap;
	struct pinctrl_dev			*pctl_dev;
	struct device				*dev;
	const struct axp20x_pinctrl_desc	*desc;
	struct axp20x_pinctrl_group		*groups;
	unsigned int				ngroups;
	struct axp20x_pinctrl_function		*functions;
	unsigned int				nfunctions;
	unsigned int				gpio_status_offset;
};

static const struct axp20x_desc_pin axp209_pins[] = {
	AXP20X_PIN(AXP20X_PINCTRL_PIN(0, "GPIO0", (void *)AXP20X_GPIO0_CTRL),
		   AXP20X_FUNCTION(0x0, "gpio_out"),
		   AXP20X_FUNCTION(0x2, "gpio_in"),
		   AXP20X_FUNCTION(0x3, "ldo"),
		   AXP20X_FUNCTION(0x4, "adc")),
	AXP20X_PIN(AXP20X_PINCTRL_PIN(1, "GPIO1", (void *)AXP20X_GPIO1_CTRL),
		   AXP20X_FUNCTION(0x0, "gpio_out"),
		   AXP20X_FUNCTION(0x2, "gpio_in"),
		   AXP20X_FUNCTION(0x3, "ldo"),
		   AXP20X_FUNCTION(0x4, "adc")),
	AXP20X_PIN(AXP20X_PINCTRL_PIN(2, "GPIO2", (void *)AXP20X_GPIO2_CTRL),
		   AXP20X_FUNCTION(0x0, "gpio_out"),
		   AXP20X_FUNCTION(0x2, "gpio_in")),
};

static const struct axp20x_desc_pin axp813_pins[] = {
	AXP20X_PIN(AXP20X_PINCTRL_PIN(0, "GPIO0", (void *)AXP20X_GPIO0_CTRL),
		   AXP20X_FUNCTION(0x0, "gpio_out"),
		   AXP20X_FUNCTION(0x2, "gpio_in"),
		   AXP20X_FUNCTION(0x3, "ldo"),
		   AXP20X_FUNCTION(0x4, "adc")),
	AXP20X_PIN(AXP20X_PINCTRL_PIN(1, "GPIO1", (void *)AXP20X_GPIO1_CTRL),
		   AXP20X_FUNCTION(0x0, "gpio_out"),
		   AXP20X_FUNCTION(0x2, "gpio_in"),
		   AXP20X_FUNCTION(0x3, "ldo")),
};

static const struct axp20x_pinctrl_desc axp20x_pinctrl_data = {
	.pins	= axp209_pins,
	.npins	= ARRAY_SIZE(axp209_pins),
};

static const struct axp20x_pinctrl_desc axp813_pinctrl_data = {
	.pins	= axp813_pins,
	.npins	= ARRAY_SIZE(axp813_pins),
};

static int axp20x_gpio_input(struct gpio_chip *chip, unsigned offset)
{
	return pinctrl_gpio_direction_input(chip->base + offset);
}

static int axp20x_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	struct axp20x_pctl *pctl = gpiochip_get_data(chip);
	unsigned int val;
	int ret;

	ret = regmap_read(pctl->regmap, AXP20X_GPIO20_SS, &val);
	if (ret)
		return ret;

	return !!(val & BIT(offset + pctl->gpio_status_offset));
}

static int axp20x_gpio_get_direction(struct gpio_chip *chip, unsigned offset)
{
	struct axp20x_pctl *pctl = gpiochip_get_data(chip);
	int reg = (int)pctl->desc->pins[offset].pin.drv_data;
	unsigned int val;
	int ret;

	ret = regmap_read(pctl->regmap, reg, &val);
	if (ret)
		return ret;

	/*
	 * This shouldn't really happen if the pin is in use already,
	 * or if it's not in use yet, it doesn't matter since we're
	 * going to change the value soon anyway. Default to output.
	 */
	if ((val & AXP20X_GPIO_FUNCTIONS) > 2)
		return 0;

	/*
	 * The GPIO directions are the three lowest values.
	 * 2 is input, 0 and 1 are output
	 */
	return val & 2;
}

static int axp20x_gpio_output(struct gpio_chip *chip, unsigned offset,
			      int value)
{
	chip->set(chip, offset, value);

	return 0;
}

static void axp20x_gpio_set(struct gpio_chip *chip, unsigned offset,
			    int value)
{
	struct axp20x_pctl *pctl = gpiochip_get_data(chip);
	int reg = (int)pctl->desc->pins[offset].pin.drv_data;

	regmap_update_bits(pctl->regmap, reg,
			   AXP20X_GPIO_FUNCTIONS,
			   value ? AXP20X_GPIO_FUNCTION_OUT_HIGH :
			   AXP20X_GPIO_FUNCTION_OUT_LOW);
}

static int axp20x_pmx_set(struct pinctrl_dev *pctldev, unsigned int offset,
			  u8 config)
{
	struct axp20x_pctl *pctl = pinctrl_dev_get_drvdata(pctldev);
	int reg = (int)pctl->desc->pins[offset].pin.drv_data;

	return regmap_update_bits(pctl->regmap, reg, AXP20X_GPIO_FUNCTIONS,
				  config);
}

static int axp20x_pmx_func_cnt(struct pinctrl_dev *pctldev)
{
	struct axp20x_pctl *pctl = pinctrl_dev_get_drvdata(pctldev);

	return pctl->nfunctions;
}

static const char *axp20x_pmx_func_name(struct pinctrl_dev *pctldev,
					unsigned int selector)
{
	struct axp20x_pctl *pctl = pinctrl_dev_get_drvdata(pctldev);

	return pctl->functions[selector].name;
}

static int axp20x_pmx_func_groups(struct pinctrl_dev *pctldev,
				  unsigned int selector,
				  const char * const **groups,
				  unsigned int *num_groups)
{
	struct axp20x_pctl *pctl = pinctrl_dev_get_drvdata(pctldev);

	*groups = pctl->functions[selector].groups;
	*num_groups = pctl->functions[selector].ngroups;

	return 0;
}

static struct axp20x_desc_function *
axp20x_pinctrl_desc_find_func_by_name(struct axp20x_pctl *pctl,
				      const char *group, const char *func)
{
	const struct axp20x_desc_pin *pin;
	struct axp20x_desc_function *desc_func;
	int i;

	for (i = 0; i < pctl->desc->npins; i++) {
		pin = &pctl->desc->pins[i];

		if (!strcmp(pin->pin.name, group)) {
			desc_func = pin->functions;

			while (desc_func->name) {
				if (!strcmp(desc_func->name, func))
					return desc_func;
				desc_func++;
			}

			/*
			 * Pins are uniquely named. Groups are named after one
			 * pin name. If one pin matches group name but its
			 * function cannot be found, no other pin will match
			 * group name.
			 */
			return NULL;
		}
	}

	return NULL;
}

static int axp20x_pmx_set_mux(struct pinctrl_dev *pctldev,
			      unsigned int function, unsigned int group)
{
	struct axp20x_pctl *pctl = pinctrl_dev_get_drvdata(pctldev);
	struct axp20x_pinctrl_group *g = pctl->groups + group;
	struct axp20x_pinctrl_function *func = pctl->functions + function;
	struct axp20x_desc_function *desc_func =
		axp20x_pinctrl_desc_find_func_by_name(pctl, g->name,
						      func->name);
	if (!desc_func)
		return -EINVAL;

	return axp20x_pmx_set(pctldev, g->pin, desc_func->muxval);
}

static struct axp20x_desc_function *
axp20x_pctl_desc_find_func_by_pin(struct axp20x_pctl *pctl, unsigned int offset,
				  const char *func)
{
	const struct axp20x_desc_pin *pin;
	struct axp20x_desc_function *desc_func;
	int i;

	for (i = 0; i < pctl->desc->npins; i++) {
		pin = &pctl->desc->pins[i];

		if (pin->pin.number == offset) {
			desc_func = pin->functions;

			while (desc_func->name) {
				if (!strcmp(desc_func->name, func))
					return desc_func;

				desc_func++;
			}
		}
	}

	return NULL;
}

static int axp20x_pmx_gpio_set_direction(struct pinctrl_dev *pctldev,
					 struct pinctrl_gpio_range *range,
					 unsigned int offset, bool input)
{
	struct axp20x_pctl *pctl = pinctrl_dev_get_drvdata(pctldev);
	struct axp20x_desc_function *desc_func;
	const char *func;

	if (input)
		func = "gpio_in";
	else
		func = "gpio_out";

	desc_func = axp20x_pctl_desc_find_func_by_pin(pctl, offset, func);
	if (!desc_func)
		return -EINVAL;

	return axp20x_pmx_set(pctldev, offset, desc_func->muxval);
}

static const struct pinmux_ops axp20x_pmx_ops = {
	.get_functions_count	= axp20x_pmx_func_cnt,
	.get_function_name	= axp20x_pmx_func_name,
	.get_function_groups	= axp20x_pmx_func_groups,
	.set_mux		= axp20x_pmx_set_mux,
	.gpio_set_direction	= axp20x_pmx_gpio_set_direction,
	.strict			= true,
};

static int axp20x_groups_cnt(struct pinctrl_dev *pctldev)
{
	struct axp20x_pctl *pctl = pinctrl_dev_get_drvdata(pctldev);

	return pctl->ngroups;
}

static int axp20x_group_pins(struct pinctrl_dev *pctldev, unsigned int selector,
			     const unsigned int **pins, unsigned int *num_pins)
{
	struct axp20x_pctl *pctl = pinctrl_dev_get_drvdata(pctldev);
	struct axp20x_pinctrl_group *g = pctl->groups + selector;

	*pins = (unsigned int *)&g->pin;
	*num_pins = 1;

	return 0;
}

static const char *axp20x_group_name(struct pinctrl_dev *pctldev,
				     unsigned int selector)
{
	struct axp20x_pctl *pctl = pinctrl_dev_get_drvdata(pctldev);

	return pctl->groups[selector].name;
}

static const struct pinctrl_ops axp20x_pctrl_ops = {
	.dt_node_to_map		= pinconf_generic_dt_node_to_map_group,
	.dt_free_map		= pinconf_generic_dt_free_map,
	.get_groups_count	= axp20x_groups_cnt,
	.get_group_name		= axp20x_group_name,
	.get_group_pins		= axp20x_group_pins,
};

static struct axp20x_pinctrl_function *
axp20x_pinctrl_function_by_name(struct axp20x_pctl *pctl, const char *name)
{
	struct axp20x_pinctrl_function *func = pctl->functions;

	while (func->name) {
		if (!strcmp(func->name, name))
			return func;
		func++;
	}

	return NULL;
}

static int axp20x_pinctrl_add_function(struct axp20x_pctl *pctl,
				       const char *name)
{
	struct axp20x_pinctrl_function *func = pctl->functions;

	while (func->name) {
		if (!strcmp(func->name, name)) {
			func->ngroups++;
			return -EEXIST;
		}

		func++;
	}

	func->name = name;
	func->ngroups = 1;

	pctl->nfunctions++;

	return 0;
}

static int axp20x_attach_group_function(struct platform_device *pdev,
					const struct axp20x_desc_pin *pin)
{
	struct axp20x_pctl *pctl = platform_get_drvdata(pdev);
	struct axp20x_desc_function *desc_func = pin->functions;
	struct axp20x_pinctrl_function *func;
	const char **func_grp;

	while (desc_func->name) {
		func = axp20x_pinctrl_function_by_name(pctl, desc_func->name);
		if (!func)
			return -EINVAL;

		if (!func->groups) {
			func->groups = devm_kzalloc(&pdev->dev,
						    func->ngroups * sizeof(const char *),
						    GFP_KERNEL);
			if (!func->groups)
				return -ENOMEM;
		}

		func_grp = func->groups;
		while (*func_grp)
			func_grp++;

		*func_grp = pin->pin.name;
		desc_func++;
	}

	return 0;
}

static int axp20x_build_state(struct platform_device *pdev)
{
	struct axp20x_pctl *pctl = platform_get_drvdata(pdev);
	unsigned int npins = pctl->desc->npins;
	const struct axp20x_desc_pin *pin;
	struct axp20x_desc_function *func;
	int i, ret;

	pctl->ngroups = npins;
	pctl->groups = devm_kzalloc(&pdev->dev,
				    pctl->ngroups * sizeof(*pctl->groups),
				    GFP_KERNEL);
	if (!pctl->groups)
		return -ENOMEM;

	for (i = 0; i < npins; i++) {
		pctl->groups[i].name = pctl->desc->pins[i].pin.name;
		pctl->groups[i].pin = pctl->desc->pins[i].pin.number;
	}

	/* We assume 4 functions per pin should be enough as a default max */
	pctl->functions = devm_kzalloc(&pdev->dev,
				       npins * 4 * sizeof(*pctl->functions),
				       GFP_KERNEL);
	if (!pctl->functions)
		return -ENOMEM;

	/* Create a list of uniquely named functions */
	for (i = 0; i < npins; i++) {
		pin = &pctl->desc->pins[i];
		func = pin->functions;

		while (func->name) {
			axp20x_pinctrl_add_function(pctl, func->name);
			func++;
		}
	}

	pctl->functions = krealloc(pctl->functions,
				   pctl->nfunctions * sizeof(*pctl->functions),
				   GFP_KERNEL);

	for (i = 0; i < npins; i++) {
		pin = &pctl->desc->pins[i];
		ret = axp20x_attach_group_function(pdev, pin);
		if (ret)
			return ret;
	}

	return 0;
}

static int axp20x_pctl_probe(struct platform_device *pdev)
{
	struct axp20x_dev *axp20x = dev_get_drvdata(pdev->dev.parent);
	struct axp20x_pctl *pctl;
	struct pinctrl_desc *pctrl_desc;
	struct pinctrl_pin_desc *pins;
	struct device_node *np = pdev->dev.of_node;
	int ret, i;

	if (!of_device_is_available(pdev->dev.of_node))
		return -ENODEV;

	if (!axp20x) {
		dev_err(&pdev->dev, "Parent drvdata not set\n");
		return -EINVAL;
	}

	pctl = devm_kzalloc(&pdev->dev, sizeof(*pctl), GFP_KERNEL);
	if (!pctl)
		return -ENOMEM;

	pctl->chip.base			= -1;
	pctl->chip.can_sleep		= true;
	pctl->chip.request		= gpiochip_generic_request;
	pctl->chip.free			= gpiochip_generic_free;
	pctl->chip.parent		= &pdev->dev;
	pctl->chip.label		= dev_name(&pdev->dev);
	pctl->chip.owner		= THIS_MODULE;
	pctl->chip.get			= axp20x_gpio_get;
	pctl->chip.get_direction	= axp20x_gpio_get_direction;
	pctl->chip.set			= axp20x_gpio_set;
	pctl->chip.direction_input	= axp20x_gpio_input;
	pctl->chip.direction_output	= axp20x_gpio_output;

	pctl->regmap = axp20x->regmap;

	if (of_device_is_compatible(np, "x-powers,axp209-gpio")) {
		pctl->desc = &axp20x_pinctrl_data;
		pctl->gpio_status_offset = 4;
	} else {
		pctl->desc = &axp813_pinctrl_data;
		pctl->gpio_status_offset = 0;
	}
	pctl->dev = &pdev->dev;
	pctl->chip.ngpio = pctl->desc->npins;

	platform_set_drvdata(pdev, pctl);

	ret = axp20x_build_state(pdev);
	if (ret)
		return ret;

	pins = devm_kzalloc(&pdev->dev, pctl->desc->npins * sizeof(*pins),
			    GFP_KERNEL);
	if (!pins)
		return -ENOMEM;

	for (i = 0; i < pctl->desc->npins; i++)
		pins[i] = pctl->desc->pins[i].pin;

	pctrl_desc = devm_kzalloc(&pdev->dev, sizeof(*pctrl_desc), GFP_KERNEL);
	if (!pctrl_desc)
		return -ENOMEM;

	pctrl_desc->name = dev_name(&pdev->dev);
	pctrl_desc->owner = THIS_MODULE;
	pctrl_desc->pins = pins;
	pctrl_desc->npins = pctl->desc->npins;
	pctrl_desc->pctlops = &axp20x_pctrl_ops;
	pctrl_desc->pmxops = &axp20x_pmx_ops;

	pctl->pctl_dev = devm_pinctrl_register(&pdev->dev, pctrl_desc, pctl);
	if (IS_ERR(pctl->pctl_dev)) {
		dev_err(&pdev->dev, "couldn't register pinctrl driver\n");
		return PTR_ERR(pctl->pctl_dev);
	}

	ret = devm_gpiochip_add_data(&pdev->dev, &pctl->chip, pctl);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register GPIO chip\n");
		return ret;
	}

	ret = gpiochip_add_pin_range(&pctl->chip, dev_name(&pdev->dev),
				     pctl->desc->pins->pin.number,
				     pctl->desc->pins->pin.number,
				     pctl->desc->npins);
	if (ret) {
		dev_err(&pdev->dev, "failed to add pin range\n");
		return ret;
	}

	dev_info(&pdev->dev, "AXP209 pinctrl and GPIO driver loaded\n");

	return 0;
}

static const struct of_device_id axp20x_pctl_match[] = {
	{ .compatible = "x-powers,axp209-gpio" },
	{ .compatible = "x-powers,axp813-pctl" },
	{ }
};
MODULE_DEVICE_TABLE(of, axp20x_pctl_match);

static struct platform_driver axp20x_pctl_driver = {
	.probe		= axp20x_pctl_probe,
	.driver = {
		.name		= "axp20x-gpio",
		.of_match_table	= axp20x_pctl_match,
	},
};

module_platform_driver(axp20x_pctl_driver);

MODULE_AUTHOR("Maxime Ripard <maxime.ripard@free-electrons.com>");
MODULE_AUTHOR("Quentin Schulz <quentin.schulz@free-electrons.com>");
MODULE_DESCRIPTION("AXP20x PMIC pinctrl and GPIO driver");
MODULE_LICENSE("GPL");
