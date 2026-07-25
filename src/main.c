/*
 * es1371snd - BTRON (超漢字 / B-right/V) kernel sound driver for the
 *             Ensoniq ES1371 / Creative AudioPCI — the audio device VMware
 *             emulates by default.
 *
 * Supplies the "サウンド" (sound) device: an application opens it with
 *   opn_dev("サウンド", D_UPDATE) and plays 16-bit/48kHz/stereo PCM with
 *   wri_dev().
 *
 * Structure: the BTRON kerext device-driver scaffolding (kerext entry,
 * rendezvous port + main task, b_DefDevice registration, the DC_ and DN_
 * request dispatch, GetMemoryForDMA, defIntHdr) is adapted from the
 * intelsounddrv reference (GPL2). Only the HARDWARE layer differs: the ICH
 * AC97 (NAMBAR/NABMBAR + 32-entry SGD buffer-descriptor list) is replaced by
 * the ES1371 register set (single BAR0 I/O space, the on-chip Sample Rate
 * Converter, the AC97 codec behind the SRC, and the DAC2 single-frame DMA
 * engine). The ES1371 register names/sequences are ported from the OpenBSD
 * eap(4) driver (eapreg.h / eap.c, eap1371_* paths).
 *
 * Build with the brightv DRIVER (kerext) toolchain, install to /SYS and start
 * with `kerext es1371snd`. Re-enabling the DEBUG_PRINT trace below logs every
 * bring-up stage to the system console (PCI probe -> I/O space -> reset ->
 * codec -> SRC -> DMA -> interrupt).
 *
 * Copyright (C) 2026 satromi
 * Portions Copyright (C) 2004 Yurio Miyazawa
 *   (the intelsounddrv kerext scaffolding, GPL-2.0)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation. It is distributed in the hope that it
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * file LICENSE for the full licence text.
 *
 * The ES1371 hardware layer is derived from the OpenBSD/NetBSD eap(4)
 * driver, distributed under the following BSD licence:
 *
 *   Copyright (c) 1998, 1999 The NetBSD Foundation, Inc.
 *   All rights reserved.
 *
 *   This code is derived from software contributed to The NetBSD Foundation
 *   by Lennart Augustsson (augustss@NetBSD.org) and Charles M. Hannum.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND
 *   CONTRIBUTORS ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING,
 *   BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 *   FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *   FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *   INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *   BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *   OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *   ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *   TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *   USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <basic.h>
#include <inner.h>
#include <bstdlib.h>
#include <errcode.h>
#include <bsys/util.h>
#include <bsys/cons_io.h>
#include <bsys/cio_svc.h>
#include <util/debug.h>
#include <bstring.h>
#include <tstring.h>
#include <tctype.h>
#include <kernel/util.h>
#include <kernel/segment.h>
#include <driver/driver.h>
#include <driver/hwres.h>
#include <driver/pcat/sys.h>
#include <driver/pcat/pci.h>
#include <btron/memory.h>
#include <device/sound.h>

/* Console logging disabled: the driver is past bring-up, and the DAC2
 * started/stopped trace spams the console on every play/stop. Override the
 * <util/debug.h> DEBUG_PRINT with a no-op (call sites kept so the trace can be
 * restored by removing these two lines). */
#undef  DEBUG_PRINT
#define DEBUG_PRINT(x)  ((void)0)

/* ------------------------------------------------------------------ utils */

#define toERR(class, detail)  ((ERR)((class) << 16 | (UH)(detail)))
#define PRINT_WORD(v)         DEBUG_PRINT(("%30s = %d\n", #v, (W)(v)))
#define PRINT_HEX(v)          DEBUG_PRINT(("%30s = 0x%08x\n", #v, (W)(v)))

/* --------------------------------------------------- ES1371 register map */

#define ES_PCI_VENDOR   0x1274        /* Ensoniq                              */
#define ES_PCI_DEV_1371 0x1371        /* ES1371 / Creative AudioPCI (VMware)  */
#define ES_PCI_DEV_5880 0x5880        /* CT5880 (same programming)            */

#define ES_ICSC         0x00          /* Interrupt/Chip Select Control        */
/* NOTE: ES1371 bit0/bit1 differ from the ES1370. bit0 = PCICLKDIS (NOT the
 * ES1370 "SERR_DISABLE"), bit1 = XTALCKDIS. Power-on/reset value is 0 (both
 * clocks enabled), which is also the correct value to program at init. bit14
 * SYNC_RES pulses an AC97 warm reset. */
#define  ICSC_PCICLKDIS 0x00000001    /* bit0 (ES1371)                        */
#define  ICSC_XTALCKDIS 0x00000002    /* bit1 (ES1371)                        */
#define  ICSC_SYNC_RES  0x00004000    /* bit14: pulse 1 then 0 = AC97 warm reset */
#define  ICSC_ADC_EN    0x00000010
#define  ICSC_DAC2_EN   0x00000020
#define  ICSC_DAC1_EN   0x00000040
#define  ICSC_BREQ      0x00000080
#define  ICSC_PDLEV     0x00000300    /* bits9:8 power-down level; ISR writes = PowerState to clear MPWR */
#define  ICSC_SET_PCLKDIV(n) (((n) & 0x1fff) << 16)
#define ES_ICSS         0x04          /* Interrupt/Chip Select Status         */
#define  ICSS_I_DAC2    0x00000002    /* DAC2 (PCM playback) interrupt        */
#define  ICSS_INTR      0x80000000
#define ES_MEMPAGE      0x0c          /* selects the paged frame regs 0x30..  */
#define  MEMPAGE_DAC    0x0c
#define ES_SRC          0x10          /* Sample Rate Converter interface      */
#define  SRC_RAMWE      0x01000000
#define  SRC_RBUSY      0x00800000
#define  SRC_DISABLE    0x00400000
#define  SRC_DISP1      0x00200000
#define  SRC_DISP2      0x00100000
#define  SRC_DISREC     0x00080000
#define  SRC_CTLMASK    (SRC_DISABLE|SRC_DISP1|SRC_DISP2|SRC_DISREC)
#define  SRC_ADDR(a)    ((UW)(a) << 25)
#define  SRC_DATA(d)    ((UW)(d) & 0xffff)
#define  SRC_STATE_MASK 0x00870000
#define  SRC_STATE_OK   0x00010000
/* SRC RAM word addresses */
#define  ESRC_DAC1      0x74
#define  ESRC_DAC2      0x70
#define  ESRC_ADC       0x78
#define  ESRC_DAC1_VOLL 0x7c
#define  ESRC_DAC1_VOLR 0x7d
#define  ESRC_DAC2_VOLL 0x7e
#define  ESRC_DAC2_VOLR 0x7f
#define  ESRC_ADC_VOLL  0x6c
#define  ESRC_ADC_VOLR  0x6d
#define   ESRC_TRUNC_N  0x00
#define   ESRC_IREGS    0x01
#define   ESRC_ACF      0x02
#define   ESRC_VFF      0x03
#define  ESRC_SET_N(n)      ((n) << 4)
#define  ESRC_SET_VFI(n)    ((n) << 10)
#define  ESRC_SET_DAC_VOLI(n) ((n) << 12)
#define  ESRC_SET_ADC_VOL(n)  ((n) << 8)
#define ES_CODEC        0x14          /* AC97 codec read/write                */
#define  CODEC_VALID    0x80000000
#define  CODEC_WIP      0x40000000
#define  CODEC_READ     0x00800000
#define  CODEC_WCMD(a,d) (((UW)(a) << 16) | (UH)(d))
#define ES_SIC          0x20          /* Serial Interface Control             */
#define  SIC_P2_S_MB    0x00000004    /* DAC2 stereo                          */
#define  SIC_P2_S_EB    0x00000008    /* DAC2 16-bit                          */
#define  SIC_P2_DAC_SEN 0x00000040
#define  SIC_P2_INTR_EN 0x00000200    /* DAC2 interrupt enable                */
#define  SIC_P2_PAUSE   0x00001000
#define  SIC_P2_LOOP_SEL 0x00004000
#define  SIC_SET_P2_ST_INC(i)  ((i) << 16)
#define  SIC_SET_P2_END_INC(i) ((i) << 19)
#define  SIC_INC_BITS   0x003f0000
#define ES_DAC2_CSR     0x28          /* DAC2 sample count (interrupt / curr) */
#define  GET_CURRSAMP(r) ((r) >> 16)
/* paged (MEMPAGE=0x0c) frame registers */
#define ES_DAC2_ADDR    0x38          /* DAC2 PCI frame address (physical)    */
#define ES_DAC2_SIZE    0x3c          /* DAC2 frame size: (curr<<16)|(dwords-1)*/
#define  SET_SIZE(c,s)  (((UW)(c) << 16) | (UW)(s))

#define ES_RW_TIMEOUT   5000

/* AC97 codec registers (standard) */
#define AC97_RESET      0x00
#define AC97_MASTER_VOL 0x02
#define AC97_PCMOUT_VOL 0x18
#define AC97_POWER      0x26
#define AC97_EXT_ID     0x28
#define AC97_EXT_CTRL   0x2a
#define AC97_PCM_DAC_RATE 0x2c

/* --------------------------------------------------------- driver state */

typedef struct {
	PRI     priority;
	ID      deviceID;
	ID      acceptingPort;
	DevReq *deviceRequest;
	Bool    opened;
	Bool    suspended;
	W       openCnt;            /* concurrent openers (DC_OPEN..DC_CLOSE pairs) */
} DriverInfo;

static W   pciConfigAddr;
static W   ioBase;                    /* ES1371 BAR0 I/O base port            */
static W   irq;
static PRI taskPriority = 25;

/* DAC2 DMA ring: one physically-contiguous buffer the chip reads continuously
 * (DAC2 loops over [ADDR, ADDR+SIZE)). We split it into DMA_PARTS sub-buffers and
 * interrupt at each boundary, refilling the just-finished sub-buffer from the
 * application FIFO. Using 4 quarters instead of 2 halves: (a) the play position
 * is frozen under VMware so the refill phase is open-loop -- a single missed/
 * doubled IRQ would otherwise invert the parity forever and write the live half
 * every time (a fixed ~23Hz seam that survives a full FIFO); with 4 parts a
 * +/-1 slip leaves a ~32ms guard band and self-heals within one loop, and any
 * residual boundary tear is 1/4 the size. */
#define DMA_FRAMES   4096                         /* stereo 16-bit frames (16KB ring).
                                                   * 32KB needs 8 physically-CONTIGUOUS
                                                   * pages which b_get_mbk cannot grant
                                                   * here (0xfff80000) -- 4 pages (16KB)
                                                   * is the proven ceiling.            */
#define DMA_BYTES    (DMA_FRAMES * 4)             /* 4 bytes/frame            */
#define DMA_PARTS    4                            /* sub-buffers (quarters)   */
#define DMA_PART_BYTES (DMA_BYTES / DMA_PARTS)    /* 4KB per IRQ refill       */
static UB *dmaBuffer        = NULL;               /* logical (CPU) address    */
static UW  dmaBufferPhys    = 0;                  /* physical (for the chip)  */
static W   dmaPartFrames    = DMA_FRAMES / DMA_PARTS;

/* ---- REFILL_BY_TIMER: interrupt-free playback (preventive freeze fix) ---- */
/* The VMware freeze is a level-triggered PCI INTx storm: the DAC2 interrupt's
 * ACK may not clear on VMware's ES1371 model, so the ISR re-enters forever =
 * ring-0 livelock. Structural cure: DISABLE the DAC2 interrupt entirely and
 * refill the DAC2 ring from a periodic CYCLIC handler instead, clocked by wall
 * time (SYSTIME) so it self-corrects even though VMware freezes the play-position
 * registers. With no enabled DAC2 interrupt source the storm cannot happen at
 * all. Default OFF = the proven IRQ + total-IRQ-watchdog path; rebuild with
 * `make ... -DREFILL_BY_TIMER=1` (or set it here) to A/B on-device. */
#ifndef REFILL_BY_TIMER
#define REFILL_BY_TIMER 0
#endif
#if REFILL_BY_TIMER
#define CYC_INTERVAL_MS  10               /* cyclic tick period (ms)              */
static ID       cycId    = -1;            /* vdef_cyc handler id                  */
static UW       swWrite  = 0;             /* software ring write cursor (bytes)   */
static SYSTIME  lastFill = 0;             /* wall time of the last refill (ms)    */
static volatile Bool refillOn = False;    /* gate: refill only while PCM_PLAY     */
static void cyclicRefill(void);           /* fwd: defined after mixInto           */
#endif
static W   dmaLastHalf      = -1;                 /* index of last sub-buffer filled */

/* application PCM FIFO (16-bit stereo, 48kHz). wri_dev() appends here; the
 * interrupt handler drains it into the DAC2 ring. Silence is played when it
 * underruns. */
#define FIFO_BYTES   (48000 * 2)                  /* ~0.5s per channel (Kcalloc heap) */
#define MIX_CHANNELS 4                            /* concurrent streams mixed together */

/* One SPSC PCM ring PER OPENER, keyed by DevReq.taskid -- the only stable per-app
 * discriminator BTRON gives a driver (devid is the DEVICE id, identical for every
 * opener). The owning app's write path is the sole producer (advances head); the
 * DAC2 refill (mixInto) is the sole consumer (advances tail). Aligned 32-bit
 * head/tail are atomic on i386, so the SPSC pair needs no lock; only claim/free of
 * a slot is DI-guarded so the ISR never iterates a half-initialised channel. */
typedef struct {
	Bool inUse;                 /* claimed by an opener                        */
	ID   owner;                 /* DevReq.taskid that owns this channel        */
	UB  *buf;                   /* Kcalloc'd FIFO_BYTES ring (heap, NOT DMA)    */
	volatile W head;            /* producer (app write) writes                 */
	volatile W tail;            /* consumer (mixInto) reads                     */
	H    lastL, lastR;          /* last frame, for per-channel underrun DC-hold */
} MixChannel;
static MixChannel mixCh[MIX_CHANNELS];
static W          fifoSize = FIFO_BYTES;          /* ring size, same for every channel */

static SDPcmInfo pcmInfo = {
	{
		{0x0007,1,0,1,0,0},{0x0007,1,0,1,0,0},{0x0007,1,0,1,0,0},
		{0x0007,1,0,1,0,0},{0x0007,1,0,1,0,0},{0x0007,1,0,1,0,0},
		{0,0,0,0,0,0},{0,0,0,0,0,0},{0,0,0,0,0,0},{0,0,0,0,0,0},
		{0,0,0,0,0,0},{0,0,0,0,0,0},{0,0,0,0,0,0},{0,0,0,0,0,0},
		{0,0,0,0,0,0},{0,0,0,0,0,0}
	}
};
static SDPcmMode pcmOutputMode    = {PCMST48K, PCMFmt16, 1}; /* 48k 16bit stereo */
static SDPcmCtl  pcmOutputControl = PCM_STOP;
static SDSel     outputSelector   = {{1,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0}};

/* ------------------------------------------------------------ I/O helpers */

static void eOut(W reg, UW v) { out_w(ioBase + reg, v); }
static UW   eIn (W reg)       { return in_w(ioBase + reg); }
static void eOutB(W reg, UB v){ out_b(ioBase + reg, v); }

/* ------------------------------------------------- ES1371 SRC (sample rate conv) */

static W srcDead = 0;      /* latched on the first srcWait timeout */

static UW srcWait(void)
{
	W to;
	UW s = 0;
	/* Once the SRC has failed to de-busy once (absent/broken device), STOP polling
	 * 5000 reads per call: srcInit issues ~128 srcWrites, each calling srcWait, so an
	 * unlatched dead device would burn ~640k ring-0 I/O reads (~0.7-2 s) at load. The
	 * latch caps the whole bring-up at a single 5000-read timeout. On the working
	 * (VMware) device srcWait never times out, so srcDead stays 0. */
	if (srcDead)
		return eIn(ES_SRC);
	for (to = 0; to < ES_RW_TIMEOUT; to++) {
		s = eIn(ES_SRC);
		if (!(s & SRC_RBUSY))
			return s;
	}
	srcDead = 1;
	DEBUG_PRINT(("es1371: srcWait timeout (latching srcDead)\n"));
	return s;
}

static void srcWrite(W a, W d)
{
	UW r = srcWait() & SRC_CTLMASK;
	r |= SRC_RAMWE | SRC_ADDR(a) | SRC_DATA(d);
	eOut(ES_SRC, r);
}

/* one-time SRC RAM setup; programs DAC2 for 48kHz (VFI(16)) — the device
 * native rate, so the converter runs 1:1 and no per-rate reprogramming is
 * needed. */
static void srcInit(void)
{
	W i;
	eOut(ES_SRC, SRC_DISABLE);
	for (i = 0; i < 0x80; i++)
		srcWrite(i, 0);
	srcWrite(ESRC_ADC + ESRC_TRUNC_N, ESRC_SET_N(16));
	srcWrite(ESRC_ADC + ESRC_IREGS,  ESRC_SET_VFI(16));
	srcWrite(ESRC_ADC + ESRC_VFF, 0);
	srcWrite(ESRC_ADC_VOLL, ESRC_SET_ADC_VOL(16));
	srcWrite(ESRC_ADC_VOLR, ESRC_SET_ADC_VOL(16));
	srcWrite(ESRC_DAC1 + ESRC_TRUNC_N, ESRC_SET_N(16));
	srcWrite(ESRC_DAC1 + ESRC_IREGS,  ESRC_SET_VFI(16));
	srcWrite(ESRC_DAC1 + ESRC_VFF, 0);
	srcWrite(ESRC_DAC1_VOLL, ESRC_SET_DAC_VOLI(1));
	srcWrite(ESRC_DAC1_VOLR, ESRC_SET_DAC_VOLI(1));
	srcWrite(ESRC_DAC2 + ESRC_IREGS,  ESRC_SET_VFI(16));   /* 48kHz */
	srcWrite(ESRC_DAC2 + ESRC_TRUNC_N, ESRC_SET_N(16));
	srcWrite(ESRC_DAC2 + ESRC_VFF, 0);
	srcWrite(ESRC_DAC2_VOLL, ESRC_SET_DAC_VOLI(1));
	srcWrite(ESRC_DAC2_VOLR, ESRC_SET_DAC_VOLI(1));
	eOut(ES_SRC, 0);
}

/* ------------------------------------------------- ES1371 AC97 codec access */

static void codecReadyWrite(UW wd)
{
	W  to;
	UW src, t;
	for (to = 0; to < ES_RW_TIMEOUT; to++)
		if (!(eIn(ES_CODEC) & CODEC_WIP)) break;
	/* the codec sits behind the SRC; quiesce the SRC around the access */
	src = srcWait() & SRC_CTLMASK;
	eOut(ES_SRC, src | SRC_STATE_OK);
	for (to = 0; to < ES_RW_TIMEOUT; to++) {
		t = eIn(ES_SRC);
		if ((t & SRC_STATE_MASK) == 0) break;
	}
	for (to = 0; to < ES_RW_TIMEOUT; to++) {
		t = eIn(ES_SRC);
		if ((t & SRC_STATE_MASK) == SRC_STATE_OK) break;
	}
	eOut(ES_CODEC, wd);
	srcWait();
	eOut(ES_SRC, src);
}

static void codecWrite(UB a, UH d)
{
	codecReadyWrite(CODEC_WCMD(a, d));
}

static UH codecRead(UB a)
{
	W  to;
	UW t;
	codecReadyWrite(CODEC_WCMD(a, 0) | CODEC_READ);
	for (to = 0; to < ES_RW_TIMEOUT; to++)
		if (!(eIn(ES_CODEC) & CODEC_WIP)) break;
	for (to = 0; to < ES_RW_TIMEOUT; to++) {
		t = eIn(ES_CODEC);
		if (t & CODEC_VALID) return (UH)t;
	}
	return 0;
}

/* ------------------------------------------------------- hardware bring-up */

static W esProbe(void)
{
	W addr;
	if ((addr = searchPciDev(ES_PCI_VENDOR, ES_PCI_DEV_1371)) >= 0 ||
	    (addr = searchPciDev(ES_PCI_VENDOR, ES_PCI_DEV_5880)) >= 0) {
		pciConfigAddr = addr;
		DEBUG_PRINT(("es1371: found Ensoniq ES1371 at pci cfg 0x%04x\n", addr));
		return ER_OK;
	}
	DEBUG_PRINT(("es1371: no ES1371 on the PCI bus\n"));
	return ER_NOSPT;
}

static W esSecureIO(void)
{
	/* BAR0 (PCI 0x10) is a 0x40-byte I/O region. */
	ioBase = inPciConfW(pciConfigAddr, 0x10) & 0xffc0;
	if (ioBase == 0) {
		DEBUG_PRINT(("es1371: BAR0 not assigned\n"));
		return ER_NOSPT;
	}
	if (rsvHwRes(HW_IO(HW_SOUND), ioBase, ioBase + 0x3f) < ER_OK) {
		DEBUG_PRINT(("es1371: failed to reserve I/O 0x%x\n", ioBase));
		/* continue anyway: the region may already be ours */
	}
	/* enable I/O space + bus master in the PCI command register */
	outPciConfH(pciConfigAddr, 0x04, 0x0005);
	DEBUG_PRINT(("es1371: ioBase=0x%x\n", ioBase));
	return ER_OK;
}

static W esReset(void)
{
	/* basic chip init. ICSC power-on value is 0 (both clocks enabled); the SRC
	 * does the real rate work so PCLKDIV is left default. (The old code wrote
	 * ICSC=0x01 via the ES1370 "SERR_DIS" define, which on the ES1371 is the
	 * PCICLKDIS clock-gate bit -- wrong, though VMware ignored it.) */
	eOut(ES_ICSC, 0);

	/* AC97 warm reset: pulse ICSC bit14 (SYNC_RES) to resync the AC-link before
	 * any codec access. Preserve the other ICSC bits, flush the posted PCI write
	 * with a readback, and give the link a short settle before dropping
	 * SYNC_RES. */
	{
		UW c = eIn(ES_ICSC);
		W  i;
		eOut(ES_ICSC, c | ICSC_SYNC_RES);
		(void)eIn(ES_ICSC);
		for (i = 0; i < ES_RW_TIMEOUT; i++) (void)eIn(ES_ICSC);
		eOut(ES_ICSC, c & ~ICSC_SYNC_RES);
		(void)eIn(ES_ICSC);
	}

	eOut(ES_SIC, 0);
	srcInit();

	/* AC97 codec: cold reset + unmute master & PCM out at a sane volume. */
	codecWrite(AC97_RESET, 0);
	codecWrite(AC97_POWER, 0);                  /* power up all sections     */
	codecWrite(AC97_MASTER_VOL, 0x0000);        /* 0 attenuation, unmuted    */
	codecWrite(AC97_PCMOUT_VOL, 0x0808);        /* moderate PCM-out level    */
	return ER_OK;
}

static W esRegisterInterrupt(void);
static void InterruptHandler(UW param);   /* fwd: esInit's timer-path error branch refs it */

/* Allocate physically-contiguous, DMA-capable, resident memory and return both
 * the CPU pointer and the physical address. The ES1371 DAC2 reads the buffer by
 * physical address. */
static UB *GetMemoryForDMA(W size, UW *physicalAddress)
{
	UB *logical = NULL;
	W   ec;
	const W blockSize = 4096;
	W   blocks = (size + blockSize - 1) / blockSize;

	if ((ec = b_get_mbk(&logical, blocks, M_DMA | M_SYSTEM | M_RESIDENT)) < ER_OK) {
		DEBUG_PRINT(("es1371: b_get_mbk failed 0x%08x\n", ec));
		return NULL;
	}
	if ((ec = CnvPhysicalAddr(logical, blocks * blockSize, physicalAddress)) < ER_OK
	    || ec != blocks * blockSize) {
		DEBUG_PRINT(("es1371: CnvPhysicalAddr failed/non-contiguous 0x%08x\n", ec));
		b_rel_mbk(logical);
		return NULL;
	}
	return logical;
}

static W esAllocDMA(void)
{
	UW phys = 0;
	dmaBuffer = (UB *)GetMemoryForDMA(DMA_BYTES, &phys);
	if (dmaBuffer == NULL) {
		DEBUG_PRINT(("es1371: DMA buffer alloc failed\n"));
		return ER_NOMEM;
	}
	dmaBufferPhys = phys;
	memset(dmaBuffer, 0, DMA_BYTES);

	{	W i;
		for (i = 0; i < MIX_CHANNELS; i++) {
			mixCh[i].buf = (UB *)Kcalloc(1, fifoSize);
			if (mixCh[i].buf == NULL) {
				W j;
				for (j = 0; j < i; j++) { Kfree((VB *)mixCh[j].buf); mixCh[j].buf = NULL; }
				b_rel_mbk(dmaBuffer); dmaBuffer = NULL;
				return ER_NOMEM;
			}
			mixCh[i].inUse = False; mixCh[i].owner = 0;
			mixCh[i].head = mixCh[i].tail = 0;
			mixCh[i].lastL = mixCh[i].lastR = 0;
		}
	}
	DEBUG_PRINT(("es1371: DMA phys=0x%x %d bytes, %d mix channels x %d bytes\n",
	             phys, DMA_BYTES, MIX_CHANNELS, fifoSize));
	return ER_OK;
}

/* Undo esAllocDMA (+ quiesce chip interrupt sources) after a partial bring-up.
 * Callable only once esSecureIO has mapped the I/O window. The caller must have
 * removed the interrupt vector first if esRegisterInterrupt already ran. */
static void esCleanup(void)
{
	W i;
#if REFILL_BY_TIMER
	refillOn = False;   /* stop the cyclic touching dmaBuffer once it is freed below */
#endif
	eOut(ES_ICSC, eIn(ES_ICSC) & ~ICSC_DAC2_EN);
	eOut(ES_SIC,  eIn(ES_SIC)  & ~SIC_P2_INTR_EN);
	for (i = 0; i < MIX_CHANNELS; i++) {
		if (mixCh[i].buf != NULL) { Kfree((VB *)mixCh[i].buf); mixCh[i].buf = NULL; }
		mixCh[i].inUse = False;
	}
	if (dmaBuffer != NULL) { b_rel_mbk(dmaBuffer); dmaBuffer = NULL; }
}

static W esInit(void)
{
	W ec;
	if ((ec = esProbe()) < ER_OK)            return ec;
	if ((ec = esSecureIO()) < ER_OK)         return ec;
	if ((ec = esReset()) < ER_OK)            return ec;
	if ((ec = esAllocDMA()) < ER_OK)         return ec;
	if ((ec = esRegisterInterrupt()) < ER_OK) { esCleanup(); return ec; }
#if REFILL_BY_TIMER
	{
		/* Register the cyclic refill handler (always firing; gated by refillOn).
		 * The hardware ISR stays registered too -- it now only clears MPWR/stray
		 * and runs the watchdog, since the DAC2 interrupt is never enabled. */
		T_DCYC dcyc;
		dcyc.exinf  = NULL;
		dcyc.cycatr = TA_HLNG;
		dcyc.cychdr = (FP)cyclicRefill;
		dcyc.cycact = TCY_ON;
		dcyc.cyctim = CYC_INTERVAL_MS;
		if ((ec = vdef_cyc(&dcyc)) < ER_OK) {
			defIntHdr(-irq, InterruptHandler, 0);
			esCleanup();
			return ec;
		}
		cycId = (ID)ec;
	}
#endif
	return ER_OK;
}

/* ----------------------------------------------------------- FIFO -> DMA */

/* Diagnostic counters surfaced on each PCM write (DEBUG_PRINT is active in this
 * build — it produced the PCI/IO/DMA/IRQ bring-up trace). They reveal whether the
 * DAC2 interrupt fires at the right RATE (with the SAMP_CT frame-count fix this is
 * ~47/s = about 56 over the ~1.2s test at 48kHz; the old buggy 2047 value gave the
 * tell-tale ~23/s = about 28), and whether the FIFO is UNDERRUNNING (silence
 * pads, which give the intermittent/clicky output). */
static W dbgIrqN   = 0;   /* DAC2 interrupts handled        */
static W dbgUnderN = 0;   /* FIFO underruns (silence pads)  */
static W dbgDrainB = 0;   /* bytes drained FIFO->ring       */
static W dbgFill    = -1;     /* which sub-buffer the ISR last refilled            */
static W dbgStray   = 0;   /* total non-DAC2 ICSS.INTR events (stray/power-mgmt) */
static W strayRun   = 0;   /* CONSECUTIVE non-DAC2 interrupts (storm fail-safe)  */
static W irqBurst   = 0;   /* TOTAL ISR entries since the last app PCM write; catches a
                            * mixed/stuck-DAC2 storm the consecutive-stray counter misses */
static volatile Bool isrBusy = False;  /* a DAC2 refill (mixInto) is in progress; guards
                                        * the interrupts-ENABLED window against ISR re-entry */

/* --- per-opener mixer channels ------------------------------------------- */

static MixChannel *chGet(ID owner)          /* channel currently owned by `owner` */
{
	W i;
	for (i = 0; i < MIX_CHANNELS; i++)
		if (mixCh[i].inUse && mixCh[i].owner == owner) return &mixCh[i];
	return NULL;
}
static MixChannel *chBind(ID owner)         /* find, or claim a free slot for, owner */
{
	W i, m;
	MixChannel *c = chGet(owner);
	if (c) return c;
	for (i = 0; i < MIX_CHANNELS; i++) {
		if (mixCh[i].inUse) continue;
		DI(m);                              /* claim atomically w.r.t. the ISR mixer */
		mixCh[i].owner = owner;
		mixCh[i].head  = mixCh[i].tail = 0;
		mixCh[i].lastL = mixCh[i].lastR = 0;
		mixCh[i].inUse = True;              /* publish LAST -> ISR never sees half-init */
		EI(m);
		return &mixCh[i];
	}
	return NULL;                            /* all channels busy -> caller drops audio */
}
static void chFree(ID owner)
{
	MixChannel *c = chGet(owner);
	if (c) { W m; DI(m); c->inUse = False; c->head = c->tail = 0; EI(m); }
}
static W chActive(void)
{
	W i, n = 0;
	for (i = 0; i < MIX_CHANNELS; i++) if (mixCh[i].inUse) n++;
	return n;
}

/* Fast single-channel drain (the common case: only one app playing). On underrun
 * HOLD the channel's last frame (DC) rather than writing zeros -- a zero-fill makes
 * a full-amplitude step every IRQ = an audible buzz; a DC hold is click-free. */
static void drainChannel(MixChannel *c, UB *dst, W bytes)
{
	W avail = c->head - c->tail;
	if (avail < 0) avail += fifoSize;
	while (bytes > 0) {
		if (avail > 0) {
			W chunk = fifoSize - c->tail, last;
			if (chunk > bytes) chunk = bytes;
			if (chunk > avail) chunk = avail;
			memcpy(dst, c->buf + c->tail, chunk);
			c->tail = (c->tail + chunk) % fifoSize;
			dst += chunk; bytes -= chunk; avail -= chunk;
			dbgDrainB += chunk;
			last = (c->tail - 4 + fifoSize) % fifoSize;
			c->lastL = *((H *)(c->buf + last));
			c->lastR = *((H *)(c->buf + last + 2));
		} else {
			W i;
			for (i = 0; i + 4 <= bytes; i += 4) {
				*((H *)(dst + i))     = c->lastL;
				*((H *)(dst + i + 2)) = c->lastR;
			}
			dbgUnderN++;
			bytes = 0;
		}
	}
}

/* Refill one DAC2 ring quarter. 0 active channels -> silence; 1 -> the fast drain
 * (identical to the old single-FIFO path, so a lone app pays no mix cost); >=2 ->
 * MIX: per stereo frame, sum every active channel's int16 L/R (holding each
 * channel's last frame on its own underrun) in 32-bit accumulators and clamp to
 * int16. Consumer side of the per-channel SPSC rings; runs from the ISR. */
static void mixInto(UB *dst, W bytes)
{
	MixChannel *act[MIX_CHANNELS];
	W nact = 0, k, f, frames;

	for (k = 0; k < MIX_CHANNELS; k++)
		if (mixCh[k].inUse) act[nact++] = &mixCh[k];

	if (nact == 0) { memset(dst, 0, bytes); return; }
	if (nact == 1) { drainChannel(act[0], dst, bytes); return; }

	frames = bytes >> 2;                    /* 4 bytes per stereo frame */
	for (f = 0; f < frames; f++) {
		W accL = 0, accR = 0;
		for (k = 0; k < nact; k++) {
			MixChannel *c = act[k];
			H l, r;
			W avail = c->head - c->tail;
			if (avail < 0) avail += fifoSize;
			if (avail >= 4) {               /* a WHOLE frame is available */
				l = *((H *)(c->buf + c->tail));
				r = *((H *)(c->buf + c->tail + 2));
				c->tail = (c->tail + 4) % fifoSize;
				c->lastL = l; c->lastR = r;
			} else {                        /* underrun / sub-frame remainder ->
			                                 * hold last (DC); NEVER advance tail
			                                 * past head (would corrupt the ring) */
				l = c->lastL; r = c->lastR;
			}
			accL += (W)l; accR += (W)r;
		}
		if      (accL >  32767) accL =  32767;
		else if (accL < -32768) accL = -32768;
		if      (accR >  32767) accR =  32767;
		else if (accR < -32768) accR = -32768;
		*((H *)(dst + f * 4))     = (H)accL;
		*((H *)(dst + f * 4 + 2)) = (H)accR;
	}
	dbgDrainB += bytes;
}

#if REFILL_BY_TIMER
/* Cyclic handler (handler context; MEMORY ONLY -- no I/O ports here). Refills the
 * DAC2 ring from wall-clock elapsed time: bytes = ms * 192 (48000 frames/s * 4
 * B/frame / 1000). mixInto pulls from the per-channel FIFOs (silence on
 * underrun), so this is the sole ring producer in timer mode. swWrite is primed
 * half a ring ahead of the chip read pointer at start; both advance at real-time
 * 48 kHz, so they never lap while tick jitter stays under that lead (~42 ms for
 * the 16 KB ring). No DAC2 interrupt is enabled, so no INTx storm is possible. */
static void cyclicRefill(void)
{
	SYSTIME now;
	W elapsed, bytes;
	if (!refillOn) return;
	if (vget_otm(&now) < ER_OK) return;   /* operating time: monotonic since boot */
	elapsed = (W)(now - lastFill);
	if (elapsed <= 0) return;                  /* clock unchanged / stepped back    */
	lastFill = now;
	if (elapsed > 250) elapsed = 250;          /* clamp after a stall (< ring span) */
	bytes = (elapsed * 192) & ~3;              /* 48000 * 4 / 1000 = 192 B/ms        */
	if (bytes > DMA_BYTES) bytes = DMA_BYTES;
	while (bytes > 0) {
		W chunk = DMA_BYTES - (W)swWrite;
		if (chunk > bytes) chunk = bytes;
		mixInto(dmaBuffer + swWrite, chunk);
		swWrite = (swWrite + (UW)chunk) % (UW)DMA_BYTES;
		bytes -= chunk;
	}
}
#endif

static void esStartDAC2(void)
{
	W  mask;
	UW sic;

	/* Stop any prior DAC2 run BEFORE touching the shared ring state below. Otherwise,
	 * if a DAC2 IRQ fires mid-reset (reachable via the auto-start path or a leftover
	 * run), the ISR's refill memcpy interleaves our 16KB memset and its dmaLastHalf
	 * update races ours -> a corrupted ring (a burst of stale audio, and the ISR may
	 * overwrite the quarter the chip is DMA-reading). Disabling DAC2_EN + P2_INTR_EN
	 * quiesces the ISR; the dmaLastHalf reset is DI-guarded against any in-flight IRQ,
	 * and the big memset runs with DAC2 off (no ISR to race). The per-channel PCM
	 * rings are NOT reset here -- an already-bound app's buffered audio must survive a
	 * (re)start triggered by a DIFFERENT app; each ring is reset in chBind on claim. */
	eOut(ES_ICSC, eIn(ES_ICSC) & ~ICSC_DAC2_EN);
	eOut(ES_SIC,  eIn(ES_SIC)  & ~SIC_P2_INTR_EN);
	DI(mask);
	dmaLastHalf = -1;
	EI(mask);
#ifdef DIAG_STATIC_RING
	{   /* DIAGNOSTIC: fill the whole ring with a seamless triangle (64 cycles over
	     * 4096 frames) and have the ISR SKIP the refill. If THIS plays clean, the
	     * ring DMA/SRC/loop path is fine and the noise is 100% in the FIFO/refill;
	     * if it still buzzes at ~23Hz, the seam is in the chip loop-wrap/SRC. */
		W i; H *p = (H *)dmaBuffer;
		for (i = 0; i < DMA_FRAMES; i++) {
			W ph = i & 63;
			H v = (H)((ph < 32) ? (ph * 768 - 12000) : (12000 - (ph - 32) * 768));
			p[2 * i] = v; p[2 * i + 1] = v;
		}
	}
#else
	memset(dmaBuffer, 0, DMA_BYTES);
#endif

	/* DAC2 format: 16-bit stereo, interrupt enable. */
	sic = eIn(ES_SIC);
	sic &= ~(SIC_P2_S_EB | SIC_P2_S_MB | SIC_INC_BITS);
	sic |= SIC_SET_P2_ST_INC(0) | SIC_SET_P2_END_INC(2);  /* 16-bit -> inc 2 */
	sic |= SIC_P2_S_EB | SIC_P2_S_MB;                     /* 16-bit, stereo  */
	eOut(ES_SIC, sic & ~SIC_P2_INTR_EN);
#if !REFILL_BY_TIMER
	eOut(ES_SIC, sic | SIC_P2_INTR_EN);
#endif

	/* program the DAC2 frame buffer (physical addr + size in dwords-1). */
	eOut(ES_MEMPAGE, MEMPAGE_DAC);
	eOut(ES_DAC2_ADDR, dmaBufferPhys);
	eOut(ES_DAC2_SIZE, SET_SIZE(0, (DMA_BYTES >> 2) - 1));

	/* interrupt every sub-buffer (quarter). ROOT-CAUSE FIX: DAC2 SAMP_CT (0x28)
	 * counts STEREO FRAMES for 16-bit stereo (1 dword = one L+R pair) -- the same
	 * bytes>>2 unit as DAC2_SIZE. The register holds
	 * (frames-per-interrupt - 1) = 1023 for a 4096-byte quarter. The OLD value
	 * (dmaPartFrames*2)-1 = 2047 treated it as L+R SAMPLES, so the chip interrupted
	 * every 2048 frames (HALF the ring, ~23/s) while the ISR refills only one 1024-
	 * frame quarter per IRQ -> refill ran at HALF the play rate, half the ring stayed
	 * stale (continuous buzz, worse with complex signals) and every 4th IRQ the ISR
	 * overwrote the quarter being DMA-read (the ~seconds dropout). */
	eOut(ES_DAC2_CSR, dmaPartFrames - 1);

	eOut(ES_SRC, 0);
	eOut(ES_ICSC, eIn(ES_ICSC) | ICSC_DAC2_EN);  /* GO */
	pcmOutputControl = PCM_PLAY;
#if REFILL_BY_TIMER
	swWrite = DMA_BYTES / 2;    /* prime ~half a ring ahead of the chip read ptr */
	vget_otm(&lastFill);
	refillOn = True;            /* the cyclic handler now drives the refill       */
#endif
	DEBUG_PRINT(("es1371: DAC2 started csr=%d partFrames=%d parts=%d dmaBytes=%d sic=0x%x icsc=0x%x\n",
	             dmaPartFrames - 1, dmaPartFrames, DMA_PARTS, DMA_BYTES,
	             (W)eIn(ES_SIC), (W)eIn(ES_ICSC)));
}

static void esStopDAC2(void)
{
#if REFILL_BY_TIMER
	refillOn = False;                       /* cyclicRefill becomes a no-op */
#endif
	eOut(ES_ICSC, eIn(ES_ICSC) & ~ICSC_DAC2_EN);
	eOut(ES_SIC, eIn(ES_SIC) & ~SIC_P2_INTR_EN);
	pcmOutputControl = PCM_STOP;
	DEBUG_PRINT(("es1371: DAC2 stopped\n"));
}

static void InterruptHandler(UW param)
{
	W mask;
	UW status;
	(void)param;

	DI(mask);
	status = eIn(ES_ICSS);
	if (!(status & ICSS_INTR)) {       /* foreign shared-IRQ device: not ours */
		EI(mask);
		return;
	}
	/* Total-interrupt watchdog. The consecutive-stray counter below is reset by
	 * every real DAC2 IRQ, so a STUCK DAC2 (I_DAC2 that the ACK toggle fails to
	 * clear under VMware) or a mixed DAC2/stray storm keeps re-entering forever
	 * while strayRun stays 0 -> livelock it never catches. irqBurst counts EVERY
	 * entry and is reset only by an app PCM write (procWrite); legit playback is
	 * <1 IRQ per write (~47/s vs ~60 writes/s), so thousands of entries with ZERO
	 * writes between them can only be a storm. Cut every ES1371 interrupt source
	 * and survive; the next PCM write re-arms DAC2 and resets irqBurst. */
	if (++irqBurst > 4096) {
		eOut(ES_SIC,  eIn(ES_SIC)  & ~0x00000700);
		eOut(ES_ICSC, eIn(ES_ICSC) & ~(ICSC_DAC2_EN | ICSC_DAC1_EN |
		                               ICSC_ADC_EN  | ICSC_PDLEV));
		pcmOutputControl = PCM_STOP;
		irqBurst = 0; strayRun = 0;
		EI(mask);
		return;
	}
	if (!(status & ICSS_I_DAC2) || status == 0xFFFFFFFF) {
		/* All-ones means the chip did not decode the read at all (device in D3 /
		 * I/O access disabled / hot-removed): PCI returns 0xFFFFFFFF, which fakes
		 * INTR+I_DAC2 both set. Without this test such a read would take the DAC2
		 * path and reset strayRun every time, PERMANENTLY disarming the 64-stray
		 * fail-safe below while a shared-IRQ line storms -> OS livelock. Treat it
		 * as a stray so the fail-safe can fire.
		 *
		 * Otherwise: ICSS.INTR is set but NOT DAC2 -- an interrupt source we never use (the
		 * power-management MPWR raised by a VMware suspend/resume, a CODEC SYNC_ERR,
		 * or a spurious latch). DAC2 is the only source we enable, but the summary
		 * INTR bit is level-triggered on the PCI INTx line: if we return WITHOUT
		 * dropping it, the CPU re-enters this ISR immediately and forever -- the
		 * whole OS livelocks with NO processor exception (exactly the reported
		 * freeze). So clear what we can. The one non-DAC2 source VMware actually
		 * raises is MPWR, cleared by writing PDLEV(9:8) = the current PowerState
		 * (datasheet 7.x: "the ISR should program PDLEV to equal the config-space
		 * Power State"); the ISR only ever runs in D0, so match 00. Fail-safe: if
		 * a source we cannot clear keeps storming, cut the chip interrupt entirely
		 * so the OS survives -- audio stops, and the next PCM write restarts it. */
		UW icsc = eIn(ES_ICSC);
		eOut(ES_ICSC, icsc & ~ICSC_PDLEV);       /* clear MPWR (PDLEV := D0) */
		dbgStray++;
		if (++strayRun > 32) {
			/* A source we can't identify keeps the level-triggered PCI INTx line
			 * asserted. The old fail-safe cleared ONLY DAC2 -- if the storm is not
			 * DAC2, INTx stays asserted and the CPU re-enters this ISR forever
			 * (whole-OS livelock = the VMware freeze). Cut EVERY interrupt source
			 * the ES1371 can raise: all three SIC stream-interrupt enables
			 * (P1/DAC1, P2/DAC2, R1/ADC = 0x700), and the DAC/ADC run enables plus
			 * MPWR (PDLEV) in ICSC. With no enabled source the chip deasserts INTx,
			 * so the OS survives; the next PCM write's esStartDAC2 re-enables DAC2. */
			eOut(ES_SIC,  eIn(ES_SIC)  & ~0x00000700);
			eOut(ES_ICSC, eIn(ES_ICSC) & ~(ICSC_DAC2_EN | ICSC_DAC1_EN |
			                               ICSC_ADC_EN  | ICSC_PDLEV));
			pcmOutputControl = PCM_STOP;
			strayRun = 0;
		}
		EI(mask);
		return;
	}
	{
		UW sic;
		W  fillPart;

		strayRun = 0;                  /* a real DAC2 IRQ -> not storming */

		/* ACK FIRST (spec 7.5: clear the DAC2 interrupt by setting P2_INTR_EN to 0
		 * then 1). Done BEFORE the slow mixInto memcpy so a half-completion landing
		 * during the copy stays latched and is re-delivered, not coalesced away. */
		sic = eIn(ES_SIC);
		eOut(ES_SIC, sic & ~SIC_P2_INTR_EN);
		eOut(ES_SIC, sic | SIC_P2_INTR_EN);

		dbgIrqN++;

		/* The DAC2 play-position registers are frozen under VMware (verified: 0x3C>>16
		 * stays 0, 0x28>>16 stays at SAMP_CT), so the refill is keyed purely to the IRQ
		 * cadence. The chip starts at sub-buffer 0; the first IRQ means part 0 just
		 * FINISHED (the chip is now in part 1), so refill part 0 -- dmaLastHalf starts
		 * at -1 -> fillPart 0, then 1,2,3,0,... Refilling the just-finished part keeps a
		 * ~32ms (3-part) guard band before the chip re-reads it, so a +/-1 IRQ slip no
		 * longer overwrites live audio. The dead per-IRQ MEMPAGE/position reads were
		 * removed to shorten the DI window (less chance of coalescing an IRQ). */
		fillPart = (dmaLastHalf < 0) ? 0 : ((dmaLastHalf + 1) % DMA_PARTS);
		dmaLastHalf = fillPart;
		dbgFill = fillPart;
		/* The 4KB mixInto memcpy is the slow part; running it under DI holds
		 * interrupts off for the whole copy, which at VMware's IRQ cadence adds
		 * latency and livelock pressure. ACK is already done above (latched), and
		 * fillPart/dmaLastHalf are committed, so drop DI and mix with interrupts
		 * ENABLED. isrBusy guards against re-entry: a DAC2 IRQ arriving mid-mix
		 * (if the OS does not mask the line during the handler) advances
		 * dmaLastHalf and returns WITHOUT a second concurrent mixInto -- that one
		 * quarter goes un-refilled (a brief stale-audio glitch), never a buffer
		 * race. */
		if (isrBusy) { EI(mask); return; }
		isrBusy = True;
		EI(mask);
#if !defined(DIAG_STATIC_RING) && !REFILL_BY_TIMER
		mixInto(dmaBuffer + fillPart * DMA_PART_BYTES, DMA_PART_BYTES);
#endif
		isrBusy = False;
		return;
	}
}

static W esRegisterInterrupt(void)
{
	irq = inPciConfB(pciConfigAddr, PCR_IRQLIN);
	if (irq < 3 || irq > 15) {
		DEBUG_PRINT(("es1371: no usable IRQ (%d)\n", irq));
		return ER_NOSPT;
	}
	if (defIntHdr(irq, InterruptHandler, 0) < ER_OK) {
		DEBUG_PRINT(("es1371: defIntHdr failed (irq %d)\n", irq));
		return ER_NOSPT;
	}
	DEBUG_PRINT(("es1371: irq=%d\n", irq));
	return ER_OK;
}

/* ---------------------------------------------- device-request processing */

static ERR procOpen(DriverInfo *di, ID devid)
{
	di->openCnt++;
	di->opened = True;
	(void)devid;
	return ER_OK;
}
static ERR procClose(DriverInfo *di, ID taskid, Bool all)
{
	if (all) {                              /* DC_CLOSEALL: drop every stream */
		W i, m;
		DI(m);
		for (i = 0; i < MIX_CHANNELS; i++) { mixCh[i].inUse = False; mixCh[i].head = mixCh[i].tail = 0; }
		EI(m);
		di->openCnt = 0;
	} else {
		chFree(taskid);                     /* free just the closing app's stream */
		if (di->openCnt > 0) di->openCnt--;
		if (di->openCnt == 0) {
			/* LAST opener gone: no legitimate producer remains, so reap EVERY
			 * channel. Covers an app whose PCM writes came from a different task
			 * than its close (a worker task writes, the main task closes) --
			 * chFree(closer taskid) misses that channel, and without this reap a
			 * slot would leak per app run until all 4 are gone = system-wide
			 * silence until reboot. */
			W i, m;
			DI(m);
			for (i = 0; i < MIX_CHANNELS; i++) { mixCh[i].inUse = False; mixCh[i].head = mixCh[i].tail = 0; }
			EI(m);
		}
	}
	if (chActive() == 0) esStopDAC2();      /* silence the DAC only when none remain */
	di->opened = (di->openCnt > 0);
	return ER_OK;
}

static WERR returnInfo(DevRsp *rsp, DevReq *req, UB *p, W size)
{
	W n = req->datacnt;                 /* bytes the caller asked for */
	if (n < 0) n = 0;                   /* W is SIGNED: a negative datacnt would skip the
	                                     * `n > size` clamp and reach memcpy as (size_t)~0
	                                     * -> a ~4GB copy in ring 0. Floor from below first. */
	if (n > size) n = size;
	memcpy((UB *)req->memptr, p, n);
	rsp->datacnt = n;
	return n;
}

static ERR procRead(DriverInfo *di, DevRsp *rsp, DevReq *req)
{
	(void)di;
	switch (req->datano) {
	case DN_SDPCMINFO: return returnInfo(rsp, req, (UB *)&pcmInfo, sizeof(pcmInfo));
	case DN_SDPCMMODE: return returnInfo(rsp, req, (UB *)&pcmOutputMode, sizeof(SDPcmMode));
	case DN_SDPCMCTL:  return returnInfo(rsp, req, (UB *)&pcmOutputControl, sizeof(SDPcmCtl));
	case DN_SDPCMCNT: {
		/* queued (accepted but not yet played) bytes of the CALLER's channel.
		 * Lets an app bound its own audio latency: the SPSC head/tail pair is
		 * read without a lock -- a concurrent ISR drain only makes the answer
		 * momentarily stale, never wild. Channels are keyed by requester task
		 * ID, so this must be read from the task that writes the PCM. */
		MixChannel *ch = chBind(req->taskid);
		W q = 0;
		if (ch != NULL) {
			q = ch->head - ch->tail;
			if (q < 0) q += fifoSize;
		}
		return returnInfo(rsp, req, (UB *)&q, sizeof(q));
	}
	case DN_SDPCMBUFSZ: return returnInfo(rsp, req, (UB *)&fifoSize, sizeof(fifoSize));
	case DN_SDSELOUT:  return returnInfo(rsp, req, (UB *)&outputSelector, sizeof(SDSel));
	default:           rsp->datacnt = 0; return ER_OK;
	}
}

/* append application PCM into a channel's ring, returning bytes accepted. */
static W fifoWriteCh(MixChannel *c, const UB *src, W bytes)
{
	W space, written = 0;
	space = c->tail - c->head - 1;
	if (space < 0) space += fifoSize;
	space &= ~3;                        /* keep the ring 4-byte (L,R int16 frame)
	                                     * aligned: the -1 full/empty guard byte made
	                                     * `space` 3 mod 4, so a near-full clamp
	                                     * advanced head by a partial frame -> a
	                                     * permanent L/R smear. Floor to a whole frame. */
	bytes &= ~3;                        /* AND floor the CALLER's count. datacnt is
	                                     * untrusted (any opener may wri_dev a non-mult-of-4
	                                     * count); an unfloored partial frame would advance
	                                     * head off the 4-byte grid, permanently de-aligning
	                                     * the SPSC ring and making the ISR drainChannel/mixInto
	                                     * read a straddled frame past the buffer end. Negative
	                                     * `bytes` stays <=0 here and is dropped by the loop. */
	if (bytes > space) bytes = space;
	while (bytes > 0) {
		W chunk = fifoSize - c->head;
		if (chunk > bytes) chunk = bytes;
		memcpy(c->buf + c->head, src, chunk);
		c->head = (c->head + chunk) % fifoSize;
		src += chunk; bytes -= chunk; written += chunk;
	}
	return written;
}

static ERR procWrite(DriverInfo *di, DevRsp *rsp, DevReq *req)
{
	if (di->suspended) {
		/* Device is power-suspended: swallow every write WITHOUT touching the chip
		 * (no chBind, no esStartDAC2) so a PCM stream cannot restart DAC2 DMA mid
		 * transition. Report 0 accepted so the client's pump loop stops cleanly
		 * instead of spinning; audio simply drops until DC_RESUME. */
		rsp->datacnt = 0;
		return ER_OK;
	}
	switch (req->datano) {
	case DN_SDPCMMODE:
		if (req->datacnt == sizeof(SDPcmMode))
			memcpy(&pcmOutputMode, (UB *)req->memptr, sizeof(SDPcmMode));
		rsp->datacnt = req->datacnt;
		return ER_OK;
	case DN_SDPCMCTL: {
		/* validate the declared size before dereferencing memptr (as DN_SDPCMMODE does):
		 * a short datacnt whose buffer ends at a page tail would otherwise fault the
		 * 4-byte read in ring 0. */
		if (req->datacnt >= (W)sizeof(SDPcmCtl)) {
			SDPcmCtl c = *(SDPcmCtl *)req->memptr;
			if (c == PCM_PLAY && pcmOutputControl != PCM_PLAY) esStartDAC2();
			else if (c == PCM_STOP) {
				chFree(req->taskid);            /* stop only THIS app's stream */
				if (chActive() == 0) esStopDAC2();  /* silence the DAC only when none left */
			}
		}
		rsp->datacnt = req->datacnt;
		return ER_OK;
	}
	case DN_SDPCMBUFSZ:
	case DN_SDSELOUT:
	case DN_SDVOLMAIN: case DN_SDVOLPCM:
		rsp->datacnt = req->datacnt;
		return ER_OK;
	case 0:
	default: {
		/* raw PCM payload -> this app's channel (bound lazily by taskid);
		 * auto-start DAC2 on first data. */
		MixChannel *ch = chBind(req->taskid);
		W n;
		if (req->datacnt <= 0 || req->memptr == NULL) {
			/* zero-length write = free-space query (wri_dev(dd,0,NULL,0,&n)):
			 * report how many bytes the next write would accept, WITHOUT
			 * starting the DAC or resetting the ISR watchdog. */
			W space = 0;
			if (ch != NULL) {
				space = ch->tail - ch->head - 1;
				if (space < 0) space += fifoSize;
				space &= ~3;
			}
			rsp->datacnt = space;
			return ER_OK;
		}
		n = (ch != NULL) ? fifoWriteCh(ch, (const UB *)req->memptr, req->datacnt) : 0;
		irqBurst = 0;                    /* app made progress -> re-arm the ISR storm watchdog */
		if (pcmOutputControl != PCM_PLAY) esStartDAC2();
		/* NOTE: no per-write DEBUG_PRINT here -- it fires once per client pump call
		 * (~60/s) and BTRON console output is slow enough to tank the client's frame
		 * rate, which then underruns the FIFO and chops the audio. The bring-up logs
		 * above and the one-shot esStartDAC2 log are enough. */
		rsp->datacnt = n;
		return ER_OK;
	}
	}
}

static ERR procRequest(DriverInfo *di, DevRsp *rsp, DevReq *req)
{
	switch (req->cmd.cmd) {
	case DC_OPEN:     return procOpen(di, req->devid);
	case DC_CLOSE:    return procClose(di, req->taskid, False);
	case DC_CLOSEALL: return procClose(di, req->taskid, True);
	case DC_READ:     return procRead(di, rsp, req);
	case DC_WRITE:    return procWrite(di, rsp, req);
	case DC_ABORT:    return ER_OK;
	case DC_SUSPEND:  di->suspended = True;  esStopDAC2(); return ER_OK;
	case DC_RESUME:   di->suspended = False; return ER_OK;
	default:          return toERR(EC_PAR, ED_CMD);
	}
}

/* ---------------------------------------------------- device registration */

static WERR registerDevice(DriverInfo *di)
{
	/* device name "サウンド" as explicit TRON codes (TC name[8]); .c files do not
	 * get the .C wide-string->TC conversion, so L"..." is not usable here. */
	static DevDef dd = { {0}, 1, { 0x2535,0x2526,0x2573,0x2549, 0,0,0,0 }, 0 };
	ERR ec;
	dd.attr.chardev = 1;
	dd.attr.nowait  = 1;
	dd.portid = di->acceptingPort;
	if ((ec = b_DefDevice(&dd, NULL)) < ER_OK)
		return ec;
	di->deviceID = (ID)ec;
	return ER_OK;
}

static void mainTask(DriverInfo *di)
{
	T_CPOR cpor;
	ERR    ec;
	RNO    rno;
	UW     ptn;
	DevReq req;
	INT    reqsz;
	DevRsp rsp;

	cpor.poratr  = TA_NULL;
	cpor.maxcmsz = sizeof(DevReq);
	cpor.maxrmsz = sizeof(DevRsp);
	if ((ec = vcre_por(&cpor)) < ER_OK) { Kfree((VB *)di); exd_tsk(); }
	di->acceptingPort = (ID)ec;

	if (registerDevice(di) < ER_OK) { Kfree((VB *)di); exd_tsk(); }

	while (True) {
		/* D_NORM_PTN covers only OPEN/CLOSE/CLOSEALL/READ/WRITE -- SUSPEND, RESUME
		 * and ABORT must be OR'd in explicitly or acp_por never accepts them and
		 * their (system) callers block forever. Accept the SAME full set whether or
		 * not suspended: a client may keep issuing DC_WRITE from a worker task while
		 * the device is suspended, holding a GUI lock across that call --
		 * if we stopped accepting DC_WRITE the client would block until resume,
		 * freezing its UI. Instead we always accept, and procWrite DROPS writes while
		 * suspended (no DAC2 touch), so nothing restarts the chip mid power transition
		 * yet no caller ever wedges. (The old `if (di->suspended) ptn = ...RESUME
		 * only...` caused exactly that wedge.) */
		ptn = D_NORM_PTN | D_CALPTN(DC_SUSPEND) | D_CALPTN(DC_RESUME) | D_CALPTN(DC_ABORT);
		if (acp_por(&rno, (VP)&req, &reqsz, di->acceptingPort, ptn) < ER_OK)
			continue;
		rsp.devid = req.devid; rsp.cmd = req.cmd;
		rsp.datano = req.datano; rsp.datacnt = 0;
		rsp.error.err = (reqsz == sizeof(DevReq))
			? procRequest(di, &rsp, &req)
			: toERR(EC_PAR, ED_CMD);
		rpl_rdv(rno, (VP)&rsp, sizeof(DevRsp));
	}
}

static WERR startDriver(W priority)
{
	DriverInfo *di;
	T_CTSK ctsk;
	ID tid;
	ERR ec;

	if ((di = (DriverInfo *)Kcalloc(1, sizeof(DriverInfo))) == NULL)
		return ER_NOMEM;
	di->priority = priority;

	ctsk.exinf   = di;
	ctsk.task    = (FP)mainTask;
	ctsk.itskpri = priority;
	ctsk.stksz   = 2048;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	if ((tid = ec = vcre_tsk(&ctsk)) < ER_OK) { Kfree((VB *)di); return ec; }
	if ((ec = sta_tsk(tid, (INT)di)) < ER_OK) { del_tsk(tid); Kfree((VB *)di); return ec; }
	return tid;
}

/* --------------------------------------------------------- kerext entry */

ERR main(Bool startUp, TC *arg)
{
	ERR ec;
	(void)arg;
	if (!startUp) {
		/* Driver unload / system shutdown. If the OS delivers this (brightv calls
		 * main() with startUp=False on kerext teardown), QUIESCE the ES1371 so
		 * VMware is not left tearing the device down under an active DAC2 DMA + a
		 * registered IRQ -- that is what breaks the emulated-device transport pipe
		 * ("トランスポート(VMDB)エラー -14 Pipe connection has been broken"). Touch
		 * the chip only if bring-up completed (dmaBuffer set by esAllocDMA, which
		 * also implies esRegisterInterrupt succeeded so `irq` is valid). Remove the
		 * interrupt vector BEFORE freeing buffers (esCleanup's contract). */
		if (dmaBuffer != NULL) {
			esStopDAC2();
			eOut(ES_SIC,  eIn(ES_SIC)  & ~0x00000700);
			eOut(ES_ICSC, eIn(ES_ICSC) & ~(ICSC_DAC2_EN | ICSC_DAC1_EN | ICSC_ADC_EN));
			if (irq >= 3 && irq <= 15)
				defIntHdr(-irq, InterruptHandler, 0);
			esCleanup();
		}
		return ER_OK;
	}
	DEBUG_PRINT(("ES1371 Sound Driver (VMware AudioPCI) v0.19: opt REFILL_BY_TIMER (IRQ-less cyclic refill); v0.18 total-IRQ watchdog + stray32; v0.17 INTx full-mask + ISR mix outside DI + shutdown quiesce\n"));

	if ((ec = esInit()) < ER_OK) {
		DEBUG_PRINT(("es1371: esInit failed 0x%08x\n", ec));
		return ec;
	}
	if ((ec = startDriver(taskPriority)) < ER_OK) {
		/* esInit already armed the interrupt vector; if we return failure with it
		 * still registered and the loader releases this module image, the next
		 * interrupt on the (shared) line vectors into freed ring-0 code = OS crash.
		 * Unregister FIRST (defIntHdr with a negative irq), then free the DMA/mix
		 * buffers. */
		defIntHdr(-irq, InterruptHandler, 0);
		esCleanup();
		DEBUG_PRINT(("es1371: startDriver failed 0x%08x\n", ec));
		return ec;
	}
	return ER_OK;
}
