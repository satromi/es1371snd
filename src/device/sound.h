/*
 * device/sound.h - BTRON "サウンド" PCM device interface.
 *
 * RECONSTRUCTED minimal interface: the official 超漢字 <device/sound.h> is not
 * present in this SDK (only beep.h/disk.h/etc. ship under include/device/).
 * This header defines the app<->driver protocol es1371snd implements; the values
 * are self-consistent for the driver and the applications built against it. If
 * compatibility with standard applications is needed, drop in the official
 * header instead.
 */
#ifndef __DEVICE_SOUND_H__
#define __DEVICE_SOUND_H__

#include <basic.h>

/* sample-rate codes (index into the rate table) */
#define PCMST8K     0
#define PCMST11K    1
#define PCMST16K    2
#define PCMST22K    3
#define PCMST44K    4
#define PCMST48K    5

/* data formats */
#define PCMFmt8     1
#define PCMFmt16    2

/* playback control */
typedef W SDPcmCtl;
#define PCM_STOP    0
#define PCM_PLAY    1
#define PCM_PAUSE   2

typedef struct {
	W samplerate;     /* PCMST*  */
	W datafmt;        /* PCMFmt* */
	W stereo;         /* 0=mono 1=stereo */
} SDPcmMode;

typedef struct {
	UH fmt;           /* supported format bitmask */
	UB stereo;
	UB resv1;
	UB chans;
	UB resv2;
	UB resv3;
} SDPcmRate;

typedef struct {
	SDPcmRate rate[16];
} SDPcmInfo;

typedef struct {
	UB s[18];         /* input/output source selector flags */
} SDSel;

/* device data numbers (the `datano` of a device request). PCM sample data uses
 * datano 0; the negative numbers are control/parameter items. */
#define DN_SDPCMINFO    (-1)
#define DN_SDPCMMODE    (-2)
#define DN_SDPCMCTL     (-3)
#define DN_SDPCMCNT     (-4)
#define DN_SDPCMENDCNT  (-5)
#define DN_SDPCMBUFSZ   (-6)
#define DN_SDSELIN      (-10)
#define DN_SDSELOUT     (-11)
#define DN_SDGAININ     (-12)
#define DN_SDGAINOUT    (-13)
#define DN_SDVOLMAIN    (-20)
#define DN_SDVOLTREBLE  (-21)
#define DN_SDVOLBASS    (-22)
#define DN_SDVOLPCM     (-23)
#define DN_SDVOLBEEP    (-24)
#define DN_SDVOLMIC     (-25)
#define DN_SDVOLLINE    (-26)
#define DN_SDVOLCD      (-27)
#define DN_SDVOLMUSIC   (-28)
#define DN_SDVOLAUX1    (-29)
#define DN_SDVOLAUX2    (-30)

#endif /* __DEVICE_SOUND_H__ */
