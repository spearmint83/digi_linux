/*
 * Copyright (C) 2016 Digi International, Inc.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/i2c.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/control.h>
#include <sound/pcm_params.h>
#include <sound/soc-dapm.h>
#include <linux/pinctrl/consumer.h>
#include <linux/mfd/syscon.h>
#include "../codecs/max98088.h"
#include "fsl_sai.h"

struct imx_priv {
	struct snd_soc_component *component;
	struct platform_device *pdev;
};

struct imx_max98088_data {
	struct snd_soc_dai_link dai;
	struct snd_soc_card card;
	struct clk *mclk;
	bool is_codec_master;
	struct regmap *gpr;
};

static struct imx_priv card_priv;

static const struct snd_soc_dapm_widget imx_max98088_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_SPK("Ext Spk", NULL),
	SND_SOC_DAPM_MIC("Mic1", NULL),
	SND_SOC_DAPM_MIC("Mic2", NULL),
	SND_SOC_DAPM_LINE("LineInA", NULL),
	SND_SOC_DAPM_LINE("LineInB", NULL),
	SND_SOC_DAPM_LINE("LineOut", NULL),
};

static int imx_hifi_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = asoc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = asoc_rtd_to_cpu(rtd, 0);
	struct snd_soc_card *card = rtd->card;
	struct imx_max98088_data *data = snd_soc_card_get_drvdata(card);
	struct device *dev = card->dev;
	unsigned int fmt;
	int ret = 0;

	/* Codec always slave, master is not supported */
	fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_CBS_CFS;

	/* Set cpu DAI configuration */
	ret = snd_soc_dai_set_fmt(cpu_dai, fmt);
	if (ret) {
		dev_err(dev, "failed to set cpu dai fmt: %d\n", ret);
		return ret;
	}

	/* set codec DAI configuration */
	ret = snd_soc_dai_set_fmt(codec_dai, fmt);
	if (ret) {
		dev_err(dev, "failed to set codec dai fmt: %d\n", ret);
		return ret;
	}

	/* Select I2S Bus clock to set RCLK and BCLK */
	ret = snd_soc_dai_set_sysclk(cpu_dai, 0, 0, SND_SOC_CLOCK_OUT);
	if (ret) {
		dev_err(dev, "failed to set cpu sysclk: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0, 0, 2, params_width(params));
	if (ret) {
		dev_err(dev, "failed to set cpu dai tdm slot: %d\n", ret);
		return ret;
	}

	/* Set the default MCLK rate for the codec */
	ret = snd_soc_dai_set_sysclk(codec_dai, 0,
				     clk_get_rate(data->mclk),
				     SND_SOC_CLOCK_IN);
	if (ret) {
		dev_err(dev, "failed to set codec sysclock: %d\n", ret);
		return ret;
	}

	return ret;
}

/* Exact audio frequencies for the selected master clock */
static u32 imx_max98088_rates[] = {
	8000, 12000, 16000, 24000, 32000, 48000, 64000, 96000
};
static struct snd_pcm_hw_constraint_list imx_max98088_rate_constraints = {
	.count = ARRAY_SIZE(imx_max98088_rates),
	.list = imx_max98088_rates,
};

static int imx_hifi_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct imx_max98088_data *data = snd_soc_card_get_drvdata(card);
	int ret = 0;

	ret = snd_pcm_hw_constraint_list(substream->runtime, 0,
					 SNDRV_PCM_HW_PARAM_RATE,
					 &imx_max98088_rate_constraints);
	if (ret)
		return ret;

	ret = clk_prepare_enable(data->mclk);
	if (ret) {
		dev_err(card->dev, "Failed to enable MCLK: %d\n", ret);
		return ret;
	}

	return ret;
}

static void imx_hifi_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct imx_max98088_data *data = snd_soc_card_get_drvdata(card);

	clk_disable_unprepare(data->mclk);
}

static struct snd_soc_ops imx_hifi_ops = {
	.hw_params = imx_hifi_hw_params,
	.startup   = imx_hifi_startup,
	.shutdown  = imx_hifi_shutdown,
};

static int imx_max98088_probe(struct platform_device *pdev)
{
	struct device_node *cpu_np, *codec_np = NULL;
	struct device_node *gpr_np;
	struct platform_device *cpu_pdev;
	struct imx_priv *priv = &card_priv;
	struct i2c_client *codec_dev;
	struct imx_max98088_data *data;
	struct snd_soc_dai_link_component *dlc;
	int ret;

	priv->pdev = pdev;

	dlc = devm_kzalloc(&pdev->dev, 3 * sizeof(*dlc), GFP_KERNEL);
	if (!dlc)
		return -ENOMEM;

	cpu_np = of_parse_phandle(pdev->dev.of_node, "cpu-dai", 0);
	if (!cpu_np) {
		dev_err(&pdev->dev, "cpu dai phandle missing or invalid\n");
		ret = -EINVAL;
		goto fail;
	}

	codec_np = of_parse_phandle(pdev->dev.of_node, "audio-codec", 0);
	if (!codec_np) {
		dev_err(&pdev->dev, "phandle missing or invalid\n");
		ret = -EINVAL;
		goto fail;
	}

	cpu_pdev = of_find_device_by_node(cpu_np);
	if (!cpu_pdev) {
		dev_err(&pdev->dev, "failed to find SAI platform device\n");
		ret = -EINVAL;
		goto fail;
	}

	codec_dev = of_find_i2c_device_by_node(codec_np);
	if (!codec_dev || !codec_dev->dev.driver) {
		dev_err(&pdev->dev, "failed to find codec platform device\n");
		ret = -EPROBE_DEFER;
		goto fail;
	}

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto fail;
	}

	/* Only SAI as master is supported */
	data->is_codec_master = false;

	data->mclk = devm_clk_get(&codec_dev->dev, "mclk");
	if (IS_ERR(data->mclk)) {
		ret = PTR_ERR(data->mclk);
		dev_err(&pdev->dev, "failed to get codec clk: %d\n", ret);
		goto fail;
	}

	gpr_np = of_parse_phandle(pdev->dev.of_node, "gpr", 0);
	if (gpr_np) {
		data->gpr = syscon_node_to_regmap(gpr_np);
		if (IS_ERR(data->gpr)) {
			ret = PTR_ERR(data->gpr);
			dev_err(&pdev->dev, "failed to get gpr regmap\n");
			goto fail;
		}

		/* set SAI2_MCLK_DIR to enable codec MCLK */
		regmap_update_bits(data->gpr, 4, 1 << 20, 1 << 20);
	}

	data->dai.cpus = &dlc[0];
	data->dai.num_cpus = 1;
	data->dai.platforms = &dlc[1];
	data->dai.num_platforms = 1;
	data->dai.codecs = &dlc[2];
	data->dai.num_codecs = 1;

	data->dai.name = "HiFi";
	data->dai.stream_name = "HiFi";
	data->dai.codecs->dai_name = "HiFi";
	data->dai.codecs->of_node = codec_np;
	data->dai.cpus->dai_name = dev_name(&cpu_pdev->dev);
	data->dai.platforms->of_node = cpu_np;
	data->dai.ops = &imx_hifi_ops;
	data->dai.dai_fmt |= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_CBS_CFS;
	data->card.owner = THIS_MODULE;

	data->card.dev = &pdev->dev;
	ret = snd_soc_of_parse_card_name(&data->card, "model");
	if (ret)
		goto fail;

	data->card.num_links = 1;
	data->card.dai_link = &data->dai;
	data->card.dapm_widgets = imx_max98088_dapm_widgets;
	data->card.num_dapm_widgets = ARRAY_SIZE(imx_max98088_dapm_widgets);

	platform_set_drvdata(pdev, &data->card);
	snd_soc_card_set_drvdata(&data->card, data);

	ret = snd_soc_of_parse_audio_routing(&data->card, "audio-routing");
	if (ret)
		goto fail;

	platform_set_drvdata(pdev, &data->card);
	snd_soc_card_set_drvdata(&data->card, data);
	ret = devm_snd_soc_register_card(&pdev->dev, &data->card);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n", ret);
		goto fail;
	}

fail:
	if (cpu_np)
		of_node_put(cpu_np);
	if (codec_np)
		of_node_put(codec_np);

	return ret;
}

static int imx_max98088_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct imx_max98088_data *data = snd_soc_card_get_drvdata(card);

	snd_soc_unregister_card(card);
	clk_put(data->mclk);
	kfree(data);

	return 0;
}

static const struct of_device_id imx_max98088_dt_ids[] = {
	{ .compatible = "fsl,imx-audio-max98088", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx_max98088_dt_ids);

static struct platform_driver imx_max98088_driver = {
	.driver = {
		.name = "imx-max98088",
		.pm = &snd_soc_pm_ops,
		.of_match_table = imx_max98088_dt_ids,
	},
	.probe = imx_max98088_probe,
	.remove = imx_max98088_remove,
};
module_platform_driver(imx_max98088_driver);

MODULE_AUTHOR("Digi International, Inc.");
MODULE_DESCRIPTION("Freescale i.MX max98088 ASoC machine driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:imx-max98088");
