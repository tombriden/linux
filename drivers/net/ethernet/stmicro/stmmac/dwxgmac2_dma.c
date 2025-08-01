// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * Copyright (c) 2018 Synopsys, Inc. and/or its affiliates.
 * stmmac XGMAC support.
 */

#include <linux/iopoll.h>
#include "stmmac.h"
#include "dwxgmac2.h"

static int dwxgmac2_dma_reset(void __iomem *ioaddr)
{
	u32 value = readl(ioaddr + XGMAC_DMA_MODE);

	/* DMA SW reset */
	writel(value | XGMAC_SWR, ioaddr + XGMAC_DMA_MODE);

	return readl_poll_timeout(ioaddr + XGMAC_DMA_MODE, value,
				  !(value & XGMAC_SWR), 0, 100000);
}

static void dwxgmac2_dma_init(void __iomem *ioaddr,
			      struct stmmac_dma_cfg *dma_cfg)
{
	u32 value = readl(ioaddr + XGMAC_DMA_SYSBUS_MODE);

	if (dma_cfg->aal)
		value |= XGMAC_AAL;

	if (dma_cfg->eame)
		value |= XGMAC_EAME;

	writel(value, ioaddr + XGMAC_DMA_SYSBUS_MODE);
}

static void dwxgmac2_dma_init_chan(struct stmmac_priv *priv,
				   void __iomem *ioaddr,
				   struct stmmac_dma_cfg *dma_cfg, u32 chan)
{
	u32 value = readl(ioaddr + XGMAC_DMA_CH_CONTROL(chan));

	if (dma_cfg->pblx8)
		value |= XGMAC_PBLx8;

	writel(value, ioaddr + XGMAC_DMA_CH_CONTROL(chan));
	writel(XGMAC_DMA_INT_DEFAULT_EN, ioaddr + XGMAC_DMA_CH_INT_EN(chan));
}

static void dwxgmac2_dma_init_rx_chan(struct stmmac_priv *priv,
				      void __iomem *ioaddr,
				      struct stmmac_dma_cfg *dma_cfg,
				      dma_addr_t phy, u32 chan)
{
	u32 rxpbl = dma_cfg->rxpbl ?: dma_cfg->pbl;
	u32 value;

	value = readl(ioaddr + XGMAC_DMA_CH_RX_CONTROL(chan));
	value &= ~XGMAC_RxPBL;
	value |= (rxpbl << XGMAC_RxPBL_SHIFT) & XGMAC_RxPBL;
	writel(value, ioaddr + XGMAC_DMA_CH_RX_CONTROL(chan));

	writel(upper_32_bits(phy), ioaddr + XGMAC_DMA_CH_RxDESC_HADDR(chan));
	writel(lower_32_bits(phy), ioaddr + XGMAC_DMA_CH_RxDESC_LADDR(chan));
}

static void dwxgmac2_dma_init_tx_chan(struct stmmac_priv *priv,
				      void __iomem *ioaddr,
				      struct stmmac_dma_cfg *dma_cfg,
				      dma_addr_t phy, u32 chan)
{
	u32 txpbl = dma_cfg->txpbl ?: dma_cfg->pbl;
	u32 value;

	value = readl(ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan));
	value &= ~XGMAC_TxPBL;
	value |= (txpbl << XGMAC_TxPBL_SHIFT) & XGMAC_TxPBL;
	value |= XGMAC_OSP;
	writel(value, ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan));

	writel(upper_32_bits(phy), ioaddr + XGMAC_DMA_CH_TxDESC_HADDR(chan));
	writel(lower_32_bits(phy), ioaddr + XGMAC_DMA_CH_TxDESC_LADDR(chan));
}

static void dwxgmac2_dma_axi(void __iomem *ioaddr, struct stmmac_axi *axi)
{
	u32 value = readl(ioaddr + XGMAC_DMA_SYSBUS_MODE);
	int i;

	if (axi->axi_lpi_en)
		value |= XGMAC_EN_LPI;
	if (axi->axi_xit_frm)
		value |= XGMAC_LPI_XIT_PKT;

	value &= ~XGMAC_WR_OSR_LMT;
	value |= (axi->axi_wr_osr_lmt << XGMAC_WR_OSR_LMT_SHIFT) &
		XGMAC_WR_OSR_LMT;

	value &= ~XGMAC_RD_OSR_LMT;
	value |= (axi->axi_rd_osr_lmt << XGMAC_RD_OSR_LMT_SHIFT) &
		XGMAC_RD_OSR_LMT;

	if (!axi->axi_fb)
		value |= XGMAC_UNDEF;

	value &= ~XGMAC_BLEN;
	for (i = 0; i < AXI_BLEN; i++) {
		switch (axi->axi_blen[i]) {
		case 256:
			value |= XGMAC_BLEN256;
			break;
		case 128:
			value |= XGMAC_BLEN128;
			break;
		case 64:
			value |= XGMAC_BLEN64;
			break;
		case 32:
			value |= XGMAC_BLEN32;
			break;
		case 16:
			value |= XGMAC_BLEN16;
			break;
		case 8:
			value |= XGMAC_BLEN8;
			break;
		case 4:
			value |= XGMAC_BLEN4;
			break;
		}
	}

	writel(value, ioaddr + XGMAC_DMA_SYSBUS_MODE);
	writel(XGMAC_TDPS, ioaddr + XGMAC_TX_EDMA_CTRL);
	writel(XGMAC_RDPS, ioaddr + XGMAC_RX_EDMA_CTRL);
}

static void dwxgmac2_dma_dump_regs(struct stmmac_priv *priv,
				   void __iomem *ioaddr, u32 *reg_space)
{
	int i;

	for (i = (XGMAC_DMA_MODE / 4); i < XGMAC_REGSIZE; i++)
		reg_space[i] = readl(ioaddr + i * 4);
}

static void dwxgmac2_dma_rx_mode(struct stmmac_priv *priv, void __iomem *ioaddr,
				 int mode, u32 channel, int fifosz, u8 qmode)
{
	u32 value = readl(ioaddr + XGMAC_MTL_RXQ_OPMODE(channel));
	unsigned int rqs = fifosz / 256 - 1;

	if (mode == SF_DMA_MODE) {
		value |= XGMAC_RSF;
	} else {
		value &= ~XGMAC_RSF;
		value &= ~XGMAC_RTC;

		if (mode <= 64)
			value |= 0x0 << XGMAC_RTC_SHIFT;
		else if (mode <= 96)
			value |= 0x2 << XGMAC_RTC_SHIFT;
		else
			value |= 0x3 << XGMAC_RTC_SHIFT;
	}

	value &= ~XGMAC_RQS;
	value |= (rqs << XGMAC_RQS_SHIFT) & XGMAC_RQS;

	if ((fifosz >= 4096) && (qmode != MTL_QUEUE_AVB)) {
		u32 flow = readl(ioaddr + XGMAC_MTL_RXQ_FLOW_CONTROL(channel));
		unsigned int rfd, rfa;

		value |= XGMAC_EHFC;

		/* Set Threshold for Activating Flow Control to min 2 frames,
		 * i.e. 1500 * 2 = 3000 bytes.
		 *
		 * Set Threshold for Deactivating Flow Control to min 1 frame,
		 * i.e. 1500 bytes.
		 */
		switch (fifosz) {
		case 4096:
			/* This violates the above formula because of FIFO size
			 * limit therefore overflow may occur in spite of this.
			 */
			rfd = 0x03; /* Full-2.5K */
			rfa = 0x01; /* Full-1.5K */
			break;

		default:
			rfd = 0x07; /* Full-4.5K */
			rfa = 0x04; /* Full-3K */
			break;
		}

		flow &= ~XGMAC_RFD;
		flow |= rfd << XGMAC_RFD_SHIFT;

		flow &= ~XGMAC_RFA;
		flow |= rfa << XGMAC_RFA_SHIFT;

		writel(flow, ioaddr + XGMAC_MTL_RXQ_FLOW_CONTROL(channel));
	}

	writel(value, ioaddr + XGMAC_MTL_RXQ_OPMODE(channel));

	/* Enable MTL RX overflow */
	value = readl(ioaddr + XGMAC_MTL_QINTEN(channel));
	writel(value | XGMAC_RXOIE, ioaddr + XGMAC_MTL_QINTEN(channel));
}

static void dwxgmac2_dma_tx_mode(struct stmmac_priv *priv, void __iomem *ioaddr,
				 int mode, u32 channel, int fifosz, u8 qmode)
{
	u32 value = readl(ioaddr + XGMAC_MTL_TXQ_OPMODE(channel));
	unsigned int tqs = fifosz / 256 - 1;

	if (mode == SF_DMA_MODE) {
		value |= XGMAC_TSF;
	} else {
		value &= ~XGMAC_TSF;
		value &= ~XGMAC_TTC;

		if (mode <= 64)
			value |= 0x0 << XGMAC_TTC_SHIFT;
		else if (mode <= 96)
			value |= 0x2 << XGMAC_TTC_SHIFT;
		else if (mode <= 128)
			value |= 0x3 << XGMAC_TTC_SHIFT;
		else if (mode <= 192)
			value |= 0x4 << XGMAC_TTC_SHIFT;
		else if (mode <= 256)
			value |= 0x5 << XGMAC_TTC_SHIFT;
		else if (mode <= 384)
			value |= 0x6 << XGMAC_TTC_SHIFT;
		else
			value |= 0x7 << XGMAC_TTC_SHIFT;
	}

	/* Use static TC to Queue mapping */
	value |= (channel << XGMAC_Q2TCMAP_SHIFT) & XGMAC_Q2TCMAP;

	value &= ~XGMAC_TXQEN;
	if (qmode != MTL_QUEUE_AVB)
		value |= 0x2 << XGMAC_TXQEN_SHIFT;
	else
		value |= 0x1 << XGMAC_TXQEN_SHIFT;

	value &= ~XGMAC_TQS;
	value |= (tqs << XGMAC_TQS_SHIFT) & XGMAC_TQS;

	writel(value, ioaddr +  XGMAC_MTL_TXQ_OPMODE(channel));
}

static void dwxgmac2_enable_dma_irq(struct stmmac_priv *priv,
				    void __iomem *ioaddr, u32 chan,
				    bool rx, bool tx)
{
	u32 value = readl(ioaddr + XGMAC_DMA_CH_INT_EN(chan));

	if (rx)
		value |= XGMAC_DMA_INT_DEFAULT_RX;
	if (tx)
		value |= XGMAC_DMA_INT_DEFAULT_TX;

	writel(value, ioaddr + XGMAC_DMA_CH_INT_EN(chan));
}

static void dwxgmac2_disable_dma_irq(struct stmmac_priv *priv,
				     void __iomem *ioaddr, u32 chan,
				     bool rx, bool tx)
{
	u32 value = readl(ioaddr + XGMAC_DMA_CH_INT_EN(chan));

	if (rx)
		value &= ~XGMAC_DMA_INT_DEFAULT_RX;
	if (tx)
		value &= ~XGMAC_DMA_INT_DEFAULT_TX;

	writel(value, ioaddr + XGMAC_DMA_CH_INT_EN(chan));
}

static void dwxgmac2_dma_start_tx(struct stmmac_priv *priv,
				  void __iomem *ioaddr, u32 chan)
{
	u32 value;

	value = readl(ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan));
	value |= XGMAC_TXST;
	writel(value, ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan));

	value = readl(ioaddr + XGMAC_TX_CONFIG);
	value |= XGMAC_CONFIG_TE;
	writel(value, ioaddr + XGMAC_TX_CONFIG);
}

static void dwxgmac2_dma_stop_tx(struct stmmac_priv *priv, void __iomem *ioaddr,
				 u32 chan)
{
	u32 value;

	value = readl(ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan));
	value &= ~XGMAC_TXST;
	writel(value, ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan));

	value = readl(ioaddr + XGMAC_TX_CONFIG);
	value &= ~XGMAC_CONFIG_TE;
	writel(value, ioaddr + XGMAC_TX_CONFIG);
}

static void dwxgmac2_dma_start_rx(struct stmmac_priv *priv,
				  void __iomem *ioaddr, u32 chan)
{
	u32 value;

	value = readl(ioaddr + XGMAC_DMA_CH_RX_CONTROL(chan));
	value |= XGMAC_RXST;
	writel(value, ioaddr + XGMAC_DMA_CH_RX_CONTROL(chan));

	value = readl(ioaddr + XGMAC_RX_CONFIG);
	value |= XGMAC_CONFIG_RE;
	writel(value, ioaddr + XGMAC_RX_CONFIG);
}

static void dwxgmac2_dma_stop_rx(struct stmmac_priv *priv, void __iomem *ioaddr,
				 u32 chan)
{
	u32 value;

	value = readl(ioaddr + XGMAC_DMA_CH_RX_CONTROL(chan));
	value &= ~XGMAC_RXST;
	writel(value, ioaddr + XGMAC_DMA_CH_RX_CONTROL(chan));
}

static int dwxgmac2_dma_interrupt(struct stmmac_priv *priv,
				  void __iomem *ioaddr,
				  struct stmmac_extra_stats *x, u32 chan,
				  u32 dir)
{
	struct stmmac_pcpu_stats *stats = this_cpu_ptr(priv->xstats.pcpu_stats);
	u32 intr_status = readl(ioaddr + XGMAC_DMA_CH_STATUS(chan));
	u32 intr_en = readl(ioaddr + XGMAC_DMA_CH_INT_EN(chan));
	int ret = 0;

	if (dir == DMA_DIR_RX)
		intr_status &= XGMAC_DMA_STATUS_MSK_RX;
	else if (dir == DMA_DIR_TX)
		intr_status &= XGMAC_DMA_STATUS_MSK_TX;

	/* ABNORMAL interrupts */
	if (unlikely(intr_status & XGMAC_AIS)) {
		if (unlikely(intr_status & XGMAC_RBU)) {
			x->rx_buf_unav_irq++;
			ret |= handle_rx;
		}
		if (unlikely(intr_status & XGMAC_TPS)) {
			x->tx_process_stopped_irq++;
			ret |= tx_hard_error;
		}
		if (unlikely(intr_status & XGMAC_FBE)) {
			x->fatal_bus_error_irq++;
			ret |= tx_hard_error;
		}
	}

	/* TX/RX NORMAL interrupts */
	if (likely(intr_status & XGMAC_RI)) {
		u64_stats_update_begin(&stats->syncp);
		u64_stats_inc(&stats->rx_normal_irq_n[chan]);
		u64_stats_update_end(&stats->syncp);
		ret |= handle_rx;
	}
	if (likely(intr_status & (XGMAC_TI | XGMAC_TBU))) {
		u64_stats_update_begin(&stats->syncp);
		u64_stats_inc(&stats->tx_normal_irq_n[chan]);
		u64_stats_update_end(&stats->syncp);
		ret |= handle_tx;
	}

	/* Clear interrupts */
	writel(intr_en & intr_status, ioaddr + XGMAC_DMA_CH_STATUS(chan));

	return ret;
}

static int dwxgmac2_get_hw_feature(void __iomem *ioaddr,
				   struct dma_features *dma_cap)
{
	u32 hw_cap;

	/* MAC HW feature 0 */
	hw_cap = readl(ioaddr + XGMAC_HW_FEATURE0);
	dma_cap->edma = (hw_cap & XGMAC_HWFEAT_EDMA) >> 31;
	dma_cap->ediffc = (hw_cap & XGMAC_HWFEAT_EDIFFC) >> 30;
	dma_cap->vxn = (hw_cap & XGMAC_HWFEAT_VXN) >> 29;
	dma_cap->vlins = (hw_cap & XGMAC_HWFEAT_SAVLANINS) >> 27;
	dma_cap->tssrc = (hw_cap & XGMAC_HWFEAT_TSSTSSEL) >> 25;
	dma_cap->multi_addr = (hw_cap & XGMAC_HWFEAT_ADDMACADRSEL) >> 18;
	dma_cap->rx_coe = (hw_cap & XGMAC_HWFEAT_RXCOESEL) >> 16;
	dma_cap->tx_coe = (hw_cap & XGMAC_HWFEAT_TXCOESEL) >> 14;
	dma_cap->eee = (hw_cap & XGMAC_HWFEAT_EEESEL) >> 13;
	dma_cap->atime_stamp = (hw_cap & XGMAC_HWFEAT_TSSEL) >> 12;
	dma_cap->av = (hw_cap & XGMAC_HWFEAT_AVSEL) >> 11;
	dma_cap->av &= !((hw_cap & XGMAC_HWFEAT_RAVSEL) >> 10);
	dma_cap->arpoffsel = (hw_cap & XGMAC_HWFEAT_ARPOFFSEL) >> 9;
	dma_cap->rmon = (hw_cap & XGMAC_HWFEAT_MMCSEL) >> 8;
	dma_cap->pmt_magic_frame = (hw_cap & XGMAC_HWFEAT_MGKSEL) >> 7;
	dma_cap->pmt_remote_wake_up = (hw_cap & XGMAC_HWFEAT_RWKSEL) >> 6;
	dma_cap->sma_mdio = (hw_cap & XGMAC_HWFEAT_SMASEL) >> 5;
	dma_cap->vlhash = (hw_cap & XGMAC_HWFEAT_VLHASH) >> 4;
	dma_cap->half_duplex = (hw_cap & XGMAC_HWFEAT_HDSEL) >> 3;
	dma_cap->mbps_1000 = (hw_cap & XGMAC_HWFEAT_GMIISEL) >> 1;

	/* MAC HW feature 1 */
	hw_cap = readl(ioaddr + XGMAC_HW_FEATURE1);
	dma_cap->l3l4fnum = (hw_cap & XGMAC_HWFEAT_L3L4FNUM) >> 27;
	/* If L3L4FNUM < 8, then the number of L3L4 filters supported by
	 * XGMAC is equal to L3L4FNUM. From L3L4FNUM >= 8 the number of
	 * L3L4 filters goes on like 8, 16, 32, ... Current maximum of
	 * L3L4FNUM = 10.
	 */
	if (dma_cap->l3l4fnum >= 8 && dma_cap->l3l4fnum <= 10)
		dma_cap->l3l4fnum = 8 << (dma_cap->l3l4fnum - 8);
	else if (dma_cap->l3l4fnum > 10)
		dma_cap->l3l4fnum = 32;

	dma_cap->hash_tb_sz = (hw_cap & XGMAC_HWFEAT_HASHTBLSZ) >> 24;
	dma_cap->numtc = ((hw_cap & XGMAC_HWFEAT_NUMTC) >> 21) + 1;
	dma_cap->rssen = (hw_cap & XGMAC_HWFEAT_RSSEN) >> 20;
	dma_cap->dbgmem = (hw_cap & XGMAC_HWFEAT_DBGMEMA) >> 19;
	dma_cap->tsoen = (hw_cap & XGMAC_HWFEAT_TSOEN) >> 18;
	dma_cap->sphen = (hw_cap & XGMAC_HWFEAT_SPHEN) >> 17;
	dma_cap->dcben = (hw_cap & XGMAC_HWFEAT_DCBEN) >> 16;

	dma_cap->addr64 = (hw_cap & XGMAC_HWFEAT_ADDR64) >> 14;
	switch (dma_cap->addr64) {
	case 0:
		dma_cap->addr64 = 32;
		break;
	case 1:
		dma_cap->addr64 = 40;
		break;
	case 2:
		dma_cap->addr64 = 48;
		break;
	default:
		dma_cap->addr64 = 32;
		break;
	}

	dma_cap->advthword = (hw_cap & XGMAC_HWFEAT_ADVTHWORD) >> 13;
	dma_cap->ptoen = (hw_cap & XGMAC_HWFEAT_PTOEN) >> 12;
	dma_cap->osten = (hw_cap & XGMAC_HWFEAT_OSTEN) >> 11;
	dma_cap->tx_fifo_size =
		128 << ((hw_cap & XGMAC_HWFEAT_TXFIFOSIZE) >> 6);
	dma_cap->pfcen = (hw_cap & XGMAC_HWFEAT_PFCEN) >> 5;
	dma_cap->rx_fifo_size =
		128 << ((hw_cap & XGMAC_HWFEAT_RXFIFOSIZE) >> 0);

	/* MAC HW feature 2 */
	hw_cap = readl(ioaddr + XGMAC_HW_FEATURE2);
	dma_cap->aux_snapshot_n = (hw_cap & XGMAC_HWFEAT_AUXSNAPNUM) >> 28;
	dma_cap->pps_out_num = (hw_cap & XGMAC_HWFEAT_PPSOUTNUM) >> 24;
	dma_cap->number_tx_channel =
		((hw_cap & XGMAC_HWFEAT_TXCHCNT) >> 18) + 1;
	dma_cap->number_rx_channel =
		((hw_cap & XGMAC_HWFEAT_RXCHCNT) >> 12) + 1;
	dma_cap->number_tx_queues =
		((hw_cap & XGMAC_HWFEAT_TXQCNT) >> 6) + 1;
	dma_cap->number_rx_queues =
		((hw_cap & XGMAC_HWFEAT_RXQCNT) >> 0) + 1;

	/* MAC HW feature 3 */
	hw_cap = readl(ioaddr + XGMAC_HW_FEATURE3);
	dma_cap->tbs_ch_num = ((hw_cap & XGMAC_HWFEAT_TBSCH) >> 28) + 1;
	dma_cap->tbssel = (hw_cap & XGMAC_HWFEAT_TBSSEL) >> 27;
	dma_cap->fpesel = (hw_cap & XGMAC_HWFEAT_FPESEL) >> 26;
	dma_cap->sgfsel = (hw_cap & XGMAC_HWFEAT_SGFSEL) >> 25;
	dma_cap->estwid = (hw_cap & XGMAC_HWFEAT_ESTWID) >> 23;
	dma_cap->estdep = (hw_cap & XGMAC_HWFEAT_ESTDEP) >> 20;
	dma_cap->estsel = (hw_cap & XGMAC_HWFEAT_ESTSEL) >> 19;
	dma_cap->ttsfd = (hw_cap & XGMAC_HWFEAT_TTSFD) >> 16;
	dma_cap->asp = (hw_cap & XGMAC_HWFEAT_ASP) >> 14;
	dma_cap->dvlan = (hw_cap & XGMAC_HWFEAT_DVLAN) >> 13;
	dma_cap->frpes = (hw_cap & XGMAC_HWFEAT_FRPES) >> 11;
	dma_cap->frpbs = (hw_cap & XGMAC_HWFEAT_FRPPB) >> 9;
	dma_cap->pou_ost_en = (hw_cap & XGMAC_HWFEAT_POUOST) >> 8;
	dma_cap->frppipe_num = ((hw_cap & XGMAC_HWFEAT_FRPPIPE) >> 5) + 1;
	dma_cap->cbtisel = (hw_cap & XGMAC_HWFEAT_CBTISEL) >> 4;
	dma_cap->frpsel = (hw_cap & XGMAC_HWFEAT_FRPSEL) >> 3;
	dma_cap->nrvf_num = (hw_cap & XGMAC_HWFEAT_NRVF) >> 0;

	/* MAC HW feature 4 */
	hw_cap = readl(ioaddr + XGMAC_HW_FEATURE4);
	dma_cap->asp |= (hw_cap & XGMAC_HWFEAT_EASP) >> 2;
	dma_cap->pcsel = (hw_cap & XGMAC_HWFEAT_PCSEL) >> 0;

	return 0;
}

static void dwxgmac2_rx_watchdog(struct stmmac_priv *priv, void __iomem *ioaddr,
				 u32 riwt, u32 queue)
{
	writel(riwt & XGMAC_RWT, ioaddr + XGMAC_DMA_CH_Rx_WATCHDOG(queue));
}

static void dwxgmac2_set_rx_ring_len(struct stmmac_priv *priv,
				     void __iomem *ioaddr, u32 len, u32 chan)
{
	writel(len, ioaddr + XGMAC_DMA_CH_RxDESC_RING_LEN(chan));
}

static void dwxgmac2_set_tx_ring_len(struct stmmac_priv *priv,
				     void __iomem *ioaddr, u32 len, u32 chan)
{
	writel(len, ioaddr + XGMAC_DMA_CH_TxDESC_RING_LEN(chan));
}

static void dwxgmac2_set_rx_tail_ptr(struct stmmac_priv *priv,
				     void __iomem *ioaddr, u32 ptr, u32 chan)
{
	writel(ptr, ioaddr + XGMAC_DMA_CH_RxDESC_TAIL_LPTR(chan));
}

static void dwxgmac2_set_tx_tail_ptr(struct stmmac_priv *priv,
				     void __iomem *ioaddr, u32 ptr, u32 chan)
{
	writel(ptr, ioaddr + XGMAC_DMA_CH_TxDESC_TAIL_LPTR(chan));
}

static void dwxgmac2_enable_tso(struct stmmac_priv *priv, void __iomem *ioaddr,
				bool en, u32 chan)
{
	u32 value = readl(ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan));

	if (en)
		value |= XGMAC_TSE;
	else
		value &= ~XGMAC_TSE;

	writel(value, ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan));
}

static void dwxgmac2_qmode(struct stmmac_priv *priv, void __iomem *ioaddr,
			   u32 channel, u8 qmode)
{
	u32 value = readl(ioaddr + XGMAC_MTL_TXQ_OPMODE(channel));
	u32 flow = readl(ioaddr + XGMAC_RX_FLOW_CTRL);

	value &= ~XGMAC_TXQEN;
	if (qmode != MTL_QUEUE_AVB) {
		value |= 0x2 << XGMAC_TXQEN_SHIFT;
		writel(0, ioaddr + XGMAC_MTL_TCx_ETS_CONTROL(channel));
	} else {
		value |= 0x1 << XGMAC_TXQEN_SHIFT;
		writel(flow & (~XGMAC_RFE), ioaddr + XGMAC_RX_FLOW_CTRL);
	}

	writel(value, ioaddr +  XGMAC_MTL_TXQ_OPMODE(channel));
}

static void dwxgmac2_set_bfsize(struct stmmac_priv *priv, void __iomem *ioaddr,
				int bfsize, u32 chan)
{
	u32 value;

	value = readl(ioaddr + XGMAC_DMA_CH_RX_CONTROL(chan));
	value &= ~XGMAC_RBSZ;
	value |= bfsize << XGMAC_RBSZ_SHIFT;
	writel(value, ioaddr + XGMAC_DMA_CH_RX_CONTROL(chan));
}

static void dwxgmac2_enable_sph(struct stmmac_priv *priv, void __iomem *ioaddr,
				bool en, u32 chan)
{
	u32 value = readl(ioaddr + XGMAC_RX_CONFIG);

	value &= ~XGMAC_CONFIG_HDSMS;
	value |= XGMAC_CONFIG_HDSMS_256; /* Segment max 256 bytes */
	writel(value, ioaddr + XGMAC_RX_CONFIG);

	value = readl(ioaddr + XGMAC_DMA_CH_CONTROL(chan));
	if (en)
		value |= XGMAC_SPH;
	else
		value &= ~XGMAC_SPH;
	writel(value, ioaddr + XGMAC_DMA_CH_CONTROL(chan));
}

static int dwxgmac2_enable_tbs(struct stmmac_priv *priv, void __iomem *ioaddr,
			       bool en, u32 chan)
{
	u32 value = readl(ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan));

	if (en)
		value |= XGMAC_EDSE;
	else
		value &= ~XGMAC_EDSE;

	writel(value, ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan));

	value = readl(ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan)) & XGMAC_EDSE;
	if (en && !value)
		return -EIO;

	writel(XGMAC_DEF_FTOS, ioaddr + XGMAC_DMA_TBS_CTRL0);
	writel(XGMAC_DEF_FTOS, ioaddr + XGMAC_DMA_TBS_CTRL1);
	writel(XGMAC_DEF_FTOS, ioaddr + XGMAC_DMA_TBS_CTRL2);
	writel(XGMAC_DEF_FTOS, ioaddr + XGMAC_DMA_TBS_CTRL3);
	return 0;
}

const struct stmmac_dma_ops dwxgmac210_dma_ops = {
	.reset = dwxgmac2_dma_reset,
	.init = dwxgmac2_dma_init,
	.init_chan = dwxgmac2_dma_init_chan,
	.init_rx_chan = dwxgmac2_dma_init_rx_chan,
	.init_tx_chan = dwxgmac2_dma_init_tx_chan,
	.axi = dwxgmac2_dma_axi,
	.dump_regs = dwxgmac2_dma_dump_regs,
	.dma_rx_mode = dwxgmac2_dma_rx_mode,
	.dma_tx_mode = dwxgmac2_dma_tx_mode,
	.enable_dma_irq = dwxgmac2_enable_dma_irq,
	.disable_dma_irq = dwxgmac2_disable_dma_irq,
	.start_tx = dwxgmac2_dma_start_tx,
	.stop_tx = dwxgmac2_dma_stop_tx,
	.start_rx = dwxgmac2_dma_start_rx,
	.stop_rx = dwxgmac2_dma_stop_rx,
	.dma_interrupt = dwxgmac2_dma_interrupt,
	.get_hw_feature = dwxgmac2_get_hw_feature,
	.rx_watchdog = dwxgmac2_rx_watchdog,
	.set_rx_ring_len = dwxgmac2_set_rx_ring_len,
	.set_tx_ring_len = dwxgmac2_set_tx_ring_len,
	.set_rx_tail_ptr = dwxgmac2_set_rx_tail_ptr,
	.set_tx_tail_ptr = dwxgmac2_set_tx_tail_ptr,
	.enable_tso = dwxgmac2_enable_tso,
	.qmode = dwxgmac2_qmode,
	.set_bfsize = dwxgmac2_set_bfsize,
	.enable_sph = dwxgmac2_enable_sph,
	.enable_tbs = dwxgmac2_enable_tbs,
};
