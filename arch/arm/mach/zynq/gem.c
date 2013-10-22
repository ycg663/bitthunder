#include <bitthunder.h>
#include <asm/barrier.h>
#include <string.h>
#include "gem.h"
#include "slcr.h"

BT_DEF_MODULE_NAME					("zynq-gem")
BT_DEF_MODULE_DESCRIPTION			("Zynq Gigabit Ethernet Mac driver")
BT_DEF_MODULE_AUTHOR				("James Walmsley")
BT_DEF_MODULE_EMAIL					("jwalmsley@riegl.com")

#define SEND_BD_CNT					256
#define RECV_BD_CNT					256

#define RX_BUF_SIZE					1536
#define TX_BUF_SIZE					1536

enum RX_STATE {
	RX_STATE_READY,
	RX_STATE_SIGNALLED,
	RX_STATE_COPY,
};

enum TX_STATE {
	TX_STATE_READY,
	TX_STATE_SOF,
	TX_STATE_PAYLOAD,
	TX_STATE_DONE,
};

struct _BT_OPAQUE_HANDLE {
	BT_HANDLE_HEADER 				h;
	volatile GEM_REGS			   *pRegs;
	const BT_INTEGRATED_DEVICE 	   *pDevice;
	BT_NET_IF					   *pIf;
	BT_NET_IF_EVENTRECEIVER			pfnEvent;
	BT_HANDLE 						hMII;
	bt_paddr_t						rxbd_phys;
	GEM_BD						   *rxbd_cached;
	volatile GEM_BD				   *rxbd;
	BT_u32							rx_ci;
	enum RX_STATE					rx_state;
	BT_u32							rx_bytes;
	bt_paddr_t						rxbufs_phys;
	void						   *rxbufs_cached;
	void 						   *rxbufs;
	bt_paddr_t						txbd_phys;
	GEM_BD						   *txbd_cached;
	volatile GEM_BD				   *txbd;
	BT_u32							tx_ci;
	BT_u32							tx_bytes;
	bt_paddr_t 						txbufs_phys;
	void 						   *txbufs_cached;
	void						   *txbufs;
};

struct _MII_HANDLE {
	BT_HANDLE_HEADER		h;
	struct bt_mii_bus		mii;
};

static const BT_IF_HANDLE oMIIHandleInterface;


static BT_ERROR gem_interrupt_handler(BT_u32 ulIRQ, void *pParam) {
	BT_HANDLE hMac = (BT_HANDLE) pParam;

	BT_u32 regisr = hMac->pRegs->intr_status;
	hMac->pRegs->intr_status = regisr;
	while(regisr) {
		if(regisr & GEM_INT_RX_COMPLETE) {
			hMac->pfnEvent(hMac->pIf, BT_NET_IF_RX_READY, BT_TRUE);
			hMac->pRegs->rx_status = RX_STAT_FRAME_RECD;
		}

		if(regisr & GEM_INT_TX_COMPLETE) {
			hMac->pRegs->tx_status = 0x20;
		}

		if(regisr & GEM_INT_RX_OVERRUN) {
			BT_kPrint("rx overrun");
		}

		regisr = hMac->pRegs->intr_status;
		hMac->pRegs->intr_status = regisr;
	}

	return BT_ERR_NONE;
}

static BT_ERROR mac_cleanup(BT_HANDLE hMac) {
	return BT_ERR_UNIMPLEMENTED;
}

static BT_ERROR mac_eventsubscribe(BT_HANDLE hMac, BT_NET_IF_EVENTRECEIVER pfnReceiver, BT_NET_IF *pIf) {
	hMac->pIf = pIf;
	hMac->pfnEvent = pfnReceiver;
	return BT_ERR_NONE;
}

static BT_ERROR mac_getaddr(BT_HANDLE hMac, BT_u8 *addr, BT_u32 ulLength) {
	volatile GEM_REGS *pRegs = hMac->pRegs;

	BT_u32 regvall, regvalh;

	regvall = pRegs->spec_addr1_bot;
    regvalh = pRegs->spec_addr1_top;

    BT_kPrint("MAC 0x%08x, 0x%08x, %02x:%02x:%02x:%02x:%02x:%02x\n",
			regvall, regvalh,
			(regvall & 0xff), ((regvall >> 8) & 0xff),
			((regvall >> 16) & 0xff), (regvall >> 24),
			(regvalh & 0xff), (regvalh >> 8));


	addr[0] = (regvall & 0xff);
	addr[1] = ((regvall >>  8) & 0xff);
	addr[2] = ((regvall >> 16) & 0xff);
	addr[3] = ((regvall >> 24) & 0xff);
	addr[4] = (regvalh & 0xff);
	addr[5] = ((regvalh >> 8) & 0xff);

	return BT_ERR_NONE;
}

static BT_ERROR mac_setaddr(BT_HANDLE hMac, BT_u8 *addr, BT_u32 ulLength) {
	volatile GEM_REGS *pRegs = hMac->pRegs;

	pRegs->spec_addr1_bot = addr[3] << 24 | addr[2] << 16 | addr[1] << 8 | addr[0];
	dsb();
	pRegs->spec_addr1_top = addr[5] << 8 | addr[4];

	return BT_ERR_NONE;
}

static BT_u32 mac_getmtusize(BT_HANDLE hMac, BT_ERROR *pError) {
	return RX_BUF_SIZE;
}


static BT_u32 mac_dataready(BT_HANDLE hMac, BT_ERROR *pError) {
	volatile GEM_BD *cur = &hMac->rxbd[hMac->rx_ci];
	if(cur->address & RX_BD_OWNERSHIP) {
		hMac->rx_state = RX_STATE_SIGNALLED;
		hMac->rx_bytes = cur->flags & RX_BD_LENGTH;
		return hMac->rx_bytes;
	}

	return 0;
}

static BT_ERROR mac_read(BT_HANDLE hMac, BT_u32 ulSize, BT_u32 pos, void *pBuffer) {
	BT_u8 *p = (BT_u8 *) hMac->rxbufs + (RX_BUF_SIZE * hMac->rx_ci);
	memcpy(pBuffer, p + pos, ulSize);
	hMac->rx_bytes -= ulSize;
	hMac->rx_state = RX_STATE_COPY;

	if(!hMac->rx_bytes) {
		volatile GEM_BD *cur = &hMac->rxbd[hMac->rx_ci];
		hMac->rx_ci += 1;
		if(hMac->rx_ci == RECV_BD_CNT) {
			hMac->rx_ci = 0;
		}
		hMac->rx_state = RX_STATE_READY;
		cur->address &= ~RX_BD_OWNERSHIP;
	}

	return BT_ERR_NONE;
}

static BT_ERROR mac_write(BT_HANDLE hMac, BT_u32 ulSize, void *pBuffer) {
	BT_u8 *p = (BT_u8 *) hMac->txbufs + (TX_BUF_SIZE * hMac->tx_ci);
	memcpy(p+hMac->tx_bytes, pBuffer, ulSize);
	hMac->tx_bytes += ulSize;
	return BT_ERR_NONE;
}

static BT_ERROR mac_drop(BT_HANDLE hMac, BT_u32 ulSize) {
	return BT_ERR_NONE;
}

static BT_BOOL mac_tx_ready(BT_HANDLE hMac, BT_ERROR *pError) {
	volatile GEM_BD *cur = &hMac->txbd[hMac->tx_ci];
	if(cur->flags & TX_BD_USED) {
		cur->flags &= (TX_BD_USED | TX_BD_WRAP);

		hMac->pRegs->tx_status = hMac->pRegs->tx_status;

		return BT_TRUE;
	}

	return BT_FALSE;
}

static BT_ERROR mac_sendframe(BT_HANDLE hMac) {
	volatile GEM_BD *cur = &hMac->txbd[hMac->tx_ci];
	cur->flags &= (TX_BD_USED | TX_BD_WRAP);
	cur->flags |= TX_BD_LAST;
	cur->flags |= (hMac->tx_bytes & TX_BD_LENGTH);
	hMac->tx_bytes = 0;

	dsb();

	cur->flags &= ~TX_BD_USED;
	hMac->tx_ci += 1;

	if(hMac->tx_ci == SEND_BD_CNT) {
		hMac->tx_ci = 0;
	}

	dsb();

	hMac->pRegs->net_ctrl |= NET_CTRL_STARTTX;

	dsb();

	return BT_ERR_NONE;
}

static BT_ERROR mac_send_event(BT_HANDLE hMac, BT_u32 ulEvent) {
	return BT_ERR_NONE;
}

static struct _MII_HANDLE* mii_init(BT_HANDLE hMac, BT_ERROR *pError) {
	struct _MII_HANDLE *pMII = (struct _MII_HANDLE *) BT_CreateHandle(&oMIIHandleInterface, sizeof(struct _MII_HANDLE), pError);
	if(!pMII) {
		if(pError) {
			*pError = BT_ERR_NO_MEMORY;
		}
		return NULL;
	}

hMac->hMII = (BT_HANDLE) pMII;

	pMII->mii.hMAC = hMac;
	pMII->mii.name = "zynq gem-mii bus";

	return pMII;
}


static BT_ERROR mac_init(BT_HANDLE hMac) {
	BT_ERROR Error = BT_ERR_NONE;
	struct _MII_HANDLE *pMII = mii_init(hMac, &Error);
	if(!pMII) {
		return Error;
	}

	Error = BT_RegisterMiiBus((BT_HANDLE) pMII, &pMII->mii);
	if(Error) {
		return Error;
	}

	hMac->pRegs->net_ctrl |= NET_CTRL_TXEN;
	hMac->pRegs->net_ctrl |= NET_CTRL_RXEN;

	hMac->pRegs->intr_enable = GEM_INT_ALL_MASK;

	return BT_ERR_NONE;
}

static BT_u16 mii_read(BT_HANDLE hMII, BT_u32 phy_id, BT_u32 regnum, BT_ERROR *pError) {

	struct _MII_HANDLE *pMII = (struct _MII_HANDLE *) hMII;
	while(!(pMII->mii.hMAC->pRegs->net_status & NET_STATUS_MGMT_IDLE)) {
		BT_ThreadYield();
	}

	BT_u32 reg = (phy_id << 23) & PHY_MAINT_PHY_ADDR;
	reg |= (regnum << 18) & PHY_MAINT_REG_ADDR;
	reg |= PHY_MAINT_R_MASK;
	reg |= 0x40020000;

	pMII->mii.hMAC->pRegs->phy_maint = reg;

	while(!(pMII->mii.hMAC->pRegs->net_status & NET_STATUS_MGMT_IDLE)) {
		BT_ThreadYield();
	}

	return pMII->mii.hMAC->pRegs->phy_maint & PHY_MAINT_DATA;
}

static BT_ERROR mii_write(BT_HANDLE hMII, BT_u32 phy_id, BT_u32 regnum, BT_u16 val) {

	struct _MII_HANDLE *pMII = (struct _MII_HANDLE *) hMII;
	while(!(pMII->mii.hMAC->pRegs->net_status & NET_STATUS_MGMT_IDLE)) {
		BT_ThreadYield();
	}

	BT_u32 reg = (phy_id << 23) & PHY_MAINT_PHY_ADDR;
	reg |= (regnum << 18) & PHY_MAINT_REG_ADDR;
	reg |= PHY_MAINT_W_MASK;
	reg |= 0x40020000;
	reg |= val;

	pMII->mii.hMAC->pRegs->phy_maint = reg;

	while(!(pMII->mii.hMAC->pRegs->net_status & NET_STATUS_MGMT_IDLE)) {
		BT_ThreadYield();
	}

	return BT_ERR_NONE;
}

static BT_ERROR mii_reset(BT_HANDLE hMII) {
	return BT_ERR_NONE;
}

static BT_ERROR mii_cleanup(BT_HANDLE hMII) {
	return BT_ERR_NONE;
}

static const BT_DEV_IF_MII mii_ops = {
	.pfnRead 	= mii_read,
	.pfnWrite 	= mii_write,
	.pfnReset 	= mii_reset,
};

static const BT_IF_DEVICE oMIIDeviceIF = {
	.eConfigType	= BT_DEV_IF_T_MII,
	.unConfigIfs 	= {
		.pMiiIF = &mii_ops,
	},
};

static const BT_IF_HANDLE oMIIHandleInterface = {
	BT_MODULE_DEF_INFO,
	.oIfs = {
		.pDevIF = &oMIIDeviceIF,
	},
	.eType = BT_HANDLE_T_DEVICE,
	.pfnCleanup = mii_cleanup,
};

static const BT_DEV_IF_EMAC mac_ops = {
	.ulCapabilities 	= BT_NET_IF_CAPABILITIES_ETHERNET | BT_NET_IF_CAPABILITIES_100MBPS | BT_NET_IF_CAPABILITIES_1000MBPS,
	.pfnEventSubscribe 	= mac_eventsubscribe,
	.pfnInitialise		= mac_init,
	.pfnGetMACAddr		= mac_getaddr,
	.pfnSetMACAddr		= mac_setaddr,
	.pfnGetMTU			= mac_getmtusize,
	.pfnDataReady		= mac_dataready,
	.pfnRead			= mac_read,
	.pfnWrite			= mac_write,
	.pfnDropFrame		= mac_drop,
	.pfnTxFifoReady		= mac_tx_ready,
	.pfnSendFrame		= mac_sendframe,
	.pfnSendEvent		= mac_send_event,
};

/*static const BT_IF_POWER oPowerInterface = {
	.pfnSetPowerState = mac_set_power_state,
	.pfnGetPowerState = mac_get_power_state,
	};*/

static const BT_IF_DEVICE oDeviceIF = {
	//.pPowerIF 		= &oPowerInterface,
	.eConfigType	= BT_DEV_IF_T_EMAC,
	.unConfigIfs 	= {
		.pEMacIF = &mac_ops,
	},
};

static const BT_IF_HANDLE oHandleInterface = {
	BT_MODULE_DEF_INFO,
	.oIfs = {
		.pDevIF = &oDeviceIF,
	},
	.eType = BT_HANDLE_T_DEVICE,
	.pfnCleanup = mac_cleanup,
};

BT_u8 dividers[8] = {
	8,
	16,
	32,
	48,
	64,
	96,
	128,
	224
};

static BT_ERROR descriptor_init(BT_HANDLE hMac) {
	// Allocate coherent memory for buffers.
	hMac->rxbufs_phys = bt_page_alloc(RX_BUF_SIZE * RECV_BD_CNT);
	hMac->rxbufs_cached = (void *) bt_phys_to_virt(hMac->rxbufs_phys);
	hMac->rxbufs = bt_ioremap((void *) hMac->rxbufs_phys, RX_BUF_SIZE * RECV_BD_CNT);	// Get an uncached view on the buffer memory.

    memset(hMac->rxbufs_cached, 0, RX_BUF_SIZE * RECV_BD_CNT);

	hMac->rxbd_phys = bt_page_alloc(sizeof(GEM_BD) * RECV_BD_CNT);
	hMac->rxbd_cached = (void *) bt_phys_to_virt(hMac->rxbd_phys);
	hMac->rxbd = bt_ioremap((void *) hMac->rxbd_phys, sizeof(GEM_BD) * RECV_BD_CNT);		// Get an uncached view on the buffer descriptor memory.

	//BT_DCacheInvalidateLine((void *) hMac->rxbd_cached);

	memset((void *) hMac->rxbd, 0, sizeof(GEM_BD) * RECV_BD_CNT);

	BT_u32 i;
	for(i = 0; i < RECV_BD_CNT; i++) {
		hMac->rxbd_cached[i].address = (hMac->rxbufs_phys + (RX_BUF_SIZE * i));
	}

	// Set last bd to wrap!
	hMac->rxbd_cached[RECV_BD_CNT-1].address |= RX_BD_WRAP;

	// Allocate coherent memory for buffers.
	hMac->txbufs_phys = bt_page_alloc(TX_BUF_SIZE * SEND_BD_CNT);
	hMac->txbufs_cached = (void *) bt_phys_to_virt(hMac->txbufs_phys);
	hMac->txbufs = bt_ioremap((void *) hMac->txbufs_phys, TX_BUF_SIZE * SEND_BD_CNT);	// Get an uncached view on the buffer memory.

    memset(hMac->txbufs_cached, 0, TX_BUF_SIZE * SEND_BD_CNT);

	hMac->txbd_phys 	= bt_page_alloc(sizeof(GEM_BD) * SEND_BD_CNT);
	hMac->txbd_cached	= (void *) bt_phys_to_virt(hMac->txbd_phys);
	hMac->txbd 			= bt_ioremap((void *) hMac->txbd_phys, sizeof(GEM_BD) * SEND_BD_CNT);

	//BT_DCacheInvalidateRange((void *) hMac->txbd_cached, (sizeof(GEM_BD) * SEND_BD_CNT));

	memset((void *) hMac->txbd, 0, sizeof(GEM_BD) * SEND_BD_CNT);

	for(i = 0; i < SEND_BD_CNT; i++) {
		hMac->txbd_cached[i].address 	= (hMac->txbufs_phys + (TX_BUF_SIZE * i));
		hMac->txbd_cached[i].flags 	= TX_BD_USED;
	}

	hMac->txbd_cached[SEND_BD_CNT-1].flags |= TX_BD_WRAP;

	BT_DCacheFlush();

	BT_kPrint("GEM: TXBD: %08x (%08x)", hMac->txbd_phys, hMac->txbd);
	BT_kPrint("GEM: RXBD: %08x (%08x)", hMac->rxbd_phys, hMac->rxbd);

	return BT_ERR_NONE;
}


/*static int descriptor_free(BT_HANDLE hMac) {


	return BT_ERR_NONE;
	}*/

static void mac_set_hwaddr(BT_HANDLE hMac) {
	// Set mac address
	// Get mac address from device tree, or bootloader params.
	BT_u8 ucMac[6] = {0x00, 0x50, 0x45, 0x67, 0x89, 0x00};
	mac_setaddr(hMac, ucMac, 6);
}

static void mac_reset_hw(BT_HANDLE hMac) {
	volatile GEM_REGS *pRegs = hMac->pRegs;
	// Initialise the MAC
	pRegs->net_ctrl = 0;

	pRegs->net_ctrl |= NET_CTRL_STATCLR;
	//pRegs->net_ctrl |= NET_CTRL_LOOPEN;

	// Initialise the MAC
	pRegs->rx_status = 0x0F;
	pRegs->tx_status = 0xFF;

	pRegs->intr_disable = 0x07FFFEFF;

	// Clear the buffer queues.
	pRegs->rx_qbar = 0;
	pRegs->tx_qbar = 0;
}

static void mac_init_hw(BT_HANDLE hMac) {

	volatile GEM_REGS *pRegs = hMac->pRegs;

	mac_reset_hw(hMac);
	mac_set_hwaddr(hMac);

	BT_u32 regval = 0;
	regval |= NET_CFG_FDEN;
	regval |= NET_CFG_RXCHKSUMEN;
	regval |= NET_CFG_PAUSECOPYDI;
	regval |= NET_CFG_FCSREM;
	regval |= NET_CFG_PAUSEEN;
	regval |= NET_CFG_SPEED;
	regval |= NET_CFG_HDRXEN;
	regval |= NET_CFG_MCASTHASHEN;

	// Set the MDC clock divisor.
	BT_u32 cpu1x_freq = BT_ZYNQ_GetCpu1xFrequency();
	BT_u32 div = 0;
	for(div = 0; div < 8; div++) {
		if((cpu1x_freq / dividers[div]) < 2500000) {
			break;
		}
	}

	volatile ZYNQ_SLCR_REGS *pSLCR = bt_ioremap((void *) ZYNQ_SLCR, BT_SIZE_4K);
	BT_u32 InputClk;

	zynq_slcr_unlock(pSLCR);
	BT_u32 clk_sel = ZYNQ_SLCR_CLK_CTRL_SRCSEL_VAL(pSLCR->GEM0_CLK_CTRL);
	switch(clk_sel) {
	case ZYNQ_SLCR_CLK_CTRL_SRCSEL_ARMPLL:
		InputClk = BT_ZYNQ_GetArmPLLFrequency();
		break;

	case ZYNQ_SLCR_CLK_CTRL_SRCSEL_IOPLL:
		InputClk = BT_ZYNQ_GetIOPLLFrequency();
		break;

	case ZYNQ_SLCR_CLK_CTRL_SRCSEL_DDRPLL:
		InputClk = BT_ZYNQ_GetDDRPLLFrequency();
		break;

	default:
		break;
	}

	BT_DIVIDER_PARAMS oDiv;
	oDiv.diva_max = 64;
	oDiv.diva_min = 1;
	oDiv.divb_max = 64;
	oDiv.divb_min = 1;

	BT_CalculateClockDivider(InputClk, 25000000, &oDiv);

	pSLCR->GEM0_CLK_CTRL &= ~0x03F03F00;
	pSLCR->GEM0_CLK_CTRL |=  ((oDiv.diva_val << 8) & 0x3f00) | ((oDiv.divb_val << 20) & 0x03f00000);

	zynq_slcr_lock(pSLCR);

	bt_iounmap(pSLCR);

	regval |= (7 << 18) & NET_CFG_MDCCLKDIV;

	pRegs->intr_status = 0xFFFFFFFF;

	pRegs->net_cfg = regval;

	pRegs->hash_bot = 0;
	pRegs->hash_top = 0;

	// Init TX and RX dma Q addresses
	pRegs->rx_qbar = hMac->rxbd_phys;
	pRegs->tx_qbar = hMac->txbd_phys;

	pRegs->dma_cfg = 0;

	pRegs->dma_cfg &= ~DMA_CFG_RXBUF;
	pRegs->dma_cfg |= (0x18 << 16) & DMA_CFG_RXBUF;

	pRegs->dma_cfg &= ~DMA_CFG_RXSIZE;
	pRegs->dma_cfg |= (0x3 << 8) & DMA_CFG_RXSIZE;

	pRegs->dma_cfg &= ~DMA_CFG_TXSIZE;
	pRegs->dma_cfg |= DMA_CFG_TXSIZE;

	pRegs->dma_cfg |= DMA_CFG_TCPCKSUM;
	pRegs->dma_cfg &= ~DMA_CFG_AHB_ENDIAN_PKT_SWP;

	pRegs->dma_cfg &= ~DMA_CFG_BLENGTH;
	pRegs->dma_cfg |= (0x10 & DMA_CFG_BLENGTH);

	pRegs->net_ctrl |= NET_CTRL_MDEN;
}

static BT_HANDLE mac_probe(const BT_INTEGRATED_DEVICE *pDevice, BT_ERROR *pError) {

	BT_HANDLE hMac;
	BT_ERROR Error = BT_ERR_NONE;

	hMac = BT_CreateHandle(&oHandleInterface, sizeof(struct _BT_OPAQUE_HANDLE), pError);
	if(!hMac) {
		Error = BT_ERR_NO_MEMORY;
		goto err_out;
	}

	const BT_RESOURCE *pResource = BT_GetIntegratedResource(pDevice, BT_RESOURCE_MEM, 0);
	if(!pResource) {
		Error = BT_ERR_GENERIC;
		goto err_free_handle_out;
	}

	hMac->pRegs = bt_ioremap((void *) pResource->ulStart, sizeof(GEM_REGS));

	descriptor_init(hMac);

	mac_init_hw(hMac);

	pResource = BT_GetIntegratedResource(pDevice, BT_RESOURCE_IRQ, 0);
	if(!pResource) {
		Error = BT_ERR_GENERIC;
		goto err_free_buffers_out;
	}

	Error = BT_RegisterInterrupt(pResource->ulStart, gem_interrupt_handler, hMac);
	if(Error) {
		Error = BT_ERR_GENERIC;
		goto err_free_buffers_out;
	}

	BT_EnableInterrupt(pResource->ulStart);

	BT_RegisterNetworkInterface(hMac);

	return hMac;

err_free_buffers_out:

err_free_handle_out:
	BT_DestroyHandle(hMac);

err_out:
	if(pError) {
		*pError = Error;
	}

	return NULL;
}

BT_INTEGRATED_DRIVER_DEF mac_driver = {
	.name = "zynq,gem",
	.pfnProbe = mac_probe,
};

#ifdef BT_CONFIG_MACH_ZYNQ_GEM_0
static const BT_RESOURCE oZynq_mac0_resources[] = {
	{
		.ulStart 	= 0xE000B000,
		.ulEnd  	= 0xE000B000 + BT_SIZE_4K -1,
		.ulFlags	= BT_RESOURCE_MEM,
	},
	{
		.ulStart 	= 0,
		.ulEnd		= 0,
		.ulFlags	= BT_RESOURCE_ENUM,
	},
	{
		.ulStart	= 54,	// Ethernet 0 IRQ
		.ulEnd		= 55,	// Ethernet 0 Wakeup IRQ
		.ulFlags	= BT_RESOURCE_IRQ,
	},
};

BT_INTEGRATED_DEVICE_DEF oZynq_mac0_device = {
	.name				= "zynq,gem",
	.ulTotalResources	= BT_ARRAY_SIZE(oZynq_mac0_resources),
	.pResources			= oZynq_mac0_resources,
};
#endif

#ifdef BT_CONFIG_MACH_ZYNQ_GEM_1
static const BT_RESOURCE oZynq_mac1_resources[] = {
	{
		.ulStart 	= 0xE000C000,
		.ulEnd  	= 0xE000C000 + BT_SIZE_4K -1,
		.ulFlags	= BT_RESOURCE_MEM,
	},
	{
		.ulStart 	= 1,
		.ulEnd		= 1,
		.ulFlags	= BT_RESOURCE_ENUM,
	},
	{
		.ulStart	= 77,	// Ethernet 0 IRQ
		.ulEnd		= 78,	// Ethernet 0 Wakeup IRQ
		.ulFlags	= BT_RESOURCE_IRQ,
	},
};

BT_INTEGRATED_DEVICE_DEF oZynq_mac1_device = {
	.name				= "zynq,gem",
	.ulTotalResources	= BT_ARRAY_SIZE(oZynq_mac1_resources),
	.pResources			= oZynq_mac1_resources,
};
#endif