#include <console.h>
#include <debug.h>
#include <errno.h>
#include <mmio.h>
#include <string.h>
#include <sp804_timer.h>
#include <dw_mmc.h>
#include <hi6220.h>
#include <hi6553.h>

#define MMC_PLL			100000000

static void init_mmc_pll(void)
{
	unsigned int data;

	NOTICE("#%s, %d\n", __func__, __LINE__);
	data = hi6553_read_8(LDO19_REG_ADJ);
	NOTICE("#%s, %d\n", __func__, __LINE__);
	data |= 0x7;		/* 3.0V */
	hi6553_write_8(LDO19_REG_ADJ, data);
	NOTICE("#%s, %d\n", __func__, __LINE__);
	/* select syspll as mmc clock */
	mmio_write_32(PERI_SC_CLK_SEL0, 1 << 5 | 1 << 21);
	/* enable mmc0 clock */
	NOTICE("#%s, %d\n", __func__, __LINE__);
	mmio_write_32(PERI_SC_PERIPH_CLKEN0, PERI_CLK_MMC0);
	do {
		data = mmio_read_32(PERI_SC_PERIPH_CLKSTAT0);
	} while (!(data & PERI_CLK_MMC0));
	/* enable source clock to mmc0 */
	data = mmio_read_32(PERI_SC_PERIPH_CLKEN12);
	data |= 1 << 1;
	mmio_write_32(PERI_SC_PERIPH_CLKEN12, data);
	NOTICE("#%s, %d\n", __func__, __LINE__);
	/* scale mmc frequency to 100MHz (divider as 12 since PLL is 1.2GHz */
	mmio_write_32(PERI_SC_CLKCFG8BIT1, (1 << 7) | 0xb);
	NOTICE("#%s, %d\n", __func__, __LINE__);
}

static void reset_mmc0_clk(void)
{
	unsigned int data;

	/* disable mmc0 bus clock */
	mmio_write_32(PERI_SC_PERIPH_CLKDIS0, PERI_CLK_MMC0);
	do {
		data = mmio_read_32(PERI_SC_PERIPH_CLKSTAT0);
	} while (data & PERI_CLK_MMC0);
	/* enable mmc0 bus clock */
	mmio_write_32(PERI_SC_PERIPH_CLKEN0, PERI_CLK_MMC0);
	do {
		data = mmio_read_32(PERI_SC_PERIPH_CLKSTAT0);
	} while (!(data & PERI_CLK_MMC0));
	/* reset mmc0 clock domain */
	mmio_write_32(PERI_SC_PERIPH_RSTEN0, PERI_CLK_MMC0);

	/* bypass mmc0 clock phase */
	data = mmio_read_32(PERI_SC_PERIPH_CTRL2);
	data |= 3;
	mmio_write_32(PERI_SC_PERIPH_CTRL2, data);

	/* disable low power */
	data = mmio_read_32(PERI_SC_PERIPH_CTRL13);
	data |= 1 << 3;
	mmio_write_32(PERI_SC_PERIPH_CTRL13, data);
	do {
		data = mmio_read_32(PERI_SC_PERIPH_RSTSTAT0);
	} while (!(data & (1 << 0)));

	/* unreset mmc0 clock domain */
	mmio_write_32(PERI_SC_PERIPH_RSTDIS0, 1 << 0);
	do {
		data = mmio_read_32(PERI_SC_PERIPH_RSTSTAT0);
	} while (data & (1 << 0));
}

static int update_mmc0_clock(void)
{
	unsigned int data;

	/* CMD_UPDATE_CLK */
	data = BIT_CMD_WAIT_PRVDATA_COMPLETE | BIT_CMD_UPDATE_CLOCK_ONLY |
		BIT_CMD_START;
	mmio_write_32(MMC0_CMD, data);
	while (1) {
		data = mmio_read_32(MMC0_CMD);
		if (!(data & CMD_START_BIT))
			break;
		data = mmio_read_32(MMC0_RINTSTS);
		if (data & MMC_INT_HLE) {
			NOTICE("fail to update mmc clock frequency\n");
			return -EINVAL;
		}
	}
	return 0;
}

static int set_mmc0_clock(int rate)
{
	int ret, divider, found = 0;
	unsigned int data;

	for (divider = 1; divider < 256; divider++) {
		if ((MMC_PLL / (2 * divider)) <= rate) {
			found = 1;
			break;
		}
	}
	if (!found)
		return -EINVAL;
	NOTICE("#%s, %d, divider:%d\n", __func__, __LINE__, divider);

	do {
		data = mmio_read_32(MMC0_STATUS);
	} while (data & MMC_STS_DATA_BUSY);

	/* Disable mmc clock first */
	mmio_write_32(MMC0_CLKENA, 0);
	do {
		ret = update_mmc0_clock();
	} while (ret);

	do {
		mmio_write_32(MMC0_CLKDIV, divider);
		ret = update_mmc0_clock();
	} while (ret);

	/* enable mmc clock */
	do {
		mmio_write_32(MMC0_CLKENA, 1);
		mmio_write_32(MMC0_CLKSRC, 0);
		ret = update_mmc0_clock();
	} while (ret);
	return 0;
}

static void set_mmc0_io(void)
{
	mmio_write_32(MMC0_CTYPE, MMC_8BIT_MODE);
	mmio_write_32(MMC0_TMOUT, ~0);	/* maxium timeout value */
	mmio_write_32(MMC0_DEBNCE, 0x00ffffff);
	mmio_write_32(MMC0_BLKSIZ, MMC_BLOCK_SIZE);
	mmio_write_32(MMC0_BYTCNT, 256 * 1024);
}

static int mmc0_send_cmd(unsigned int cmd, unsigned int arg, unsigned int *buf)
{
#if 1
	unsigned int data, err_mask;
#else
	unsigned int data;
#endif

	if (!buf) {
		NOTICE("buf is invalid\n");
		return -EFAULT;
	}
	NOTICE("#%s, %d\n", __func__, __LINE__);

	mmio_write_32(MMC0_CMDARG, arg);

	/* clear interrupts */
	mmio_write_32(MMC0_RINTSTS, ~0);

	switch (cmd) {
	case 0:
		data = BIT_CMD_SEND_INIT;
		break;
	case 1:
		data = BIT_CMD_RESPONSE_EXPECT;
		break;
	case 2:
		data = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_LONG_RESPONSE |
			BIT_CMD_CHECK_RESPONSE_CRC | BIT_CMD_SEND_INIT;
		break;
	case 3:
		data = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
			BIT_CMD_SEND_INIT;
		break;
	case 8:
		data = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
			BIT_CMD_DATA_EXPECTED | BIT_CMD_READ |
			BIT_CMD_WAIT_PRVDATA_COMPLETE;
		break;
	case 9:
		data = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
			BIT_CMD_LONG_RESPONSE;
		break;
	case 12:
		data = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
			BIT_CMD_STOP_ABORT_CMD;
		break;
	case 17:
	case 18:
		data = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
			BIT_CMD_DATA_EXPECTED | BIT_CMD_READ |
			BIT_CMD_WAIT_PRVDATA_COMPLETE;
		break;
	case 24:
	case 25:
		data = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
			BIT_CMD_DATA_EXPECTED | BIT_CMD_WRITE |
			BIT_CMD_WAIT_PRVDATA_COMPLETE;
		break;
	case 30:
		data = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
			BIT_CMD_DATA_EXPECTED;
		break;
	case 7:
		if (arg)
			data = BIT_CMD_RESPONSE_EXPECT |
				BIT_CMD_CHECK_RESPONSE_CRC;
		else
			data = 0;
		break;
	default:
		data = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC;
		break;
	}
	data |= (cmd & 0x3f) | BIT_CMD_USE_HOLD_REG | BIT_CMD_START;
	NOTICE("#%s, cmd:%d, data:%x\n", __func__, cmd, data);
	mmio_write_32(MMC0_CMD, data);
#if 1
	err_mask = MMC_INT_EBE | MMC_INT_HLE | MMC_INT_RTO | MMC_INT_RCRC |
		   MMC_INT_RE;
#endif
	//NOTICE("#%s, %d, data:%x\n", __func__, __LINE__, data);
	do {
		data = mmio_read_32(MMC0_RINTSTS);
		if (data)
			NOTICE("#%s, %d, data:%x\n", __func__, __LINE__, data);
		if (data & err_mask)
			return data;
	} while (!(data & MMC_INT_CMD_DONE));

	buf[0] = mmio_read_32(MMC0_RESP0);
	if ((cmd == 2) || (cmd == 9)) {
		buf[1] = mmio_read_32(MMC0_RESP1);
		buf[2] = mmio_read_32(MMC0_RESP2);
		buf[3] = mmio_read_32(MMC0_RESP3);
	}
	return 0;
}

/* Only print error message if it meets failure? */
static void mmc0_check_tran_mode(void)
{
	unsigned int buf[4];
	int ret;

	mmio_write_32(MMC0_RINTSTS, 0xffff);

	while (1) {
		ret = mmc0_send_cmd(13, EMMC_FIX_RCA << 16, buf);
		if (ret) {
			NOTICE("failed on command 13\n");
			return;
		}
		if (((buf[0] >> 9) & 0xf) == 4)
			return;
	}
}

static int mmc0_update_ext_csd(int index, int value)
{
	unsigned int arg, data, buf[4];
	int ret;

	arg = 3 << 24;
	arg |= (index & 0xff) << 16;
	arg |= (value & 0xff) << 8;
	arg |= 1;
	memset(buf, 0, 4 * sizeof(buf[0]));

	ret = mmc0_send_cmd(6, arg, buf);
	if (ret) {
		NOTICE("failed to send command 6\n");
		return ret;
	}

	/* wait busy de-assert */
	while (1) {
		data = mmio_read_32(MMC0_STATUS);
		if (!(data & MMC_STS_DATA_BUSY))
			break;
	}

	do {
		ret = mmc0_send_cmd(13, EMMC_FIX_RCA << 16, buf);
		if (ret) {
			NOTICE("failed to send command 13\n");
			return ret;
		}

		if (buf[0] & (1 << 7)) {
			NOTICE("maybe switch mmc mode error\n");
			return -1;
		}
	} while ((buf[0] & 0x1e00) >> 9 == 7);

	return 0;
}

#define EXTCSD_BUS_WIDTH		183

static int mmc0_set_clock_and_width(int rate, int width)
{
	int ret;

	switch (width) {
	case 0:
		mmio_write_32(MMC0_CTYPE, 0);
		ret = mmc0_update_ext_csd(EXTCSD_BUS_WIDTH, 0);
		break;
	case 8:
		mmio_write_32(MMC0_CTYPE, 1 << 16);
		ret = mmc0_update_ext_csd(EXTCSD_BUS_WIDTH, 2 + 4);
		mmio_write_32(MMC0_UHSREG, 1 << 16);
		break;
	default:
		NOTICE("wrong bus width:%d\n", width);
		return -EINVAL;
	}
	if (ret) {
		NOTICE("return failure on %s, %d\n", __func__, __LINE__);
		return ret;
	}

	set_mmc0_clock(rate);
	return 0;
}

static int manu_id;

#define EXTCSD_HS_TIMING		185

static int enum_mmc0_card(void)
{
	unsigned int buf[4], cid[4];
	int ret = 0, i, version;

	NOTICE("#%s, %d\n", __func__, __LINE__);
	/* CMD0: IDLE */
	ret = mmc0_send_cmd(0, 0, buf);
	NOTICE("#%s, %d, ret:%d\n", __func__, __LINE__, ret);
	if (ret) {
		NOTICE("failed to send IDLE command\n");
		return ret;
	}

	while (1) {
		udelay(1000);
		/* CMD1: READY */
		ret = mmc0_send_cmd(1, 0x40ff8000, buf);
		if (ret) {
			NOTICE("failed to send READY command\n");
			return ret;
		}
		if (buf[0] & 0x80000000)
			break;
	}

	/* CMD2: IDENT */
	ret = mmc0_send_cmd(2, 0, buf);
	if (ret) {
		NOTICE("failed to send IDENT command\n");
		return ret;
	}
	for (i = 0; i < 4; i++)
		cid[i] = buf[i];

	NOTICE("#%s, %d\n", __func__, __LINE__);
	/* CMD3: STBY */
	ret = mmc0_send_cmd(3, EMMC_FIX_RCA << 16, buf);
	if (ret) {
		NOTICE("failed to send STBY command\n");
		return ret;
	}

	/* CMD9: get CSD */
	ret = mmc0_send_cmd(9, EMMC_FIX_RCA << 16, buf);
	if (ret) {
		NOTICE("failed to get CSD\n");
		return ret;
	}
	NOTICE("#%s, %d\n", __func__, __LINE__);
	version = (buf[3] >> 26) & 0xf;
	switch (version) {
	case 0:	/* MMC v1.0-v1.2 */
	case 1:	/* MMC v1.4 */
		manu_id = (cid[3] >> 8) & 0xffffff;
		break;
	case 2:	/* MMC v2.0-v2.2 */
	case 3:	/* MMC v3.1-v3.3 */
	case 4:	/* MMC v4 */
		manu_id = (cid[3] >> 24) & 0xff;
		break;
	default:
		NOTICE("wrong mmc version (%d) is specified.\n", version);
		break;
	}

	NOTICE("#%s, %d\n", __func__, __LINE__);
	/* CMD7: TRAN */
	ret = mmc0_send_cmd(7, EMMC_FIX_RCA << 16, buf);
	if (ret) {
		NOTICE("failed to send TRAN command\n");
		return ret;
	}
	mmc0_check_tran_mode();

	ret = mmc0_update_ext_csd(EXTCSD_HS_TIMING, 1);
	if (ret) {
		NOTICE("alter HS mode fail\n");
	}
	NOTICE("#%s, %d\n", __func__, __LINE__);

	ret = mmc0_set_clock_and_width(50000000, 8);
	NOTICE("#%s, %d\n", __func__, __LINE__);
	return 0;
}

#define EXTCSD_PARTITION_CONFIG		179
#define BOOT_PARTITION			(1 << 3)
#define RW_PARTITION_DEFAULT		0

static int enable_mmc0(void)
{
	unsigned int data;

	/* reset mmc0 */
	data = MMC_CTRL_RESET | MMC_FIFO_RESET | MMC_DMA_RESET;
	mmio_write_32(MMC0_CTRL, data);
	/* wait until reset operation finished */
	do {
		data = mmio_read_32(MMC0_CTRL);
	} while (data);

#if 1
	data = MMC_INT_EN | MMC_DMA_EN;
	mmio_write_32(MMC0_CTRL, data);

	mmio_write_32(MMC0_INTMASK, 0x0);
	mmio_write_32(MMC0_RINTSTS, ~0);
	mmio_write_32(MMC0_IDINTEN, ~0);
	mmio_write_32(MMC0_IDSTS, ~0);

	mmio_write_32(MMC0_BMOD, MMC_IDMAC_SWRESET);
	do {
		data = mmio_read_32(MMC0_BMOD);
		//NOTICE("#%s, %d, data:%x\n", __func__, __LINE__, data);
	} while (data & MMC_IDMAC_SWRESET);

	data |= MMC_IDMAC_ENABLE | MMC_IDMAC_FB;
	mmio_write_32(MMC0_BMOD, data);

	data = MMC_DMA_BURST_SIZE(2) | MMC_FIFO_TWMARK(8) | MMC_FIFO_RWMARK(7);
	mmio_write_32(MMC0_FIFOTH, data);
	data = MMC_CARD_RD_THR(512) | MMC_CARD_RD_THR_EN;
	mmio_write_32(MMC0_CARDTHRCTL, data);
#endif

	udelay(100);
	set_mmc0_clock(378000);
	udelay(100);

	set_mmc0_io();
	return 0;
}


void init_mmc(void)
{
	unsigned int buf[4];
	int ret;

	NOTICE("#%s, %d\n", __func__, __LINE__);
	init_mmc_pll();
	NOTICE("#%s, %d\n", __func__, __LINE__);
	reset_mmc0_clk();
	NOTICE("#%s, %d\n", __func__, __LINE__);
	enable_mmc0();

	NOTICE("#%s, %d\n", __func__, __LINE__);
	ret = enum_mmc0_card();
	if (ret)
		return;
	NOTICE("#%s, %d\n", __func__, __LINE__);

	/* set boot mode to 8-bit */
	mmc0_update_ext_csd(177, 2);
	/* response to RESET signal */
	mmc0_update_ext_csd(162, 1);
	/* set access userdata area */
	mmc0_update_ext_csd(EXTCSD_PARTITION_CONFIG, BOOT_PARTITION | RW_PARTITION_DEFAULT);

	mmio_write_32(MMC0_RINTSTS, ~0);
	NOTICE("#%s, %d\n", __func__, __LINE__);

	ret = mmc0_send_cmd(23, 0, buf);
	if (ret) {
		NOTICE("failed to send cmd 23\n");
		mmio_write_32(MMC0_RINTSTS, ~0);
		return;
	}
	NOTICE("#%s, %d\n", __func__, __LINE__);
	mmio_write_32(MMC0_RINTSTS, ~0);
}
