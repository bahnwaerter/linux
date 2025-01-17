/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2012-2013 Freescale Semiconductor, Inc.
 */

#ifndef __FSL_SAI_H
#define __FSL_SAI_H

#include <sound/dmaengine_pcm.h>

#define FSL_SAI_FORMATS (SNDRV_PCM_FMTBIT_S16_LE |\
			 SNDRV_PCM_FMTBIT_S20_3LE |\
			 SNDRV_PCM_FMTBIT_S24_LE |\
			 SNDRV_PCM_FMTBIT_S32_LE)

/* SAI Register Map Register */
#define FSL_SAI_TCSR(ofs)	(0x00 + ofs) /* SAI Transmit Control */
#define FSL_SAI_TCR1(ofs)	(0x04 + ofs) /* SAI Transmit Configuration 1 */
#define FSL_SAI_TCR2(ofs)	(0x08 + ofs) /* SAI Transmit Configuration 2 */
#define FSL_SAI_TCR3(ofs)	(0x0c + ofs) /* SAI Transmit Configuration 3 */
#define FSL_SAI_TCR4(ofs)	(0x10 + ofs) /* SAI Transmit Configuration 4 */
#define FSL_SAI_TCR5(ofs)	(0x14 + ofs) /* SAI Transmit Configuration 5 */
#define FSL_SAI_TDR0	0x20 /* SAI Transmit Data 0 */
#define FSL_SAI_TDR1	0x24 /* SAI Transmit Data 1 */
#define FSL_SAI_TDR2	0x28 /* SAI Transmit Data 2 */
#define FSL_SAI_TDR3	0x2C /* SAI Transmit Data 3 */
#define FSL_SAI_TDR4	0x30 /* SAI Transmit Data 4 */
#define FSL_SAI_TDR5	0x34 /* SAI Transmit Data 5 */
#define FSL_SAI_TDR6	0x38 /* SAI Transmit Data 6 */
#define FSL_SAI_TDR7	0x3C /* SAI Transmit Data 7 */
#define FSL_SAI_TFR0	0x40 /* SAI Transmit FIFO 0 */
#define FSL_SAI_TFR1	0x44 /* SAI Transmit FIFO 1 */
#define FSL_SAI_TFR2	0x48 /* SAI Transmit FIFO 2 */
#define FSL_SAI_TFR3	0x4C /* SAI Transmit FIFO 3 */
#define FSL_SAI_TFR4	0x50 /* SAI Transmit FIFO 4 */
#define FSL_SAI_TFR5	0x54 /* SAI Transmit FIFO 5 */
#define FSL_SAI_TFR6	0x58 /* SAI Transmit FIFO 6 */
#define FSL_SAI_TFR7	0x5C /* SAI Transmit FIFO 7 */
#define FSL_SAI_TMR	0x60 /* SAI Transmit Mask */
#define FSL_SAI_RCSR(ofs)	(0x80 + ofs) /* SAI Receive Control */
#define FSL_SAI_RCR1(ofs)	(0x84 + ofs)/* SAI Receive Configuration 1 */
#define FSL_SAI_RCR2(ofs)	(0x88 + ofs) /* SAI Receive Configuration 2 */
#define FSL_SAI_RCR3(ofs)	(0x8c + ofs) /* SAI Receive Configuration 3 */
#define FSL_SAI_RCR4(ofs)	(0x90 + ofs) /* SAI Receive Configuration 4 */
#define FSL_SAI_RCR5(ofs)	(0x94 + ofs) /* SAI Receive Configuration 5 */
#define FSL_SAI_RDR0	0xa0 /* SAI Receive Data 0 */
#define FSL_SAI_RDR1	0xa4 /* SAI Receive Data 1 */
#define FSL_SAI_RDR2	0xa8 /* SAI Receive Data 2 */
#define FSL_SAI_RDR3	0xac /* SAI Receive Data 3 */
#define FSL_SAI_RDR4	0xb0 /* SAI Receive Data 4 */
#define FSL_SAI_RDR5	0xb4 /* SAI Receive Data 5 */
#define FSL_SAI_RDR6	0xb8 /* SAI Receive Data 6 */
#define FSL_SAI_RDR7	0xbc /* SAI Receive Data 7 */
#define FSL_SAI_RFR0	0xc0 /* SAI Receive FIFO 0 */
#define FSL_SAI_RFR1	0xc4 /* SAI Receive FIFO 1 */
#define FSL_SAI_RFR2	0xc8 /* SAI Receive FIFO 2 */
#define FSL_SAI_RFR3	0xcc /* SAI Receive FIFO 3 */
#define FSL_SAI_RFR4	0xd0 /* SAI Receive FIFO 4 */
#define FSL_SAI_RFR5	0xd4 /* SAI Receive FIFO 5 */
#define FSL_SAI_RFR6	0xd8 /* SAI Receive FIFO 6 */
#define FSL_SAI_RFR7	0xdc /* SAI Receive FIFO 7 */
#define FSL_SAI_RMR	0xe0 /* SAI Receive Mask */

#define FSL_SAI_xCSR(tx, ofs)	(tx ? FSL_SAI_TCSR(ofs) : FSL_SAI_RCSR(ofs))
#define FSL_SAI_xCR1(tx, ofs)	(tx ? FSL_SAI_TCR1(ofs) : FSL_SAI_RCR1(ofs))
#define FSL_SAI_xCR2(tx, ofs)	(tx ? FSL_SAI_TCR2(ofs) : FSL_SAI_RCR2(ofs))
#define FSL_SAI_xCR3(tx, ofs)	(tx ? FSL_SAI_TCR3(ofs) : FSL_SAI_RCR3(ofs))
#define FSL_SAI_xCR4(tx, ofs)	(tx ? FSL_SAI_TCR4(ofs) : FSL_SAI_RCR4(ofs))
#define FSL_SAI_xCR5(tx, ofs)	(tx ? FSL_SAI_TCR5(ofs) : FSL_SAI_RCR5(ofs))
#define FSL_SAI_xDR(tx, ofs)	(tx ? FSL_SAI_TDR(ofs) : FSL_SAI_RDR(ofs))
#define FSL_SAI_xFR(tx, ofs)	(tx ? FSL_SAI_TFR(ofs) : FSL_SAI_RFR(ofs))
#define FSL_SAI_xMR(tx)		(tx ? FSL_SAI_TMR : FSL_SAI_RMR)

/* SAI Transmit/Receive Control Register */
#define FSL_SAI_CSR_TERE	BIT(31)
#define FSL_SAI_CSR_FR		BIT(25)
#define FSL_SAI_CSR_SR		BIT(24)
#define FSL_SAI_CSR_xF_SHIFT	16
#define FSL_SAI_CSR_xF_W_SHIFT	18
#define FSL_SAI_CSR_xF_MASK	(0x1f << FSL_SAI_CSR_xF_SHIFT)
#define FSL_SAI_CSR_xF_W_MASK	(0x7 << FSL_SAI_CSR_xF_W_SHIFT)
#define FSL_SAI_CSR_WSF		BIT(20)
#define FSL_SAI_CSR_SEF		BIT(19)
#define FSL_SAI_CSR_FEF		BIT(18)
#define FSL_SAI_CSR_FWF		BIT(17)
#define FSL_SAI_CSR_FRF		BIT(16)
#define FSL_SAI_CSR_xIE_SHIFT	8
#define FSL_SAI_CSR_xIE_MASK	(0x1f << FSL_SAI_CSR_xIE_SHIFT)
#define FSL_SAI_CSR_WSIE	BIT(12)
#define FSL_SAI_CSR_SEIE	BIT(11)
#define FSL_SAI_CSR_FEIE	BIT(10)
#define FSL_SAI_CSR_FWIE	BIT(9)
#define FSL_SAI_CSR_FRIE	BIT(8)
#define FSL_SAI_CSR_FRDE	BIT(0)

/* SAI Transmit and Receive Configuration 1 Register */
#define FSL_SAI_CR1_RFW_MASK	0x1f

/* SAI Transmit and Receive Configuration 2 Register */
#define FSL_SAI_CR2_SYNC	BIT(30)
#define FSL_SAI_CR2_MSEL_MASK	(0x3 << 26)
#define FSL_SAI_CR2_MSEL_BUS	0
#define FSL_SAI_CR2_MSEL_MCLK1	BIT(26)
#define FSL_SAI_CR2_MSEL_MCLK2	BIT(27)
#define FSL_SAI_CR2_MSEL_MCLK3	(BIT(26) | BIT(27))
#define FSL_SAI_CR2_MSEL(ID)	((ID) << 26)
#define FSL_SAI_CR2_BCP		BIT(25)
#define FSL_SAI_CR2_BCD_MSTR	BIT(24)
#define FSL_SAI_CR2_DIV_MASK	0xff

/* SAI Transmit and Receive Configuration 3 Register */
#define FSL_SAI_CR3_TRCE	BIT(16)
#define FSL_SAI_CR3_TRCE_MASK	GENMASK(23, 16)
#define FSL_SAI_CR3_WDFL(x)	(x)
#define FSL_SAI_CR3_WDFL_MASK	0x1f

/* SAI Transmit and Receive Configuration 4 Register */
#define FSL_SAI_CR4_FRSZ(x)	(((x) - 1) << 16)
#define FSL_SAI_CR4_FRSZ_MASK	(0x1f << 16)
#define FSL_SAI_CR4_SYWD(x)	(((x) - 1) << 8)
#define FSL_SAI_CR4_SYWD_MASK	(0x1f << 8)
#define FSL_SAI_CR4_MF		BIT(4)
#define FSL_SAI_CR4_FSE		BIT(3)
#define FSL_SAI_CR4_FSP		BIT(1)
#define FSL_SAI_CR4_FSD_MSTR	BIT(0)

/* SAI Transmit and Receive Configuration 5 Register */
#define FSL_SAI_CR5_WNW(x)	(((x) - 1) << 24)
#define FSL_SAI_CR5_WNW_MASK	(0x1f << 24)
#define FSL_SAI_CR5_W0W(x)	(((x) - 1) << 16)
#define FSL_SAI_CR5_W0W_MASK	(0x1f << 16)
#define FSL_SAI_CR5_FBT(x)	((x) << 8)
#define FSL_SAI_CR5_FBT_MASK	(0x1f << 8)

/* SAI type */
#define FSL_SAI_DMA		BIT(0)
#define FSL_SAI_USE_AC97	BIT(1)
#define FSL_SAI_NET		BIT(2)
#define FSL_SAI_TRA_SYN		BIT(3)
#define FSL_SAI_REC_SYN		BIT(4)
#define FSL_SAI_USE_I2S_SLAVE	BIT(5)

#define FSL_FMT_TRANSMITTER	0
#define FSL_FMT_RECEIVER	1

/* SAI clock sources */
#define FSL_SAI_CLK_BUS		0
#define FSL_SAI_CLK_MAST1	1
#define FSL_SAI_CLK_MAST2	2
#define FSL_SAI_CLK_MAST3	3

#define FSL_SAI_MCLK_MAX	4

/* SAI data transfer numbers per DMA request */
#define FSL_SAI_MAXBURST_TX 6
#define FSL_SAI_MAXBURST_RX 6

struct fsl_sai_soc_data {
	bool use_imx_pcm;
	unsigned int fifo_depth;
	unsigned int reg_offset;
};

struct fsl_sai {
	struct platform_device *pdev;
	struct regmap *regmap;
	struct clk *bus_clk;
	struct clk *mclk_clk[FSL_SAI_MCLK_MAX];

	bool is_slave_mode;
	bool is_lsb_first;
	bool is_dsp_mode;
	bool synchronous[2];

	unsigned int mclk_id[2];
	unsigned int mclk_streams;
	unsigned int slots;
	unsigned int slot_width;

	const struct fsl_sai_soc_data *soc_data;
	struct snd_dmaengine_dai_dma_data dma_params_rx;
	struct snd_dmaengine_dai_dma_data dma_params_tx;
};

#define TX 1
#define RX 0

#endif /* __FSL_SAI_H */
