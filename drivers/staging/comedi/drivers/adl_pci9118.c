/*
 *  comedi/drivers/adl_pci9118.c
 *
 *  hardware driver for ADLink cards:
 *   card:   PCI-9118DG, PCI-9118HG, PCI-9118HR
 *   driver: pci9118dg,  pci9118hg,  pci9118hr
 *
 * Author: Michal Dobes <dobes@tesnet.cz>
 *
 */

/*
 * Driver: adl_pci9118
 * Description: Adlink PCI-9118DG, PCI-9118HG, PCI-9118HR
 * Author: Michal Dobes <dobes@tesnet.cz>
 * Devices: [ADLink] PCI-9118DG (pci9118dg), PCI-9118HG (pci9118hg),
 * PCI-9118HR (pci9118hr)
 * Status: works
 *
 * This driver supports AI, AO, DI and DO subdevices.
 * AI subdevice supports cmd and insn interface,
 * other subdevices support only insn interface.
 * For AI:
 * - If cmd->scan_begin_src=TRIG_EXT then trigger input is TGIN (pin 46).
 * - If cmd->convert_src=TRIG_EXT then trigger input is EXTTRG (pin 44).
 * - If cmd->start_src/stop_src=TRIG_EXT then trigger input is TGIN (pin 46).
 * - It is not necessary to have cmd.scan_end_arg=cmd.chanlist_len but
 * cmd.scan_end_arg modulo cmd.chanlist_len must by 0.
 * - If return value of cmdtest is 5 then you've bad channel list
 * (it isn't possible mixture S.E. and DIFF inputs or bipolar and unipolar
 * ranges).
 *
 * There are some hardware limitations:
 * a) You cann't use mixture of unipolar/bipoar ranges or differencial/single
 *  ended inputs.
 * b) DMA transfers must have the length aligned to two samples (32 bit),
 *  so there is some problems if cmd->chanlist_len is odd. This driver tries
 *  bypass this with adding one sample to the end of the every scan and discard
 *  it on output but this cann't be used if cmd->scan_begin_src=TRIG_FOLLOW
 *  and is used flag TRIG_WAKE_EOS, then driver switch to interrupt driven mode
 *  with interrupt after every sample.
 * c) If isn't used DMA then you can use only mode where
 *  cmd->scan_begin_src=TRIG_FOLLOW.
 *
 * Configuration options:
 * [0] - PCI bus of device (optional)
 * [1] - PCI slot of device (optional)
 *	 If bus/slot is not specified, then first available PCI
 *	 card will be used.
 * [2] - 0= standard 8 DIFF/16 SE channels configuration
 *	 n = external multiplexer connected, 1 <= n <= 256
 * [3] - 0=autoselect DMA or EOC interrupts operation
 *	 1 = disable DMA mode
 *	 3 = disable DMA and INT, only insn interface will work
 * [4] - sample&hold signal - card can generate signal for external S&H board
 *	 0 = use SSHO(pin 45) signal is generated in onboard hardware S&H logic
 *	 0 != use ADCHN7(pin 23) signal is generated from driver, number say how
 *		long delay is requested in ns and sign polarity of the hold
 *		(in this case external multiplexor can serve only 128 channels)
 * [5] - 0=stop measure on all hardware errors
 *	 2 | = ignore ADOR - A/D Overrun status
 *	 8|=ignore Bover - A/D Burst Mode Overrun status
 *	 256|=ignore nFull - A/D FIFO Full status
 *
 */

/*
 * FIXME
 *
 * All the supported boards have the same PCI vendor and device IDs, so
 * auto-attachment of PCI devices will always find the first board type.
 *
 * Perhaps the boards have different subdevice IDs that we could use to
 * distinguish them?
 *
 * Need some device attributes so the board type can be corrected after
 * attachment if necessary, and possibly to set other options supported by
 * manual attachment.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/gfp.h>
#include <linux/interrupt.h>
#include <linux/io.h>

#include "../comedidev.h"

#include "amcc_s5933.h"
#include "8253.h"
#include "comedi_fc.h"

/* paranoid checks are broken */
#undef PCI9118_PARANOIDCHECK	/*
				 * if defined, then is used code which control
				 * correct channel number on every 12 bit sample
				 */

#define IORANGE_9118	64	/* I hope */
#define PCI9118_CHANLEN	255	/*
				 * len of chanlist, some source say 256,
				 * but reality looks like 255 :-(
				 */

#define PCI9118_CNT0	0x00	/* R/W: 8254 counter 0 */
#define PCI9118_CNT1	0x04	/* R/W: 8254 counter 0 */
#define PCI9118_CNT2	0x08	/* R/W: 8254 counter 0 */
#define PCI9118_CNTCTRL	0x0c	/* W:   8254 counter control */
#define PCI9118_AD_DATA	0x10	/* R:   A/D data */
#define PCI9118_DA1	0x10	/* W:   D/A registers */
#define PCI9118_DA2	0x14
#define PCI9118_ADSTAT	0x18	/* R:   A/D status register */
#define PCI9118_ADCNTRL	0x18	/* W:   A/D control register */
#define PCI9118_DI	0x1c	/* R:   digi input register */
#define PCI9118_DO	0x1c	/* W:   digi output register */
#define PCI9118_SOFTTRG	0x20	/* W:   soft trigger for A/D */
#define PCI9118_GAIN	0x24	/* W:   A/D gain/channel register */
#define PCI9118_BURST	0x28	/* W:   A/D burst number register */
#define PCI9118_SCANMOD	0x2c	/* W:   A/D auto scan mode */
#define PCI9118_ADFUNC	0x30	/* W:   A/D function register */
#define PCI9118_DELFIFO	0x34	/* W:   A/D data FIFO reset */
#define PCI9118_INTSRC	0x38	/* R:   interrupt reason register */
#define PCI9118_INTCTRL	0x38	/* W:   interrupt control register */

/* bits from A/D control register (PCI9118_ADCNTRL) */
#define AdControl_UniP	0x80	/* 1=bipolar, 0=unipolar */
#define AdControl_Diff	0x40	/* 1=differential, 0= single end inputs */
#define AdControl_SoftG	0x20	/* 1=8254 counter works, 0=counter stops */
#define	AdControl_ExtG	0x10	/*
				 * 1=8254 countrol controlled by TGIN(pin 46),
				 * 0=controlled by SoftG
				 */
#define AdControl_ExtM	0x08	/*
				 * 1=external hardware trigger (pin 44),
				 * 0=internal trigger
				 */
#define AdControl_TmrTr	0x04	/*
				 * 1=8254 is iternal trigger source,
				 * 0=software trigger is source
				 * (register PCI9118_SOFTTRG)
				 */
#define AdControl_Int	0x02	/* 1=enable INT, 0=disable */
#define AdControl_Dma	0x01	/* 1=enable DMA, 0=disable */

/* bits from A/D function register (PCI9118_ADFUNC) */
#define AdFunction_PDTrg	0x80	/*
					 * 1=positive,
					 * 0=negative digital trigger
					 * (only positive is correct)
					 */
#define AdFunction_PETrg	0x40	/*
					 * 1=positive,
					 * 0=negative external trigger
					 * (only positive is correct)
					 */
#define AdFunction_BSSH		0x20	/* 1=with sample&hold, 0=without */
#define AdFunction_BM		0x10	/* 1=burst mode, 0=normal mode */
#define AdFunction_BS		0x08	/*
					 * 1=burst mode start,
					 * 0=burst mode stop
					 */
#define AdFunction_PM		0x04	/*
					 * 1=post trigger mode,
					 * 0=not post trigger
					 */
#define AdFunction_AM		0x02	/*
					 * 1=about trigger mode,
					 * 0=not about trigger
					 */
#define AdFunction_Start	0x01	/* 1=trigger start, 0=trigger stop */

/* bits from A/D status register (PCI9118_ADSTAT) */
#define AdStatus_nFull	0x100	/* 0=FIFO full (fatal), 1=not full */
#define AdStatus_nHfull	0x080	/* 0=FIFO half full, 1=FIFO not half full */
#define AdStatus_nEpty	0x040	/* 0=FIFO empty, 1=FIFO not empty */
#define AdStatus_Acmp	0x020	/*  */
#define AdStatus_DTH	0x010	/* 1=external digital trigger */
#define AdStatus_Bover	0x008	/* 1=burst mode overrun (fatal) */
#define AdStatus_ADOS	0x004	/* 1=A/D over speed (warning) */
#define AdStatus_ADOR	0x002	/* 1=A/D overrun (fatal) */
#define AdStatus_ADrdy	0x001	/* 1=A/D already ready, 0=not ready */

/* bits for interrupt reason and control (PCI9118_INTSRC, PCI9118_INTCTRL) */
/* 1=interrupt occur, enable source,  0=interrupt not occur, disable source */
#define Int_Timer	0x08	/* timer interrupt */
#define Int_About	0x04	/* about trigger complete */
#define Int_Hfull	0x02	/* A/D FIFO hlaf full */
#define Int_DTrg	0x01	/* external digital trigger */

#define START_AI_EXT	0x01	/* start measure on external trigger */
#define STOP_AI_EXT	0x02	/* stop measure on external trigger */
#define START_AI_INT	0x04	/* start measure on internal trigger */
#define STOP_AI_INT	0x08	/* stop measure on internal trigger */

#define EXTTRG_AI	0	/* ext trg is used by AI */

static const struct comedi_lrange range_pci9118dg_hr = {
	8, {
		BIP_RANGE(5),
		BIP_RANGE(2.5),
		BIP_RANGE(1.25),
		BIP_RANGE(0.625),
		UNI_RANGE(10),
		UNI_RANGE(5),
		UNI_RANGE(2.5),
		UNI_RANGE(1.25)
	}
};

static const struct comedi_lrange range_pci9118hg = {
	8, {
		BIP_RANGE(5),
		BIP_RANGE(0.5),
		BIP_RANGE(0.05),
		BIP_RANGE(0.005),
		UNI_RANGE(10),
		UNI_RANGE(1),
		UNI_RANGE(0.1),
		UNI_RANGE(0.01)
	}
};

#define PCI9118_BIPOLAR_RANGES	4	/*
					 * used for test on mixture
					 * of BIP/UNI ranges
					 */

struct boardtype {
	const char *name;		/* board name */
	int device_id;			/* PCI device ID of card */
	int iorange_amcc;		/* iorange for own S5933 region */
	int iorange_9118;		/* pass thru card region size */
	int n_aichan;			/* num of A/D chans */
	int n_aichand;			/* num of A/D chans in diff mode */
	int mux_aichan;			/*
					 * num of A/D chans with
					 * external multiplexor
					 */
	int n_aichanlist;		/* len of chanlist */
	int n_aochan;			/* num of D/A chans */
	int ai_maxdata;			/* resolution of A/D */
	int ao_maxdata;			/* resolution of D/A */
	const struct comedi_lrange *rangelist_ai;	/* rangelist for A/D */
	const struct comedi_lrange *rangelist_ao;	/* rangelist for D/A */
	unsigned int ai_ns_min;		/* max sample speed of card v ns */
	unsigned int ai_pacer_min;	/*
					 * minimal pacer value
					 * (c1*c2 or c1 in burst)
					 */
	int half_fifo_size;		/* size of FIFO/2 */

};

static const struct boardtype boardtypes[] = {
	{
		.name		= "pci9118dg",
		.device_id	= 0x80d9,
		.iorange_amcc	= AMCC_OP_REG_SIZE,
		.iorange_9118	= IORANGE_9118,
		.n_aichan	= 16,
		.n_aichand	= 8,
		.mux_aichan	= 256,
		.n_aichanlist	= PCI9118_CHANLEN,
		.n_aochan	= 2,
		.ai_maxdata	= 0x0fff,
		.ao_maxdata	= 0x0fff,
		.rangelist_ai	= &range_pci9118dg_hr,
		.rangelist_ao	= &range_bipolar10,
		.ai_ns_min	= 3000,
		.ai_pacer_min	= 12,
		.half_fifo_size	= 512,
	}, {
		.name		= "pci9118hg",
		.device_id	= 0x80d9,
		.iorange_amcc	= AMCC_OP_REG_SIZE,
		.iorange_9118	= IORANGE_9118,
		.n_aichan	= 16,
		.n_aichand	= 8,
		.mux_aichan	= 256,
		.n_aichanlist	= PCI9118_CHANLEN,
		.n_aochan	= 2,
		.ai_maxdata	= 0x0fff,
		.ao_maxdata	= 0x0fff,
		.rangelist_ai	= &range_pci9118hg,
		.rangelist_ao	= &range_bipolar10,
		.ai_ns_min	= 3000,
		.ai_pacer_min	= 12,
		.half_fifo_size	= 512,
	}, {
		.name		= "pci9118hr",
		.device_id	= 0x80d9,
		.iorange_amcc	= AMCC_OP_REG_SIZE,
		.iorange_9118	= IORANGE_9118,
		.n_aichan	= 16,
		.n_aichand	= 8,
		.mux_aichan	= 256,
		.n_aichanlist	= PCI9118_CHANLEN,
		.n_aochan	= 2,
		.ai_maxdata	= 0xffff,
		.ao_maxdata	= 0x0fff,
		.rangelist_ai	= &range_pci9118dg_hr,
		.rangelist_ao	= &range_bipolar10,
		.ai_ns_min	= 10000,
		.ai_pacer_min	= 40,
		.half_fifo_size	= 512,
	},
};

struct pci9118_private {
	unsigned long iobase_a;	/* base+size for AMCC chip */
	unsigned int master;	/* master capable */
	unsigned int usemux;	/* we want to use external multiplexor! */
#ifdef PCI9118_PARANOIDCHECK
	unsigned short chanlist[PCI9118_CHANLEN + 1];	/*
							 * list of
							 * scanned channel
							 */
	unsigned char chanlistlen;	/* number of scanlist */
#endif
	unsigned char AdControlReg;	/* A/D control register */
	unsigned char IntControlReg;	/* Interrupt control register */
	unsigned char AdFunctionReg;	/* A/D function register */
	char ai_neverending;		/* we do unlimited AI */
	unsigned int ai_do;		/* what do AI? 0=nothing, 1 to 4 mode */
	unsigned int ai_act_scan;	/* how many scans we finished */
	unsigned int ai_n_realscanlen;	/*
					 * what we must transfer for one
					 * outgoing scan include front/back adds
					 */
	unsigned int ai_act_dmapos;	/* position in actual real stream */
	unsigned int ai_add_front;	/*
					 * how many channels we must add
					 * before scan to satisfy S&H?
					 */
	unsigned int ai_add_back;	/*
					 * how many channels we must add
					 * before scan to satisfy DMA?
					 */
	unsigned int ai_flags;
	char ai12_startstop;		/*
					 * measure can start/stop
					 * on external trigger
					 */
	unsigned int ai_divisor1, ai_divisor2;	/*
						 * divisors for start of measure
						 * on external start
						 */
	unsigned short ao_data[2];		/* data output buffer */
	char dma_doublebuf;			/* use double buffering */
	unsigned int dma_actbuf;		/* which buffer is used now */
	unsigned short *dmabuf_virt[2];		/*
						 * pointers to begin of
						 * DMA buffer
						 */
	unsigned long dmabuf_hw[2];		/* hw address of DMA buff */
	unsigned int dmabuf_size[2];		/*
						 * size of dma buffer in bytes
						 */
	unsigned int dmabuf_use_size[2];	/*
						 * which size we may now use
						 * for transfer
						 */
	unsigned int dmabuf_used_size[2];	/* which size was truly used */
	unsigned int dmabuf_panic_size[2];
	int dmabuf_pages[2];			/* number of pages in buffer */
	unsigned char exttrg_users;		/*
						 * bit field of external trigger
						 * users(0-AI, 1-AO, 2-DI, 3-DO)
						 */
	unsigned char usedma;		/* =1 use DMA transfer and not INT */
	int softsshdelay;		/*
					 * >0 use software S&H,
					 * numer is requested delay in ns
					 */
	unsigned char softsshsample;	/*
					 * polarity of S&H signal
					 * in sample state
					 */
	unsigned char softsshhold;	/*
					 * polarity of S&H signal
					 * in hold state
					 */
	unsigned int ai_maskerr;	/* which warning was printed */
	unsigned int ai_maskharderr;	/* on which error bits stops */
};

static int check_channel_list(struct comedi_device *dev,
			      struct comedi_subdevice *s, int n_chan,
			      unsigned int *chanlist, int frontadd, int backadd)
{
	const struct boardtype *this_board = comedi_board(dev);
	struct pci9118_private *devpriv = dev->private;
	unsigned int i, differencial = 0, bipolar = 0;

	/* correct channel and range number check itself comedi/range.c */
	if (n_chan < 1) {
		dev_err(dev->class_dev, "range/channel list is empty!\n");
		return 0;
	}
	if ((frontadd + n_chan + backadd) > s->len_chanlist) {
		dev_err(dev->class_dev,
			"range/channel list is too long for actual configuration!\n");
		return 0;
	}

	if (CR_AREF(chanlist[0]) == AREF_DIFF)
		differencial = 1;	/* all input must be diff */
	if (CR_RANGE(chanlist[0]) < PCI9118_BIPOLAR_RANGES)
		bipolar = 1;	/* all input must be bipolar */
	if (n_chan > 1)
		for (i = 1; i < n_chan; i++) {	/* check S.E/diff */
			if ((CR_AREF(chanlist[i]) == AREF_DIFF) !=
			    (differencial)) {
				dev_err(dev->class_dev,
					"Differential and single ended inputs can't be mixed!\n");
				return 0;
			}
			if ((CR_RANGE(chanlist[i]) < PCI9118_BIPOLAR_RANGES) !=
			    (bipolar)) {
				dev_err(dev->class_dev,
					"Bipolar and unipolar ranges can't be mixed!\n");
				return 0;
			}
			if (!devpriv->usemux && differencial &&
			    (CR_CHAN(chanlist[i]) >= this_board->n_aichand)) {
				dev_err(dev->class_dev,
					"AREF_DIFF is only available for the first 8 channels!\n");
				return 0;
			}
		}

	return 1;
}

static int setup_channel_list(struct comedi_device *dev,
			      struct comedi_subdevice *s, int n_chan,
			      unsigned int *chanlist, int rot, int frontadd,
			      int backadd, int usedma)
{
	struct pci9118_private *devpriv = dev->private;
	unsigned int i, differencial = 0, bipolar = 0;
	unsigned int scanquad, gain, ssh = 0x00;

	if (usedma == 1) {
		rot = 8;
		usedma = 0;
	}

	if (CR_AREF(chanlist[0]) == AREF_DIFF)
		differencial = 1;	/* all input must be diff */
	if (CR_RANGE(chanlist[0]) < PCI9118_BIPOLAR_RANGES)
		bipolar = 1;	/* all input must be bipolar */

	/* All is ok, so we can setup channel/range list */

	if (!bipolar) {
		devpriv->AdControlReg |= AdControl_UniP;
							/* set unibipolar */
	} else {
		devpriv->AdControlReg &= ((~AdControl_UniP) & 0xff);
							/* enable bipolar */
	}

	if (differencial) {
		devpriv->AdControlReg |= AdControl_Diff;
							/* enable diff inputs */
	} else {
		devpriv->AdControlReg &= ((~AdControl_Diff) & 0xff);
						/* set single ended inputs */
	}

	outl(devpriv->AdControlReg, dev->iobase + PCI9118_ADCNTRL);
								/* setup mode */

	outl(2, dev->iobase + PCI9118_SCANMOD);
					/* gods know why this sequence! */
	outl(0, dev->iobase + PCI9118_SCANMOD);
	outl(1, dev->iobase + PCI9118_SCANMOD);

#ifdef PCI9118_PARANOIDCHECK
	devpriv->chanlistlen = n_chan;
	for (i = 0; i < (PCI9118_CHANLEN + 1); i++)
		devpriv->chanlist[i] = 0x55aa;
#endif

	if (frontadd) {		/* insert channels for S&H */
		ssh = devpriv->softsshsample;
		for (i = 0; i < frontadd; i++) {
						/* store range list to card */
			scanquad = CR_CHAN(chanlist[0]);
						/* get channel number; */
			gain = CR_RANGE(chanlist[0]);
						/* get gain number */
			scanquad |= ((gain & 0x03) << 8);
			outl(scanquad | ssh, dev->iobase + PCI9118_GAIN);
			ssh = devpriv->softsshhold;
		}
	}

	for (i = 0; i < n_chan; i++) {	/* store range list to card */
		scanquad = CR_CHAN(chanlist[i]);	/* get channel number */
#ifdef PCI9118_PARANOIDCHECK
		devpriv->chanlist[i ^ usedma] = (scanquad & 0xf) << rot;
#endif
		gain = CR_RANGE(chanlist[i]);		/* get gain number */
		scanquad |= ((gain & 0x03) << 8);
		outl(scanquad | ssh, dev->iobase + PCI9118_GAIN);
	}

	if (backadd) {		/* insert channels for fit onto 32bit DMA */
		for (i = 0; i < backadd; i++) {	/* store range list to card */
			scanquad = CR_CHAN(chanlist[0]);
							/* get channel number */
			gain = CR_RANGE(chanlist[0]);	/* get gain number */
			scanquad |= ((gain & 0x03) << 8);
			outl(scanquad | ssh, dev->iobase + PCI9118_GAIN);
		}
	}
#ifdef PCI9118_PARANOIDCHECK
	devpriv->chanlist[n_chan ^ usedma] = devpriv->chanlist[0 ^ usedma];
						/* for 32bit operations */
#endif
	outl(0, dev->iobase + PCI9118_SCANMOD);	/* close scan queue */
	/* udelay(100); important delay, or first sample will be crippled */

	return 1;		/* we can serve this with scan logic */
}

static int pci9118_ai_eoc(struct comedi_device *dev,
			  struct comedi_subdevice *s,
			  struct comedi_insn *insn,
			  unsigned long context)
{
	unsigned int status;

	status = inl(dev->iobase + PCI9118_ADSTAT);
	if (status & AdStatus_ADrdy)
		return 0;
	return -EBUSY;
}

static int pci9118_insn_read_ai(struct comedi_device *dev,
				struct comedi_subdevice *s,
				struct comedi_insn *insn, unsigned int *data)
{
	struct pci9118_private *devpriv = dev->private;
	int ret;
	int n;

	devpriv->AdControlReg = AdControl_Int & 0xff;
	devpriv->AdFunctionReg = AdFunction_PDTrg | AdFunction_PETrg;
	outl(devpriv->AdFunctionReg, dev->iobase + PCI9118_ADFUNC);
						/*
						 * positive triggers, no S&H,
						 * no burst, burst stop,
						 * no post trigger,
						 * no about trigger,
						 * trigger stop
						 */

	if (!setup_channel_list(dev, s, 1, &insn->chanspec, 0, 0, 0, 0))
		return -EINVAL;

	outl(0, dev->iobase + PCI9118_DELFIFO);	/* flush FIFO */

	for (n = 0; n < insn->n; n++) {
		outw(0, dev->iobase + PCI9118_SOFTTRG);	/* start conversion */
		udelay(2);

		ret = comedi_timeout(dev, s, insn, pci9118_ai_eoc, 0);
		if (ret) {
			outl(0, dev->iobase + PCI9118_DELFIFO);	/* flush FIFO */
			return ret;
		}

		if (s->maxdata == 0xffff) {
			data[n] =
			    (inl(dev->iobase +
				 PCI9118_AD_DATA) & 0xffff) ^ 0x8000;
		} else {
			data[n] =
			    (inw(dev->iobase + PCI9118_AD_DATA) >> 4) & 0xfff;
		}
	}

	outl(0, dev->iobase + PCI9118_DELFIFO);	/* flush FIFO */
	return n;

}

static int pci9118_insn_write_ao(struct comedi_device *dev,
				 struct comedi_subdevice *s,
				 struct comedi_insn *insn, unsigned int *data)
{
	struct pci9118_private *devpriv = dev->private;
	int n, chanreg, ch;

	ch = CR_CHAN(insn->chanspec);
	if (ch)
		chanreg = PCI9118_DA2;
	else
		chanreg = PCI9118_DA1;


	for (n = 0; n < insn->n; n++) {
		outl(data[n], dev->iobase + chanreg);
		devpriv->ao_data[ch] = data[n];
	}

	return n;
}

static int pci9118_insn_read_ao(struct comedi_device *dev,
				struct comedi_subdevice *s,
				struct comedi_insn *insn, unsigned int *data)
{
	struct pci9118_private *devpriv = dev->private;
	int n, chan;

	chan = CR_CHAN(insn->chanspec);
	for (n = 0; n < insn->n; n++)
		data[n] = devpriv->ao_data[chan];

	return n;
}

static int pci9118_insn_bits_di(struct comedi_device *dev,
				struct comedi_subdevice *s,
				struct comedi_insn *insn, unsigned int *data)
{
	data[1] = inl(dev->iobase + PCI9118_DI) & 0xf;

	return insn->n;
}

static int pci9118_insn_bits_do(struct comedi_device *dev,
				struct comedi_subdevice *s,
				struct comedi_insn *insn,
				unsigned int *data)
{
	if (comedi_dio_update_state(s, data))
		outl(s->state & 0x0f, dev->iobase + PCI9118_DO);

	data[1] = s->state;

	return insn->n;
}

static void interrupt_pci9118_ai_mode4_switch(struct comedi_device *dev)
{
	struct pci9118_private *devpriv = dev->private;

	devpriv->AdFunctionReg =
	    AdFunction_PDTrg | AdFunction_PETrg | AdFunction_AM;
	outl(devpriv->AdFunctionReg, dev->iobase + PCI9118_ADFUNC);
	outl(0x30, dev->iobase + PCI9118_CNTCTRL);
	outl((devpriv->dmabuf_hw[1 - devpriv->dma_actbuf] >> 1) & 0xff,
	     dev->iobase + PCI9118_CNT0);
	outl((devpriv->dmabuf_hw[1 - devpriv->dma_actbuf] >> 9) & 0xff,
	     dev->iobase + PCI9118_CNT0);
	devpriv->AdFunctionReg |= AdFunction_Start;
	outl(devpriv->AdFunctionReg, dev->iobase + PCI9118_ADFUNC);
}

static unsigned int defragment_dma_buffer(struct comedi_device *dev,
					  struct comedi_subdevice *s,
					  unsigned short *dma_buffer,
					  unsigned int num_samples)
{
	struct pci9118_private *devpriv = dev->private;
	struct comedi_cmd *cmd = &s->async->cmd;
	unsigned int i = 0, j = 0;
	unsigned int start_pos = devpriv->ai_add_front,
	    stop_pos = devpriv->ai_add_front + cmd->chanlist_len;
	unsigned int raw_scanlen = devpriv->ai_add_front + cmd->chanlist_len +
	    devpriv->ai_add_back;

	for (i = 0; i < num_samples; i++) {
		if (devpriv->ai_act_dmapos >= start_pos &&
		    devpriv->ai_act_dmapos < stop_pos) {
			dma_buffer[j++] = dma_buffer[i];
		}
		devpriv->ai_act_dmapos++;
		devpriv->ai_act_dmapos %= raw_scanlen;
	}

	return j;
}

static int move_block_from_dma(struct comedi_device *dev,
					struct comedi_subdevice *s,
					unsigned short *dma_buffer,
					unsigned int num_samples)
{
	struct pci9118_private *devpriv = dev->private;
	struct comedi_cmd *cmd = &s->async->cmd;
	unsigned int num_bytes;

	num_samples = defragment_dma_buffer(dev, s, dma_buffer, num_samples);
	devpriv->ai_act_scan +=
	    (s->async->cur_chan + num_samples) / cmd->scan_end_arg;
	s->async->cur_chan += num_samples;
	s->async->cur_chan %= cmd->scan_end_arg;
	num_bytes =
	    cfc_write_array_to_buffer(s, dma_buffer,
				      num_samples * sizeof(short));
	if (num_bytes < num_samples * sizeof(short))
		return -1;
	return 0;
}

static int pci9118_exttrg_add(struct comedi_device *dev, unsigned char source)
{
	struct pci9118_private *devpriv = dev->private;

	if (source > 3)
		return -1;				/* incorrect source */
	devpriv->exttrg_users |= (1 << source);
	devpriv->IntControlReg |= Int_DTrg;
	outl(devpriv->IntControlReg, dev->iobase + PCI9118_INTCTRL);
	outl(inl(devpriv->iobase_a + AMCC_OP_REG_INTCSR) | 0x1f00,
					devpriv->iobase_a + AMCC_OP_REG_INTCSR);
							/* allow INT in AMCC */
	return 0;
}

static int pci9118_exttrg_del(struct comedi_device *dev, unsigned char source)
{
	struct pci9118_private *devpriv = dev->private;

	if (source > 3)
		return -1;			/* incorrect source */
	devpriv->exttrg_users &= ~(1 << source);
	if (!devpriv->exttrg_users) {	/* shutdown ext trg intterrupts */
		devpriv->IntControlReg &= ~Int_DTrg;
		if (!devpriv->IntControlReg)	/* all IRQ disabled */
			outl(inl(devpriv->iobase_a + AMCC_OP_REG_INTCSR) &
					(~0x00001f00),
					devpriv->iobase_a + AMCC_OP_REG_INTCSR);
						/* disable int in AMCC */
		outl(devpriv->IntControlReg, dev->iobase + PCI9118_INTCTRL);
	}
	return 0;
}

static void pci9118_calc_divisors(char mode, struct comedi_device *dev,
				  struct comedi_subdevice *s,
				  unsigned int *tim1, unsigned int *tim2,
				  unsigned int flags, int chans,
				  unsigned int *div1, unsigned int *div2,
				  unsigned int chnsshfront)
{
	const struct boardtype *this_board = comedi_board(dev);
	struct comedi_cmd *cmd = &s->async->cmd;

	switch (mode) {
	case 1:
	case 4:
		if (*tim2 < this_board->ai_ns_min)
			*tim2 = this_board->ai_ns_min;
		i8253_cascade_ns_to_timer(I8254_OSC_BASE_4MHZ,
					  div1, div2,
					  tim2, flags & TRIG_ROUND_NEAREST);
		break;
	case 2:
		if (*tim2 < this_board->ai_ns_min)
			*tim2 = this_board->ai_ns_min;
		*div1 = *tim2 / I8254_OSC_BASE_4MHZ;
						/* convert timer (burst) */
		if (*div1 < this_board->ai_pacer_min)
			*div1 = this_board->ai_pacer_min;
		*div2 = *tim1 / I8254_OSC_BASE_4MHZ;	/* scan timer */
		*div2 = *div2 / *div1;		/* major timer is c1*c2 */
		if (*div2 < chans)
			*div2 = chans;

		*tim2 = *div1 * I8254_OSC_BASE_4MHZ;	/* real convert timer */

		if (cmd->convert_src == TRIG_NOW && !chnsshfront) {
			/* use BSSH signal */
			if (*div2 < (chans + 2))
				*div2 = chans + 2;
		}

		*tim1 = *div1 * *div2 * I8254_OSC_BASE_4MHZ;
		break;
	}
}

static void pci9118_start_pacer(struct comedi_device *dev, int mode)
{
	struct pci9118_private *devpriv = dev->private;
	unsigned int divisor1 = devpriv->ai_divisor1;
	unsigned int divisor2 = devpriv->ai_divisor2;

	outl(0x74, dev->iobase + PCI9118_CNTCTRL);
	outl(0xb4, dev->iobase + PCI9118_CNTCTRL);
/* outl(0x30, dev->iobase + PCI9118_CNTCTRL); */
	udelay(1);

	if ((mode == 1) || (mode == 2) || (mode == 4)) {
		outl(divisor2 & 0xff, dev->iobase + PCI9118_CNT2);
		outl((divisor2 >> 8) & 0xff, dev->iobase + PCI9118_CNT2);
		outl(divisor1 & 0xff, dev->iobase + PCI9118_CNT1);
		outl((divisor1 >> 8) & 0xff, dev->iobase + PCI9118_CNT1);
	}
}

static int pci9118_ai_cancel(struct comedi_device *dev,
			     struct comedi_subdevice *s)
{
	struct pci9118_private *devpriv = dev->private;

	if (devpriv->usedma)
		outl(inl(devpriv->iobase_a + AMCC_OP_REG_MCSR) &
			(~EN_A2P_TRANSFERS),
			devpriv->iobase_a + AMCC_OP_REG_MCSR);	/* stop DMA */
	pci9118_exttrg_del(dev, EXTTRG_AI);
	pci9118_start_pacer(dev, 0);	/* stop 8254 counters */
	devpriv->AdFunctionReg = AdFunction_PDTrg | AdFunction_PETrg;
	outl(devpriv->AdFunctionReg, dev->iobase + PCI9118_ADFUNC);
					/*
					 * positive triggers, no S&H, no burst,
					 * burst stop, no post trigger,
					 * no about trigger, trigger stop
					 */
	devpriv->AdControlReg = 0x00;
	outl(devpriv->AdControlReg, dev->iobase + PCI9118_ADCNTRL);
					/*
					 * bipolar, S.E., use 8254, stop 8354,
					 * internal trigger, soft trigger,
					 * disable INT and DMA
					 */
	outl(0, dev->iobase + PCI9118_BURST);
	outl(1, dev->iobase + PCI9118_SCANMOD);
	outl(2, dev->iobase + PCI9118_SCANMOD);	/* reset scan queue */
	outl(0, dev->iobase + PCI9118_DELFIFO);	/* flush FIFO */

	devpriv->ai_do = 0;
	devpriv->usedma = 0;

	devpriv->ai_act_scan = 0;
	devpriv->ai_act_dmapos = 0;
	s->async->cur_chan = 0;
	s->async->inttrig = NULL;
	devpriv->ai_neverending = 0;
	devpriv->dma_actbuf = 0;

	if (!devpriv->IntControlReg)
		outl(inl(devpriv->iobase_a + AMCC_OP_REG_INTCSR) | 0x1f00,
					devpriv->iobase_a + AMCC_OP_REG_INTCSR);
							/* allow INT in AMCC */

	return 0;
}

static char pci9118_decode_error_status(struct comedi_device *dev,
					struct comedi_subdevice *s,
					unsigned char m)
{
	struct pci9118_private *devpriv = dev->private;

	if (m & 0x100) {
		dev_err(dev->class_dev,
			"A/D FIFO Full status (Fatal Error!)\n");
		devpriv->ai_maskerr &= ~0x100L;
	}
	if (m & 0x008) {
		dev_err(dev->class_dev,
			"A/D Burst Mode Overrun Status (Fatal Error!)\n");
		devpriv->ai_maskerr &= ~0x008L;
	}
	if (m & 0x004) {
		dev_err(dev->class_dev, "A/D Over Speed Status (Warning!)\n");
		devpriv->ai_maskerr &= ~0x004L;
	}
	if (m & 0x002) {
		dev_err(dev->class_dev, "A/D Overrun Status (Fatal Error!)\n");
		devpriv->ai_maskerr &= ~0x002L;
	}
	if (m & devpriv->ai_maskharderr) {
		s->async->events |= COMEDI_CB_ERROR | COMEDI_CB_EOA;
		cfc_handle_events(dev, s);
		return 1;
	}

	return 0;
}

static void pci9118_ai_munge(struct comedi_device *dev,
			     struct comedi_subdevice *s, void *data,
			     unsigned int num_bytes,
			     unsigned int start_chan_index)
{
	struct pci9118_private *devpriv = dev->private;
	unsigned int i, num_samples = num_bytes / sizeof(short);
	unsigned short *array = data;

	for (i = 0; i < num_samples; i++) {
		if (devpriv->usedma)
			array[i] = be16_to_cpu(array[i]);
		if (s->maxdata == 0xffff)
			array[i] ^= 0x8000;
		else
			array[i] = (array[i] >> 4) & 0x0fff;

	}
}

static void interrupt_pci9118_ai_onesample(struct comedi_device *dev,
					   struct comedi_subdevice *s,
					   unsigned short int_adstat,
					   unsigned int int_amcc,
					   unsigned short int_daq)
{
	struct pci9118_private *devpriv = dev->private;
	struct comedi_cmd *cmd = &s->async->cmd;
	unsigned short sampl;

	if (int_adstat & devpriv->ai_maskerr)
		if (pci9118_decode_error_status(dev, s, int_adstat))
			return;

	sampl = inw(dev->iobase + PCI9118_AD_DATA);

#ifdef PCI9118_PARANOIDCHECK
	if (s->maxdata != 0xffff) {
		if ((sampl & 0x000f) != devpriv->chanlist[s->async->cur_chan]) {
							/* data dropout! */
			dev_info(dev->class_dev,
				 "A/D  SAMPL - data dropout: received channel %d, expected %d!\n",
				 sampl & 0x000f,
				 devpriv->chanlist[s->async->cur_chan]);
			s->async->events |= COMEDI_CB_ERROR | COMEDI_CB_EOA;
			cfc_handle_events(dev, s);
			return;
		}
	}
#endif
	cfc_write_to_buffer(s, sampl);
	s->async->cur_chan++;
	if (s->async->cur_chan >= cmd->scan_end_arg) {
							/* one scan done */
		s->async->cur_chan %= cmd->scan_end_arg;
		devpriv->ai_act_scan++;
		if (!devpriv->ai_neverending) {
			/* all data sampled? */
			if (devpriv->ai_act_scan >= cmd->stop_arg)
				s->async->events |= COMEDI_CB_EOA;
		}
	}

	cfc_handle_events(dev, s);
}

static void interrupt_pci9118_ai_dma(struct comedi_device *dev,
				     struct comedi_subdevice *s,
				     unsigned short int_adstat,
				     unsigned int int_amcc,
				     unsigned short int_daq)
{
	struct pci9118_private *devpriv = dev->private;
	struct comedi_cmd *cmd = &s->async->cmd;
	unsigned int next_dma_buf, samplesinbuf, sampls, m;

	if (int_amcc & MASTER_ABORT_INT) {
		dev_err(dev->class_dev, "AMCC IRQ - MASTER DMA ABORT!\n");
		s->async->events |= COMEDI_CB_ERROR | COMEDI_CB_EOA;
		cfc_handle_events(dev, s);
		return;
	}

	if (int_amcc & TARGET_ABORT_INT) {
		dev_err(dev->class_dev, "AMCC IRQ - TARGET DMA ABORT!\n");
		s->async->events |= COMEDI_CB_ERROR | COMEDI_CB_EOA;
		cfc_handle_events(dev, s);
		return;
	}
	if (int_adstat & devpriv->ai_maskerr)
					/* if (int_adstat & 0x106) */
		if (pci9118_decode_error_status(dev, s, int_adstat))
			return;

	samplesinbuf = devpriv->dmabuf_use_size[devpriv->dma_actbuf] >> 1;
					/* number of received real samples */

	if (devpriv->dma_doublebuf) {	/*
					 * switch DMA buffers if is used
					 * double buffering
					 */
		next_dma_buf = 1 - devpriv->dma_actbuf;
		outl(devpriv->dmabuf_hw[next_dma_buf],
		     devpriv->iobase_a + AMCC_OP_REG_MWAR);
		outl(devpriv->dmabuf_use_size[next_dma_buf],
		     devpriv->iobase_a + AMCC_OP_REG_MWTC);
		devpriv->dmabuf_used_size[next_dma_buf] =
		    devpriv->dmabuf_use_size[next_dma_buf];
		if (devpriv->ai_do == 4)
			interrupt_pci9118_ai_mode4_switch(dev);
	}

	if (samplesinbuf) {
		/* how many samples is to end of buffer */
		m = s->async->prealloc_bufsz >> 1;
		sampls = m;
		move_block_from_dma(dev, s,
				    devpriv->dmabuf_virt[devpriv->dma_actbuf],
				    samplesinbuf);
		m = m - sampls;		/* m=how many samples was transferred */
	}

	if (!devpriv->ai_neverending) {
		/* all data sampled? */
		if (devpriv->ai_act_scan >= cmd->stop_arg)
			s->async->events |= COMEDI_CB_EOA;
	}

	if (devpriv->dma_doublebuf) {	/* switch dma buffers */
		devpriv->dma_actbuf = 1 - devpriv->dma_actbuf;
	} else {	/* restart DMA if is not used double buffering */
		outl(devpriv->dmabuf_hw[0],
		     devpriv->iobase_a + AMCC_OP_REG_MWAR);
		outl(devpriv->dmabuf_use_size[0],
		     devpriv->iobase_a + AMCC_OP_REG_MWTC);
		if (devpriv->ai_do == 4)
			interrupt_pci9118_ai_mode4_switch(dev);
	}

	cfc_handle_events(dev, s);
}

static irqreturn_t pci9118_interrupt(int irq, void *d)
{
	struct comedi_device *dev = d;
	struct comedi_subdevice *s = dev->read_subdev;
	struct pci9118_private *devpriv = dev->private;
	unsigned int intsrc;	/* IRQ reasons from card */
	unsigned int intcsr;	/* INT register from AMCC chip */
	unsigned int adstat;	/* STATUS register */

	if (!dev->attached)
		return IRQ_NONE;

	intsrc = inl(dev->iobase + PCI9118_INTSRC) & 0xf;
	intcsr = inl(devpriv->iobase_a + AMCC_OP_REG_INTCSR);

	if (!intsrc && !(intcsr & ANY_S593X_INT))
		return IRQ_NONE;

	outl(intcsr | 0x00ff0000, devpriv->iobase_a + AMCC_OP_REG_INTCSR);

	adstat = inw(dev->iobase + PCI9118_ADSTAT) & 0x1ff;

	if (!devpriv->ai_do)
		return IRQ_HANDLED;

	if (devpriv->ai12_startstop) {
		if ((adstat & AdStatus_DTH) && (intsrc & Int_DTrg)) {
			/* start/stop of measure */
			if (devpriv->ai12_startstop & START_AI_EXT) {
				/* deactivate EXT trigger */
				devpriv->ai12_startstop &= ~START_AI_EXT;
				if (!(devpriv->ai12_startstop & STOP_AI_EXT))
					pci9118_exttrg_del(dev, EXTTRG_AI);

				/* start pacer */
				pci9118_start_pacer(dev, devpriv->ai_do);
				outl(devpriv->AdControlReg,
				     dev->iobase + PCI9118_ADCNTRL);
			} else if (devpriv->ai12_startstop & STOP_AI_EXT) {
				/* deactivate EXT trigger */
				devpriv->ai12_startstop &= ~STOP_AI_EXT;
				pci9118_exttrg_del(dev, EXTTRG_AI);

				/* on next interrupt measure will stop */
				devpriv->ai_neverending = 0;
			}
		}
	}

	if (devpriv->usedma)
		interrupt_pci9118_ai_dma(dev, s, adstat, intcsr, intsrc);
	else
		interrupt_pci9118_ai_onesample(dev, s, adstat, intcsr, intsrc);

	return IRQ_HANDLED;
}

static int pci9118_ai_inttrig(struct comedi_device *dev,
			      struct comedi_subdevice *s,
			      unsigned int trig_num)
{
	struct pci9118_private *devpriv = dev->private;
	struct comedi_cmd *cmd = &s->async->cmd;

	if (trig_num != cmd->start_arg)
		return -EINVAL;

	devpriv->ai12_startstop &= ~START_AI_INT;
	s->async->inttrig = NULL;

	outl(devpriv->IntControlReg, dev->iobase + PCI9118_INTCTRL);
	outl(devpriv->AdFunctionReg, dev->iobase + PCI9118_ADFUNC);
	if (devpriv->ai_do != 3) {
		pci9118_start_pacer(dev, devpriv->ai_do);
		devpriv->AdControlReg |= AdControl_SoftG;
	}
	outl(devpriv->AdControlReg, dev->iobase + PCI9118_ADCNTRL);

	return 1;
}

static int pci9118_ai_cmdtest(struct comedi_device *dev,
			      struct comedi_subdevice *s,
			      struct comedi_cmd *cmd)
{
	const struct boardtype *this_board = comedi_board(dev);
	struct pci9118_private *devpriv = dev->private;
	int err = 0;
	unsigned int flags;
	unsigned int arg;
	unsigned int divisor1 = 0, divisor2 = 0;

	/* Step 1 : check if triggers are trivially valid */

	err |= cfc_check_trigger_src(&cmd->start_src,
					TRIG_NOW | TRIG_EXT | TRIG_INT);

	flags = TRIG_FOLLOW;
	if (devpriv->master)
		flags |= TRIG_TIMER | TRIG_EXT;
	err |= cfc_check_trigger_src(&cmd->scan_begin_src, flags);

	flags = TRIG_TIMER | TRIG_EXT;
	if (devpriv->master)
		flags |= TRIG_NOW;
	err |= cfc_check_trigger_src(&cmd->convert_src, flags);

	err |= cfc_check_trigger_src(&cmd->scan_end_src, TRIG_COUNT);
	err |= cfc_check_trigger_src(&cmd->stop_src,
					TRIG_COUNT | TRIG_NONE | TRIG_EXT);

	if (err)
		return 1;

	/* Step 2a : make sure trigger sources are unique */

	err |= cfc_check_trigger_is_unique(cmd->start_src);
	err |= cfc_check_trigger_is_unique(cmd->scan_begin_src);
	err |= cfc_check_trigger_is_unique(cmd->convert_src);
	err |= cfc_check_trigger_is_unique(cmd->stop_src);

	/* Step 2b : and mutually compatible */

	if (cmd->start_src == TRIG_EXT && cmd->scan_begin_src == TRIG_EXT)
		err |= -EINVAL;

	if (cmd->start_src == TRIG_INT && cmd->scan_begin_src == TRIG_INT)
		err |= -EINVAL;

	if ((cmd->scan_begin_src & (TRIG_TIMER | TRIG_EXT)) &&
	    (!(cmd->convert_src & (TRIG_TIMER | TRIG_NOW))))
		err |= -EINVAL;

	if ((cmd->scan_begin_src == TRIG_FOLLOW) &&
	    (!(cmd->convert_src & (TRIG_TIMER | TRIG_EXT))))
		err |= -EINVAL;

	if (cmd->stop_src == TRIG_EXT && cmd->scan_begin_src == TRIG_EXT)
		err |= -EINVAL;

	if (err)
		return 2;

	/* Step 3: check if arguments are trivially valid */

	switch (cmd->start_src) {
	case TRIG_NOW:
	case TRIG_EXT:
		err |= cfc_check_trigger_arg_is(&cmd->start_arg, 0);
		break;
	case TRIG_INT:
		/* start_arg is the internal trigger (any value) */
		break;
	}

	if (cmd->scan_begin_src & (TRIG_FOLLOW | TRIG_EXT))
		err |= cfc_check_trigger_arg_is(&cmd->scan_begin_arg, 0);

	if ((cmd->scan_begin_src == TRIG_TIMER) &&
	    (cmd->convert_src == TRIG_TIMER) && (cmd->scan_end_arg == 1)) {
		cmd->scan_begin_src = TRIG_FOLLOW;
		cmd->convert_arg = cmd->scan_begin_arg;
		cmd->scan_begin_arg = 0;
	}

	if (cmd->scan_begin_src == TRIG_TIMER)
		err |= cfc_check_trigger_arg_min(&cmd->scan_begin_arg,
						 this_board->ai_ns_min);

	if (cmd->scan_begin_src == TRIG_EXT)
		if (cmd->scan_begin_arg) {
			cmd->scan_begin_arg = 0;
			err |= -EINVAL;
			err |= cfc_check_trigger_arg_max(&cmd->scan_end_arg,
							 65535);
		}

	if (cmd->convert_src & (TRIG_TIMER | TRIG_NOW))
		err |= cfc_check_trigger_arg_min(&cmd->convert_arg,
						 this_board->ai_ns_min);

	if (cmd->convert_src == TRIG_EXT)
		err |= cfc_check_trigger_arg_is(&cmd->convert_arg, 0);

	if (cmd->stop_src == TRIG_COUNT)
		err |= cfc_check_trigger_arg_min(&cmd->stop_arg, 1);
	else	/* TRIG_NONE */
		err |= cfc_check_trigger_arg_is(&cmd->stop_arg, 0);

	err |= cfc_check_trigger_arg_min(&cmd->chanlist_len, 1);

	err |= cfc_check_trigger_arg_min(&cmd->scan_end_arg,
					 cmd->chanlist_len);

	if ((cmd->scan_end_arg % cmd->chanlist_len)) {
		cmd->scan_end_arg =
		    cmd->chanlist_len * (cmd->scan_end_arg / cmd->chanlist_len);
		err |= -EINVAL;
	}

	if (err)
		return 3;

	/* step 4: fix up any arguments */

	if (cmd->scan_begin_src == TRIG_TIMER) {
		arg = cmd->scan_begin_arg;
		i8253_cascade_ns_to_timer(I8254_OSC_BASE_4MHZ,
					  &divisor1, &divisor2,
					  &arg, cmd->flags);
		err |= cfc_check_trigger_arg_is(&cmd->scan_begin_arg, arg);
	}

	if (cmd->convert_src & (TRIG_TIMER | TRIG_NOW)) {
		arg = cmd->convert_arg;
		i8253_cascade_ns_to_timer(I8254_OSC_BASE_4MHZ,
					  &divisor1, &divisor2,
					  &arg, cmd->flags);
		err |= cfc_check_trigger_arg_is(&cmd->convert_arg, arg);

		if (cmd->scan_begin_src == TRIG_TIMER &&
		    cmd->convert_src == TRIG_NOW) {
			if (cmd->convert_arg == 0) {
				arg = this_board->ai_ns_min *
				      (cmd->scan_end_arg + 2);
			} else {
				arg = cmd->convert_arg * cmd->chanlist_len;
			}
			err |= cfc_check_trigger_arg_min(&cmd->scan_begin_arg,
							 arg);
		}
	}

	if (err)
		return 4;

	if (cmd->chanlist)
		if (!check_channel_list(dev, s, cmd->chanlist_len,
					cmd->chanlist, 0, 0))
			return 5;	/* incorrect channels list */

	return 0;
}

static int Compute_and_setup_dma(struct comedi_device *dev,
				 struct comedi_subdevice *s)
{
	struct pci9118_private *devpriv = dev->private;
	struct comedi_cmd *cmd = &s->async->cmd;
	unsigned int dmalen0, dmalen1, i;

	dmalen0 = devpriv->dmabuf_size[0];
	dmalen1 = devpriv->dmabuf_size[1];
	/* isn't output buff smaller that our DMA buff? */
	if (dmalen0 > s->async->prealloc_bufsz) {
		/* align to 32bit down */
		dmalen0 = s->async->prealloc_bufsz & ~3L;
	}
	if (dmalen1 > s->async->prealloc_bufsz) {
		/* align to 32bit down */
		dmalen1 = s->async->prealloc_bufsz & ~3L;
	}

	/* we want wake up every scan? */
	if (devpriv->ai_flags & TRIG_WAKE_EOS) {
		if (dmalen0 < (devpriv->ai_n_realscanlen << 1)) {
			/* uff, too short DMA buffer, disable EOS support! */
			devpriv->ai_flags &= (~TRIG_WAKE_EOS);
			dev_info(dev->class_dev,
				 "WAR: DMA0 buf too short, can't support TRIG_WAKE_EOS (%d<%d)\n",
				  dmalen0, devpriv->ai_n_realscanlen << 1);
		} else {
			/* short first DMA buffer to one scan */
			dmalen0 = devpriv->ai_n_realscanlen << 1;
			if (dmalen0 < 4) {
				dev_info(dev->class_dev,
					 "ERR: DMA0 buf len bug? (%d<4)\n",
					 dmalen0);
				dmalen0 = 4;
			}
		}
	}
	if (devpriv->ai_flags & TRIG_WAKE_EOS) {
		if (dmalen1 < (devpriv->ai_n_realscanlen << 1)) {
			/* uff, too short DMA buffer, disable EOS support! */
			devpriv->ai_flags &= (~TRIG_WAKE_EOS);
			dev_info(dev->class_dev,
				 "WAR: DMA1 buf too short, can't support TRIG_WAKE_EOS (%d<%d)\n",
				 dmalen1, devpriv->ai_n_realscanlen << 1);
		} else {
			/* short second DMA buffer to one scan */
			dmalen1 = devpriv->ai_n_realscanlen << 1;
			if (dmalen1 < 4) {
				dev_info(dev->class_dev,
					 "ERR: DMA1 buf len bug? (%d<4)\n",
					 dmalen1);
				dmalen1 = 4;
			}
		}
	}

	/* transfer without TRIG_WAKE_EOS */
	if (!(devpriv->ai_flags & TRIG_WAKE_EOS)) {
		/* if it's possible then align DMA buffers to length of scan */
		i = dmalen0;
		dmalen0 =
		    (dmalen0 / (devpriv->ai_n_realscanlen << 1)) *
		    (devpriv->ai_n_realscanlen << 1);
		dmalen0 &= ~3L;
		if (!dmalen0)
			dmalen0 = i;	/* uff. very long scan? */
		i = dmalen1;
		dmalen1 =
		    (dmalen1 / (devpriv->ai_n_realscanlen << 1)) *
		    (devpriv->ai_n_realscanlen << 1);
		dmalen1 &= ~3L;
		if (!dmalen1)
			dmalen1 = i;	/* uff. very long scan? */
		/*
		 * if measure isn't neverending then test, if it fits whole
		 * into one or two DMA buffers
		 */
		if (!devpriv->ai_neverending) {
			/* fits whole measure into one DMA buffer? */
			if (dmalen0 >
			    ((devpriv->ai_n_realscanlen << 1) *
			     cmd->stop_arg)) {
				dmalen0 =
				    (devpriv->ai_n_realscanlen << 1) *
				    cmd->stop_arg;
				dmalen0 &= ~3L;
			} else {	/*
					 * fits whole measure into
					 * two DMA buffer?
					 */
				if (dmalen1 >
				    ((devpriv->ai_n_realscanlen << 1) *
				     cmd->stop_arg - dmalen0))
					dmalen1 =
					    (devpriv->ai_n_realscanlen << 1) *
					    cmd->stop_arg - dmalen0;
				dmalen1 &= ~3L;
			}
		}
	}

	/* these DMA buffer size will be used */
	devpriv->dma_actbuf = 0;
	devpriv->dmabuf_use_size[0] = dmalen0;
	devpriv->dmabuf_use_size[1] = dmalen1;

#if 0
	if (cmd->scan_end_arg < this_board->half_fifo_size) {
		devpriv->dmabuf_panic_size[0] =
		    (this_board->half_fifo_size / cmd->scan_end_arg +
		     1) * cmd->scan_end_arg * sizeof(short);
		devpriv->dmabuf_panic_size[1] =
		    (this_board->half_fifo_size / cmd->scan_end_arg +
		     1) * cmd->scan_end_arg * sizeof(short);
	} else {
		devpriv->dmabuf_panic_size[0] =
		    (cmd->scan_end_arg << 1) % devpriv->dmabuf_size[0];
		devpriv->dmabuf_panic_size[1] =
		    (cmd->scan_end_arg << 1) % devpriv->dmabuf_size[1];
	}
#endif

	outl(inl(devpriv->iobase_a + AMCC_OP_REG_MCSR) & (~EN_A2P_TRANSFERS),
			devpriv->iobase_a + AMCC_OP_REG_MCSR);	/* stop DMA */
	outl(devpriv->dmabuf_hw[0], devpriv->iobase_a + AMCC_OP_REG_MWAR);
	outl(devpriv->dmabuf_use_size[0], devpriv->iobase_a + AMCC_OP_REG_MWTC);
	/* init DMA transfer */
	outl(0x00000000 | AINT_WRITE_COMPL,
	     devpriv->iobase_a + AMCC_OP_REG_INTCSR);
/* outl(0x02000000|AINT_WRITE_COMPL, devpriv->iobase_a+AMCC_OP_REG_INTCSR); */

	outl(inl(devpriv->iobase_a +
		 AMCC_OP_REG_MCSR) | RESET_A2P_FLAGS | A2P_HI_PRIORITY |
	     EN_A2P_TRANSFERS, devpriv->iobase_a + AMCC_OP_REG_MCSR);
	outl(inl(devpriv->iobase_a + AMCC_OP_REG_INTCSR) | EN_A2P_TRANSFERS,
			devpriv->iobase_a + AMCC_OP_REG_INTCSR);
						/* allow bus mastering */

	return 0;
}

static int pci9118_ai_docmd_sampl(struct comedi_device *dev,
				  struct comedi_subdevice *s)
{
	struct pci9118_private *devpriv = dev->private;

	switch (devpriv->ai_do) {
	case 1:
		devpriv->AdControlReg |= AdControl_TmrTr;
		break;
	case 2:
		dev_err(dev->class_dev, "%s mode 2 bug!\n", __func__);
		return -EIO;
	case 3:
		devpriv->AdControlReg |= AdControl_ExtM;
		break;
	case 4:
		dev_err(dev->class_dev, "%s mode 4 bug!\n", __func__);
		return -EIO;
	default:
		dev_err(dev->class_dev, "%s mode number bug!\n", __func__);
		return -EIO;
	}

	if (devpriv->ai12_startstop)
		pci9118_exttrg_add(dev, EXTTRG_AI);
						/* activate EXT trigger */

	if ((devpriv->ai_do == 1) || (devpriv->ai_do == 2))
		devpriv->IntControlReg |= Int_Timer;

	devpriv->AdControlReg |= AdControl_Int;

	outl(inl(devpriv->iobase_a + AMCC_OP_REG_INTCSR) | 0x1f00,
			devpriv->iobase_a + AMCC_OP_REG_INTCSR);
							/* allow INT in AMCC */

	if (!(devpriv->ai12_startstop & (START_AI_EXT | START_AI_INT))) {
		outl(devpriv->IntControlReg, dev->iobase + PCI9118_INTCTRL);
		outl(devpriv->AdFunctionReg, dev->iobase + PCI9118_ADFUNC);
		if (devpriv->ai_do != 3) {
			pci9118_start_pacer(dev, devpriv->ai_do);
			devpriv->AdControlReg |= AdControl_SoftG;
		}
		outl(devpriv->IntControlReg, dev->iobase + PCI9118_INTCTRL);
	}

	return 0;
}

static int pci9118_ai_docmd_dma(struct comedi_device *dev,
				struct comedi_subdevice *s)
{
	struct pci9118_private *devpriv = dev->private;
	struct comedi_cmd *cmd = &s->async->cmd;

	Compute_and_setup_dma(dev, s);

	switch (devpriv->ai_do) {
	case 1:
		devpriv->AdControlReg |=
		    ((AdControl_TmrTr | AdControl_Dma) & 0xff);
		break;
	case 2:
		devpriv->AdControlReg |=
		    ((AdControl_TmrTr | AdControl_Dma) & 0xff);
		devpriv->AdFunctionReg =
		    AdFunction_PDTrg | AdFunction_PETrg | AdFunction_BM |
		    AdFunction_BS;
		if (cmd->convert_src == TRIG_NOW && !devpriv->softsshdelay)
			devpriv->AdFunctionReg |= AdFunction_BSSH;
		outl(devpriv->ai_n_realscanlen, dev->iobase + PCI9118_BURST);
		break;
	case 3:
		devpriv->AdControlReg |=
		    ((AdControl_ExtM | AdControl_Dma) & 0xff);
		devpriv->AdFunctionReg = AdFunction_PDTrg | AdFunction_PETrg;
		break;
	case 4:
		devpriv->AdControlReg |=
		    ((AdControl_TmrTr | AdControl_Dma) & 0xff);
		devpriv->AdFunctionReg =
		    AdFunction_PDTrg | AdFunction_PETrg | AdFunction_AM;
		outl(devpriv->AdFunctionReg, dev->iobase + PCI9118_ADFUNC);
		outl(0x30, dev->iobase + PCI9118_CNTCTRL);
		outl((devpriv->dmabuf_hw[0] >> 1) & 0xff,
		     dev->iobase + PCI9118_CNT0);
		outl((devpriv->dmabuf_hw[0] >> 9) & 0xff,
		     dev->iobase + PCI9118_CNT0);
		devpriv->AdFunctionReg |= AdFunction_Start;
		break;
	default:
		dev_err(dev->class_dev, "%s mode number bug!\n", __func__);
		return -EIO;
	}

	if (devpriv->ai12_startstop) {
		pci9118_exttrg_add(dev, EXTTRG_AI);
						/* activate EXT trigger */
	}

	outl(0x02000000 | AINT_WRITE_COMPL,
	     devpriv->iobase_a + AMCC_OP_REG_INTCSR);

	if (!(devpriv->ai12_startstop & (START_AI_EXT | START_AI_INT))) {
		outl(devpriv->AdFunctionReg, dev->iobase + PCI9118_ADFUNC);
		outl(devpriv->IntControlReg, dev->iobase + PCI9118_INTCTRL);
		if (devpriv->ai_do != 3) {
			pci9118_start_pacer(dev, devpriv->ai_do);
			devpriv->AdControlReg |= AdControl_SoftG;
		}
		outl(devpriv->AdControlReg, dev->iobase + PCI9118_ADCNTRL);
	}

	return 0;
}

static int pci9118_ai_cmd(struct comedi_device *dev, struct comedi_subdevice *s)
{
	const struct boardtype *this_board = comedi_board(dev);
	struct pci9118_private *devpriv = dev->private;
	struct comedi_cmd *cmd = &s->async->cmd;
	unsigned int addchans = 0;
	int ret = 0;

	devpriv->ai12_startstop = 0;
	devpriv->ai_flags = cmd->flags;
	devpriv->ai_add_front = 0;
	devpriv->ai_add_back = 0;
	devpriv->ai_maskerr = 0x10e;

	/* prepare for start/stop conditions */
	if (cmd->start_src == TRIG_EXT)
		devpriv->ai12_startstop |= START_AI_EXT;
	if (cmd->stop_src == TRIG_EXT) {
		devpriv->ai_neverending = 1;
		devpriv->ai12_startstop |= STOP_AI_EXT;
	}
	if (cmd->start_src == TRIG_INT) {
		devpriv->ai12_startstop |= START_AI_INT;
		s->async->inttrig = pci9118_ai_inttrig;
	}
	if (cmd->stop_src == TRIG_NONE)
		devpriv->ai_neverending = 1;
	if (cmd->stop_src == TRIG_COUNT)
		devpriv->ai_neverending = 0;

	/*
	 * use additional sample at end of every scan
	 * to satisty DMA 32 bit transfer?
	 */
	devpriv->ai_add_front = 0;
	devpriv->ai_add_back = 0;
	if (devpriv->master) {
		devpriv->usedma = 1;
		if ((cmd->flags & TRIG_WAKE_EOS) &&
		    (cmd->scan_end_arg == 1)) {
			if (cmd->convert_src == TRIG_NOW)
				devpriv->ai_add_back = 1;
			if (cmd->convert_src == TRIG_TIMER) {
				devpriv->usedma = 0;
					/*
					 * use INT transfer if scanlist
					 * have only one channel
					 */
			}
		}
		if ((cmd->flags & TRIG_WAKE_EOS) &&
		    (cmd->scan_end_arg & 1) &&
		    (cmd->scan_end_arg > 1)) {
			if (cmd->scan_begin_src == TRIG_FOLLOW) {
				devpriv->usedma = 0;
				/*
				 * XXX maybe can be corrected to use 16 bit DMA
				 */
			} else {	/*
					 * well, we must insert one sample
					 * to end of EOS to meet 32 bit transfer
					 */
				devpriv->ai_add_back = 1;
			}
		}
	} else {	/* interrupt transfer don't need any correction */
		devpriv->usedma = 0;
	}

	/*
	 * we need software S&H signal?
	 * It adds two samples before every scan as minimum
	 */
	if (cmd->convert_src == TRIG_NOW && devpriv->softsshdelay) {
		devpriv->ai_add_front = 2;
		if ((devpriv->usedma == 1) && (devpriv->ai_add_back == 1)) {
							/* move it to front */
			devpriv->ai_add_front++;
			devpriv->ai_add_back = 0;
		}
		if (cmd->convert_arg < this_board->ai_ns_min)
			cmd->convert_arg = this_board->ai_ns_min;
		addchans = devpriv->softsshdelay / cmd->convert_arg;
		if (devpriv->softsshdelay % cmd->convert_arg)
			addchans++;
		if (addchans > (devpriv->ai_add_front - 1)) {
							/* uff, still short */
			devpriv->ai_add_front = addchans + 1;
			if (devpriv->usedma == 1)
				if ((devpriv->ai_add_front +
				     cmd->chanlist_len +
				     devpriv->ai_add_back) & 1)
					devpriv->ai_add_front++;
							/* round up to 32 bit */
		}
	}
	/* well, we now know what must be all added */
	devpriv->ai_n_realscanlen =	/*
					 * what we must take from card in real
					 * to have cmd->scan_end_arg on output?
					 */
	    (devpriv->ai_add_front + cmd->chanlist_len +
	     devpriv->ai_add_back) * (cmd->scan_end_arg /
				      cmd->chanlist_len);

	/* check and setup channel list */
	if (!check_channel_list(dev, s, cmd->chanlist_len,
				cmd->chanlist, devpriv->ai_add_front,
				devpriv->ai_add_back))
		return -EINVAL;
	if (!setup_channel_list(dev, s, cmd->chanlist_len,
				cmd->chanlist, 0, devpriv->ai_add_front,
				devpriv->ai_add_back, devpriv->usedma))
		return -EINVAL;

	/* compute timers settings */
	/*
	 * simplest way, fr=4Mhz/(tim1*tim2),
	 * channel manipulation without timers effect
	 */
	if (((cmd->scan_begin_src == TRIG_FOLLOW) ||
		(cmd->scan_begin_src == TRIG_EXT) ||
		(cmd->scan_begin_src == TRIG_INT)) &&
		(cmd->convert_src == TRIG_TIMER)) {
					/* both timer is used for one time */
		if (cmd->scan_begin_src == TRIG_EXT)
			devpriv->ai_do = 4;
		else
			devpriv->ai_do = 1;
		pci9118_calc_divisors(devpriv->ai_do, dev, s,
				      &cmd->scan_begin_arg, &cmd->convert_arg,
				      devpriv->ai_flags,
				      devpriv->ai_n_realscanlen,
				      &devpriv->ai_divisor1,
				      &devpriv->ai_divisor2,
				      devpriv->ai_add_front);
	}

	if ((cmd->scan_begin_src == TRIG_TIMER) &&
		((cmd->convert_src == TRIG_TIMER) ||
		(cmd->convert_src == TRIG_NOW))) {
						/* double timed action */
		if (!devpriv->usedma) {
			dev_err(dev->class_dev,
				"cmd->scan_begin_src=TRIG_TIMER works only with bus mastering!\n");
			return -EIO;
		}

		devpriv->ai_do = 2;
		pci9118_calc_divisors(devpriv->ai_do, dev, s,
				      &cmd->scan_begin_arg, &cmd->convert_arg,
				      devpriv->ai_flags,
				      devpriv->ai_n_realscanlen,
				      &devpriv->ai_divisor1,
				      &devpriv->ai_divisor2,
				      devpriv->ai_add_front);
	}

	if ((cmd->scan_begin_src == TRIG_FOLLOW)
	    && (cmd->convert_src == TRIG_EXT)) {
		devpriv->ai_do = 3;
	}

	pci9118_start_pacer(dev, -1);	/* stop pacer */

	devpriv->AdControlReg = 0;	/*
					 * bipolar, S.E., use 8254, stop 8354,
					 * internal trigger, soft trigger,
					 * disable DMA
					 */
	outl(devpriv->AdControlReg, dev->iobase + PCI9118_ADCNTRL);
	devpriv->AdFunctionReg = AdFunction_PDTrg | AdFunction_PETrg;
					/*
					 * positive triggers, no S&H, no burst,
					 * burst stop, no post trigger,
					 * no about trigger, trigger stop
					 */
	outl(devpriv->AdFunctionReg, dev->iobase + PCI9118_ADFUNC);
	udelay(1);
	outl(0, dev->iobase + PCI9118_DELFIFO);	/* flush FIFO */
	inl(dev->iobase + PCI9118_ADSTAT);	/*
						 * flush A/D and INT
						 * status register
						 */
	inl(dev->iobase + PCI9118_INTSRC);

	devpriv->ai_act_scan = 0;
	devpriv->ai_act_dmapos = 0;
	s->async->cur_chan = 0;

	if (devpriv->usedma)
		ret = pci9118_ai_docmd_dma(dev, s);
	else
		ret = pci9118_ai_docmd_sampl(dev, s);

	return ret;
}

static int pci9118_reset(struct comedi_device *dev)
{
	struct pci9118_private *devpriv = dev->private;

	devpriv->IntControlReg = 0;
	devpriv->exttrg_users = 0;
	inl(dev->iobase + PCI9118_INTCTRL);
	outl(devpriv->IntControlReg, dev->iobase + PCI9118_INTCTRL);
						/* disable interrupts source */
	outl(0x30, dev->iobase + PCI9118_CNTCTRL);
/* outl(0xb4, dev->iobase + PCI9118_CNTCTRL); */
	pci9118_start_pacer(dev, 0);		/* stop 8254 counters */
	devpriv->AdControlReg = 0;
	outl(devpriv->AdControlReg, dev->iobase + PCI9118_ADCNTRL);
						/*
						 * bipolar, S.E., use 8254,
						 * stop 8354, internal trigger,
						 * soft trigger,
						 * disable INT and DMA
						 */
	outl(0, dev->iobase + PCI9118_BURST);
	outl(1, dev->iobase + PCI9118_SCANMOD);
	outl(2, dev->iobase + PCI9118_SCANMOD);	/* reset scan queue */
	devpriv->AdFunctionReg = AdFunction_PDTrg | AdFunction_PETrg;
	outl(devpriv->AdFunctionReg, dev->iobase + PCI9118_ADFUNC);
						/*
						 * positive triggers, no S&H,
						 * no burst, burst stop,
						 * no post trigger,
						 * no about trigger,
						 * trigger stop
						 */

	devpriv->ao_data[0] = 2047;
	devpriv->ao_data[1] = 2047;
	outl(devpriv->ao_data[0], dev->iobase + PCI9118_DA1);
						/* reset A/D outs to 0V */
	outl(devpriv->ao_data[1], dev->iobase + PCI9118_DA2);
	outl(0, dev->iobase + PCI9118_DO);	/* reset digi outs to L */
	udelay(10);
	inl(dev->iobase + PCI9118_AD_DATA);
	outl(0, dev->iobase + PCI9118_DELFIFO);	/* flush FIFO */
	outl(0, dev->iobase + PCI9118_INTSRC);	/* remove INT requests */
	inl(dev->iobase + PCI9118_ADSTAT);	/* flush A/D status register */
	inl(dev->iobase + PCI9118_INTSRC);	/* flush INT requests */
	devpriv->AdControlReg = 0;
	outl(devpriv->AdControlReg, dev->iobase + PCI9118_ADCNTRL);
						/*
						 * bipolar, S.E., use 8254,
						 * stop 8354, internal trigger,
						 * soft trigger,
						 * disable INT and DMA
						 */

	devpriv->exttrg_users = 0;

	return 0;
}

/*
 * FIXME - this is pretty ineffective because all the supported board types
 * have the same device ID!
 */
static const struct boardtype *pci9118_find_boardinfo(struct pci_dev *pcidev)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(boardtypes); i++)
		if (pcidev->device == boardtypes[i].device_id)
			return &boardtypes[i];
	return NULL;
}

static struct pci_dev *pci9118_find_pci(struct comedi_device *dev,
					struct comedi_devconfig *it)
{
	const struct boardtype *this_board = comedi_board(dev);
	struct pci_dev *pcidev = NULL;
	int bus = it->options[0];
	int slot = it->options[1];

	for_each_pci_dev(pcidev) {
		if (pcidev->vendor != PCI_VENDOR_ID_AMCC)
			continue;
		if (pcidev->device != this_board->device_id)
			continue;
		if (bus || slot) {
			/* requested particular bus/slot */
			if (pcidev->bus->number != bus ||
			    PCI_SLOT(pcidev->devfn) != slot)
				continue;
		}
		return pcidev;
	}
	dev_err(dev->class_dev,
		"no supported board found! (req. bus/slot : %d/%d)\n",
		bus, slot);
	return NULL;
}

static int pci9118_common_attach(struct comedi_device *dev, int disable_irq,
				 int master, int ext_mux, int softsshdelay,
				 int hw_err_mask)
{
	const struct boardtype *this_board = comedi_board(dev);
	struct pci9118_private *devpriv = dev->private;
	struct pci_dev *pcidev = comedi_to_pci_dev(dev);
	struct comedi_subdevice *s;
	int ret, pages, i;
	u16 u16w;

	dev->board_name = this_board->name;
	ret = comedi_pci_enable(dev);
	if (ret)
		return ret;
	if (master)
		pci_set_master(pcidev);

	devpriv->iobase_a = pci_resource_start(pcidev, 0);
	dev->iobase = pci_resource_start(pcidev, 2);

	pci9118_reset(dev);

	if (master) {		/* alloc DMA buffers */
		devpriv->dma_doublebuf = 0;
		for (i = 0; i < 2; i++) {
			for (pages = 4; pages >= 0; pages--) {
				devpriv->dmabuf_virt[i] =
				    (unsigned short *)
				    __get_free_pages(GFP_KERNEL, pages);
				if (devpriv->dmabuf_virt[i])
					break;
			}
			if (devpriv->dmabuf_virt[i]) {
				devpriv->dmabuf_pages[i] = pages;
				devpriv->dmabuf_size[i] = PAGE_SIZE * pages;
				devpriv->dmabuf_hw[i] =
				    virt_to_bus((void *)
						devpriv->dmabuf_virt[i]);
			}
		}
		if (!devpriv->dmabuf_virt[0]) {
			dev_warn(dev->class_dev,
				 "Can't allocate DMA buffer, DMA disabled!\n");
			master = 0;
		}
		if (devpriv->dmabuf_virt[1])
			devpriv->dma_doublebuf = 1;
	}
	devpriv->master = master;

	if (ext_mux > 0) {
		if (ext_mux > 256)
			ext_mux = 256;	/* max 256 channels! */
		if (softsshdelay > 0)
			if (ext_mux > 128)
				ext_mux = 128;
		devpriv->usemux = ext_mux;
	} else {
		devpriv->usemux = 0;
	}

	if (softsshdelay < 0) {
		/* select sample&hold signal polarity */
		devpriv->softsshdelay = -softsshdelay;
		devpriv->softsshsample = 0x80;
		devpriv->softsshhold = 0x00;
	} else {
		devpriv->softsshdelay = softsshdelay;
		devpriv->softsshsample = 0x00;
		devpriv->softsshhold = 0x80;
	}

	pci_read_config_word(pcidev, PCI_COMMAND, &u16w);
	pci_write_config_word(pcidev, PCI_COMMAND, u16w | 64);
				/* Enable parity check for parity error */

	if (!disable_irq && pcidev->irq) {
		ret = request_irq(pcidev->irq, pci9118_interrupt, IRQF_SHARED,
				  dev->board_name, dev);
		if (ret == 0)
			dev->irq = pcidev->irq;
	}

	ret = comedi_alloc_subdevices(dev, 4);
	if (ret)
		return ret;

	s = &dev->subdevices[0];
	s->type = COMEDI_SUBD_AI;
	s->subdev_flags = SDF_READABLE | SDF_COMMON | SDF_GROUND | SDF_DIFF;
	if (devpriv->usemux)
		s->n_chan = devpriv->usemux;
	else
		s->n_chan = this_board->n_aichan;

	s->maxdata = this_board->ai_maxdata;
	s->range_table = this_board->rangelist_ai;
	s->insn_read = pci9118_insn_read_ai;
	if (dev->irq) {
		dev->read_subdev = s;
		s->subdev_flags |= SDF_CMD_READ;
		s->len_chanlist = this_board->n_aichanlist;
		s->do_cmdtest = pci9118_ai_cmdtest;
		s->do_cmd = pci9118_ai_cmd;
		s->cancel = pci9118_ai_cancel;
		s->munge = pci9118_ai_munge;
	}

	s = &dev->subdevices[1];
	s->type = COMEDI_SUBD_AO;
	s->subdev_flags = SDF_WRITABLE | SDF_GROUND | SDF_COMMON;
	s->n_chan = this_board->n_aochan;
	s->maxdata = this_board->ao_maxdata;
	s->len_chanlist = this_board->n_aochan;
	s->range_table = this_board->rangelist_ao;
	s->insn_write = pci9118_insn_write_ao;
	s->insn_read = pci9118_insn_read_ao;

	s = &dev->subdevices[2];
	s->type = COMEDI_SUBD_DI;
	s->subdev_flags = SDF_READABLE | SDF_GROUND | SDF_COMMON;
	s->n_chan = 4;
	s->maxdata = 1;
	s->len_chanlist = 4;
	s->range_table = &range_digital;
	s->insn_bits = pci9118_insn_bits_di;

	s = &dev->subdevices[3];
	s->type = COMEDI_SUBD_DO;
	s->subdev_flags = SDF_WRITABLE | SDF_GROUND | SDF_COMMON;
	s->n_chan = 4;
	s->maxdata = 1;
	s->len_chanlist = 4;
	s->range_table = &range_digital;
	s->insn_bits = pci9118_insn_bits_do;

	devpriv->ai_maskharderr = 0x10a;
					/* default measure crash condition */
	if (hw_err_mask)		/* disable some requested */
		devpriv->ai_maskharderr &= ~hw_err_mask;

	return 0;
}

static int pci9118_attach(struct comedi_device *dev,
			  struct comedi_devconfig *it)
{
	struct pci9118_private *devpriv;
	struct pci_dev *pcidev;
	int ext_mux, disable_irq, master, softsshdelay, hw_err_mask;

	ext_mux = it->options[2];
	master = ((it->options[3] & 1) == 0);
	disable_irq = ((it->options[3] & 2) != 0);
	softsshdelay = it->options[4];
	hw_err_mask = it->options[5];

	devpriv = comedi_alloc_devpriv(dev, sizeof(*devpriv));
	if (!devpriv)
		return -ENOMEM;

	pcidev = pci9118_find_pci(dev, it);
	if (!pcidev)
		return -EIO;
	comedi_set_hw_dev(dev, &pcidev->dev);

	return pci9118_common_attach(dev, disable_irq, master, ext_mux,
				     softsshdelay, hw_err_mask);
}

static int pci9118_auto_attach(struct comedi_device *dev,
					 unsigned long context_unused)
{
	struct pci_dev *pcidev = comedi_to_pci_dev(dev);
	struct pci9118_private *devpriv;

	devpriv = comedi_alloc_devpriv(dev, sizeof(*devpriv));
	if (!devpriv)
		return -ENOMEM;

	dev->board_ptr = pci9118_find_boardinfo(pcidev);
	if (dev->board_ptr == NULL) {
		dev_err(dev->class_dev,
			"adl_pci9118: cannot determine board type for pci %s\n",
			pci_name(pcidev));
		return -EINVAL;
	}
	/*
	 * Need to 'get' the PCI device to match the 'put' in pci9118_detach().
	 * (The 'put' also matches the implicit 'get' by pci9118_find_pci().)
	 */
	pci_dev_get(pcidev);
	/* Don't disable irq, use bus master, no external mux,
	 * no sample-hold delay, no error mask. */
	return pci9118_common_attach(dev, 0, 1, 0, 0, 0);
}

static void pci9118_detach(struct comedi_device *dev)
{
	struct pci_dev *pcidev = comedi_to_pci_dev(dev);
	struct pci9118_private *devpriv = dev->private;

	if (devpriv) {
		if (dev->iobase)
			pci9118_reset(dev);
		if (dev->irq)
			free_irq(dev->irq, dev);
		if (devpriv->dmabuf_virt[0])
			free_pages((unsigned long)devpriv->dmabuf_virt[0],
				   devpriv->dmabuf_pages[0]);
		if (devpriv->dmabuf_virt[1])
			free_pages((unsigned long)devpriv->dmabuf_virt[1],
				   devpriv->dmabuf_pages[1]);
	}
	comedi_pci_disable(dev);
	if (pcidev)
		pci_dev_put(pcidev);
}

static struct comedi_driver adl_pci9118_driver = {
	.driver_name	= "adl_pci9118",
	.module		= THIS_MODULE,
	.attach		= pci9118_attach,
	.auto_attach	= pci9118_auto_attach,
	.detach		= pci9118_detach,
	.num_names	= ARRAY_SIZE(boardtypes),
	.board_name	= &boardtypes[0].name,
	.offset		= sizeof(struct boardtype),
};

static int adl_pci9118_pci_probe(struct pci_dev *dev,
				 const struct pci_device_id *id)
{
	return comedi_pci_auto_config(dev, &adl_pci9118_driver,
				      id->driver_data);
}

static const struct pci_device_id adl_pci9118_pci_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_AMCC, 0x80d9) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, adl_pci9118_pci_table);

static struct pci_driver adl_pci9118_pci_driver = {
	.name		= "adl_pci9118",
	.id_table	= adl_pci9118_pci_table,
	.probe		= adl_pci9118_pci_probe,
	.remove		= comedi_pci_auto_unconfig,
};
module_comedi_pci_driver(adl_pci9118_driver, adl_pci9118_pci_driver);

MODULE_AUTHOR("Comedi http://www.comedi.org");
MODULE_DESCRIPTION("Comedi low-level driver");
MODULE_LICENSE("GPL");
