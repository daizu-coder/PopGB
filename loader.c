#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#undef _GNU_SOURCE
#define _GNU_SOURCE
#include <string.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>

#include "defs.h"
#include "loader.h"
#include "regs.h"
#include "mem.h"
#include "hw.h"
#include "rtc.h"
#include "rc.h"
#include "lcd.h"
#include "inflate.h"
#include "miniz.h"
#define XZ_USE_CRC64
#include "xz/xz.h"
#include "save.h"
#include "sound.h"
#include "sys.h"

#ifdef UNDER_CE
#include "ce_log.h"
#include "ce_sys.h"
#define SAVE_LOG CeLog
#else
#define SAVE_LOG(...) ((void)0)
/* No way to check a file without opening it: "present" here just means
 * "try fopen()", and a failed fopen() counts as no file, as before. */
#define SYS_FILE_ABSENT   0
#define SYS_FILE_PRESENT  1
#endif

static int mbc_table[256] =
{
	0, 1, 1, 1, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 3,
	3, 3, 3, 3, 0, 0, 0, 0, 0, 5, 5, 5, MBC_RUMBLE, MBC_RUMBLE, MBC_RUMBLE, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, MBC_HUC3, MBC_HUC1
};

static int rtc_table[256] =
{
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
	1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0
};

static int batt_table[256] =
{
	0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0,
	1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0,
	0
};

static int romsize_table[256] =
{
	2, 4, 8, 16, 32, 64, 128, 256, 512,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 128, 128, 128
	/* 0, 0, 72, 80, 96  -- actual values but bad to use these! */
};

static int ramsize_table[256] =
{
	1, 1, 1, 4, 16,
	4 /* FIXME - what value should this be?! */
};


static char *bootroms[2];
static char *romfile;
static char *sramfile;
static char *rtcfile;
static char *saveprefix;

/* sramfile/rtcfile/statefile are the whole ROM file name, extension
 * included, plus .srm / .rtc / .state (Zelda.DX.gbc -> Zelda.DX.gbc.srm),
 * the same names as the sister Pop* ports. Up to PopGB v1.0.3 the saves
 * were saveprefix - the ROM file name cut at its first '.' - plus .sav /
 * .rtc / .000. A legacy file is only read, and only while the new one
 * doesn't exist; it is never written or removed. legacyrtcfile is NULL
 * when it is the same name as rtcfile. State slots other than 0 (unused
 * by the CE frontend) keep saveprefix + .001, .002, ... */
static char *statefile;
static char *legacysramfile;
static char *legacyrtcfile;

/* The cartridge RAM as the .srm/.sav file held it when it was last read
 * or written, or (while there is no save file) as it was right after
 * loading - sram_save_if_changed() skips the write while the RAM still
 * matches it. */
static byte *sram_shadow;
static int sram_shadow_valid;
/* 1 while this game has a save file (.srm, or a legacy .sav). */
static int sram_file_exists;
/* 1 while this game has a .rtc file (new or legacy name). */
static int rtc_file_exists;
/* 0 when the .rtc is there but couldn't be read in full - the clock is
 * then never written for this ROM, same as ram.loaded for the SRAM. */
static int rtc_save_ok;

static char *savename;
static char *savedir;

static int saveslot;

static int forcebatt, nobatt;
static int forcedmg, gbamode;

static int memfill = -1, memrand = -1;


static void initmem(void *mem, int size)
{
	char *p = mem;
	if (memrand >= 0)
	{
		srand(memrand ? memrand : time(0));
		while(size--) *(p++) = rand();
	}
	else if (memfill >= 0)
		memset(p, memfill, size);
}

static byte *loadfile(FILE *f, int *len)
{
	int c, l = 0, p = 0;
	byte *d = 0, buf[4096];

	for(;;)
	{
		c = fread(buf, 1, sizeof buf, f);
		if (c <= 0) break;
		l += c;
		d = realloc(d, l);
		if (!d) return 0;
		memcpy(d+p, buf, c);
		p += c;
	}
	*len = l;
	return d;
}

static byte *inf_buf;
static int inf_pos, inf_len;
static char* loader_error;

char* loader_get_error(void)
{
	return loader_error;
}

void loader_set_error(char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	loader_error = strdup(buf);
}

static int inflate_callback(byte b)
{
	if (inf_pos >= inf_len)
	{
		inf_len += 512;
		inf_buf = realloc(inf_buf, inf_len);
		if (!inf_buf) {
			loader_set_error("out of memory inflating file @ %d bytes\n", inf_pos);
			return -1;
		}
	}
	inf_buf[inf_pos++] = b;
	return 0;
}

typedef int (*unzip_or_inflate_func) (const unsigned char *data, long *p, int (* callback) (unsigned char d));

static byte *gunzip_or_inflate(byte *data, int *len, unsigned offset,
	unzip_or_inflate_func func)
{
	long pos = 0;
	inf_buf = 0;
	inf_pos = inf_len = 0;
	if (func(data+offset, &pos, inflate_callback) < 0)
		return data;
	free(data);
	*len = inf_pos;
	return inf_buf;
}

static byte *gunzip(byte *data, int *len) {
	return gunzip_or_inflate(data, len, 0, unzip);
}

/* primitive pkzip decompressor. it can only decompress the first
   file in a zip archive. */
static byte *pkunzip(byte *data, int *len) {
	unsigned short fnl, el, comp;
	unsigned int st;
	void *new;
	size_t newlen;
	int oldlen = *len;
	if (*len < 128) return data;
	memcpy(&comp, data+8, 2);
	comp = LIL(comp);
	if(comp != 0 && comp != 8) return data;
	memcpy(&fnl, data+26, 2);
	memcpy(&el, data+28, 2);
	fnl = LIL(fnl);
	el = LIL(el);
	st = 30 + fnl + el;
	if(*len < st) return data;
	if(comp == 0) {
		inf_buf = realloc(NULL, *len - st);
		memcpy(inf_buf, data+st, *len - st);
		free(data);
		inf_len = *len = *len - st;
		return inf_buf;
	}
	*len -= st;
	newlen = 0;
	new = tinfl_decompress_mem_to_heap(data+st, *len, &newlen, 0);
	if(new) {
		*len = newlen;
		free(data);
		return new;
	}
	*len = oldlen;
	return data;
}

static int write_dec(byte *data, int len) {
	int i;
	for(i=0; i < len; i++)
		if(inflate_callback(data[i])) return -1;
	return 0;
}

static int unxz(byte *data, int len) {
	struct xz_buf b;
	struct xz_dec *s;
	enum xz_ret ret;
	unsigned char out[4096];

	/*
	 * Support up to 64 MiB dictionary. The actually needed memory
	 * is allocated once the headers have been parsed.
	*/
	s = xz_dec_init(XZ_DYNALLOC, 1 << 26);
	if(!s) goto err;

	b.in = data;
	b.in_pos = 0;
	b.in_size = len;
	b.out = out;
	b.out_pos = 0;
	b.out_size = sizeof(out);

	while (1) {
		ret = xz_dec_run(s, &b);
		if(b.out_pos == sizeof(out)) {
			if(write_dec(out, sizeof(out))) goto err;
			b.out_pos = 0;
		}

		if(ret == XZ_OK) continue;

		if(write_dec(out, b.out_pos)) goto err;

		if(ret == XZ_STREAM_END) {
			xz_dec_end(s);
			return 0;
		}
		goto err;
	}

	err:
	xz_dec_end(s);
	return -1;
}

static byte *do_unxz(byte *data, int *len) {
	xz_crc32_init();
	xz_crc64_init();
	inf_buf = 0;
	inf_pos = inf_len = 0;
	if (unxz(data, *len) < 0)
		return data;
	free(data);
	*len = inf_pos;
	return inf_buf;
}

static byte *decompress(byte *data, int *len)
{
	if (data[0] == 0x1f && data[1] == 0x8b)
		return gunzip(data, len);
	if (data[0] == 0xFD && !memcmp(data+1, "7zXZ", 4))
		return do_unxz(data, len);
	if (data[0] == 'P' && !memcmp(data+1, "K\03\04", 3))
		return pkunzip(data, len);
	return data;
}

static FILE* rom_loadfile(char *fn, byte** data, int *len) {
	FILE *f;
	if (strcmp(fn, "-")) f = fopen(fn, "rb");
	else f = stdin;
	if (!f) {
	err:
		loader_set_error("cannot open rom file: %s\n", fn);
		return f;
	}
	*data = loadfile(f, len);
	if (!*data) {
		fclose(f);
		f = 0;
		goto err;
	}
	*data = decompress(*data, len);
	return f;
}

int bootrom_load() {
	byte *data;
	int len;
	FILE *f;
	REG(RI_BOOT) = 0xff;
	if (!bootroms[hw.cgb] || !bootroms[hw.cgb][0]) return 0;
	f = rom_loadfile(bootroms[hw.cgb], &data, &len);
	if(!f) return -1;
	bootrom.bank = realloc(data, 16384);
	memset(bootrom.bank[0]+len, 0xff, 16384-len);
	memcpy(bootrom.bank[0]+0x100, rom.bank[0]+0x100, 0x100);
	fclose(f);
	REG(RI_BOOT) = 0xfe;
	return 0;
}

/* memory allocation breakdown:
   loadfile returns local buffer retrieved via realloc()
   it's called only by rom_loadfile.
     rom_loadfile is called by bootrom_load and rom_load.
      bootrom_load is called once per romfile load via loader_init from
      load_rom_and_rc, and mem ends up in bootrom.bank, loader_unload() frees it.
      rom_load is called by rom_load_simple and loader_init().
       rom_load_simple is only called by rominfo in main.c, which we can ignore.
       the mem allocated by loadfile thru loader_init/rom_load ends up in
       rom.bank, which is freed in loader_unload(), just like the malloc'd
       rom.sbank.
   where it gets complicated is when rom_loadfile uncompresses data.
   the allocation returned by loadfile is passed to decompress().
   if it fails, it returns the original loadfile allocation, on success
   it returns a pointer to inf_buf which contains the uncompressed data.
*/

int rom_load()
{
	FILE *f;
	byte c, *data, *header;
	int len = 0, rlen;
	f = rom_loadfile(romfile, &data, &len);
	if(!f) return -1;
	header = data;

	memcpy(rom.name, header+0x0134, 16);
	if (rom.name[14] & 0x80) rom.name[14] = 0;
	if (rom.name[15] & 0x80) rom.name[15] = 0;
	rom.name[16] = 0;

	c = header[0x0147];
	mbc.type = mbc_table[c];
	mbc.batt = (batt_table[c] && !nobatt) || forcebatt;
	rtc.batt = rtc_table[c];
	mbc.romsize = romsize_table[header[0x0148]];
	mbc.ramsize = ramsize_table[header[0x0149]];

	if (!mbc.romsize) {
		loader_set_error("unknown ROM size %02X\n", header[0x0148]);
		return -1;
	}
	if (!mbc.ramsize) {
		loader_set_error("unknown SRAM size %02X\n", header[0x0149]);
		return -1;
	}

	rlen = 16384 * mbc.romsize;

	c = header[0x0143];

	/* from this point on, we may no longer access data and header */
	rom.bank = realloc(data, rlen);
	if (rlen > len) memset(rom.bank[0]+len, 0xff, rlen - len);

	ram.sbank = malloc(8192 * mbc.ramsize);

	initmem(ram.sbank, 8192 * mbc.ramsize);
	initmem(ram.ibank, 4096 * 8);

	mbc.rombank = 1;
	mbc.rambank = 0;

	hw.cgb = ((c == 0x80) || (c == 0xc0)) && !forcedmg;
	hw.gba = (hw.cgb && gbamode);

	if (strcmp(romfile, "-")) fclose(f);

	return 0;
}

int rom_load_simple(char *fn) {
	romfile = fn;
	return rom_load();
}

/* Reads the save into the cartridge RAM. ram.loaded is the "saving is
 * allowed for this ROM" flag: it is set when the save was read in full,
 * or when there is no save file at all (a new game). When the file is
 * there but can't be opened or read in full, it stays 0 and nothing is
 * ever written for this ROM, so the file isn't overwritten with whatever
 * the RAM happened to hold. */
int sram_load()
{
	FILE *f;
	char *path = sramfile;
	int state, size, got;

	ram.loaded = 0;
	sram_file_exists = 0;
	sram_shadow_valid = 0;

	if (!mbc.batt || !sramfile || !*sramfile) return -1;

	size = 8192 * mbc.ramsize;
	free(sram_shadow);
	sram_shadow = malloc(size);

#ifdef UNDER_CE
	state = sys_file_state(sramfile);
	if (state == SYS_FILE_ABSENT && legacysramfile)
	{
		state = sys_file_state(legacysramfile);
		path = legacysramfile;
	}
	if (state == SYS_FILE_UNKNOWN)
	{
		SAVE_LOG("sram_load: can't tell whether %s exists - saving is off for this ROM", path);
		return -1;
	}
#else
	state = SYS_FILE_PRESENT;
#endif
	if (state == SYS_FILE_ABSENT)
	{
		SAVE_LOG("sram_load: no save file yet: %s", sramfile);
		ram.loaded = 1;
		goto remember;
	}

	f = fopen(path, "rb");
	if (!f)
	{
#ifdef UNDER_CE
		SAVE_LOG("sram_load: %s is there but can't be opened (err=%lu) - saving is off for this ROM",
			path, sys_last_error());
		return -1;
#else
		/* No way to tell "absent" from "can't open" here - same as
		 * before: treat it as a new game. */
		ram.loaded = 1;
		goto remember;
#endif
	}
	got = fread(ram.sbank, 1, size, f);
	fclose(f);
	if (got != size)
	{
		SAVE_LOG("sram_load: read only %d of %d bytes from %s - saving is off for this ROM",
			got, size, path);
		return -1;
	}
	SAVE_LOG("sram_load: read %d bytes from %s", got, path);
	ram.loaded = 1;
	sram_file_exists = 1;

remember:
	if (sram_shadow)
	{
		memcpy(sram_shadow, ram.sbank, size);
		sram_shadow_valid = 1;
	}
	return sram_file_exists ? 0 : -1;
}


static void rtc_write_file(void)
{
	FILE *f;
	if (!rtc.batt || !rtcfile || !rtc_save_ok) return;
	if (!(f = fopen(rtcfile, "wb")))
	{
		SAVE_LOG("rtc_save: can't open %s for writing", rtcfile);
		return;
	}
	rtc_save_internal(f);
	fclose(f);
	rtc_file_exists = 1;
	SAVE_LOG("rtc_save: wrote %s", rtcfile);
}

/* Writes the cartridge RAM to the .srm file, and the clock to the .rtc
 * file with it (a clock cartridge's save and clock belong together). */
int sram_save()
{
	FILE *f;
	int size, wrote, closeErr;

	/* If we crash before we ever loaded sram, DO NOT SAVE! */
	if (!mbc.batt || !sramfile || !ram.loaded || !mbc.ramsize)
		return -1;

	size = 8192 * mbc.ramsize;
	f = fopen(sramfile, "wb");
	if (!f)
	{
		sram_shadow_valid = 0;
		SAVE_LOG("sram_save: can't open %s for writing", sramfile);
		return -1;
	}
	wrote = fwrite(ram.sbank, 1, size, f);
	closeErr = fclose(f);
	sram_file_exists = 1; /* "wb" created or truncated it, even if the write then failed */
	if (wrote == size && closeErr == 0 && sram_shadow)
	{
		memcpy(sram_shadow, ram.sbank, size);
		sram_shadow_valid = 1;
	}
	else
		sram_shadow_valid = 0;
	SAVE_LOG("sram_save: wrote %d of %d bytes to %s", wrote, size, sramfile);

	rtc_write_file();
	return (wrote == size && closeErr == 0) ? 0 : -1;
}

/* Exit (and die()): writes only when the cartridge RAM differs from what
 * the save file holds - or, while there is none, from what it held right
 * after loading. Returns 1 if it wrote (the .rtc with it), 0 if not. */
int sram_save_if_changed(void)
{
	if (!mbc.batt || !sramfile || !ram.loaded || !mbc.ramsize)
		return 0;
	if (sram_shadow_valid &&
	    memcmp(sram_shadow, ram.sbank, 8192 * mbc.ramsize) == 0)
	{
		SAVE_LOG("sram_save_if_changed: unchanged, not written");
		return 0;
	}
	sram_save();
	return 1;
}

/* Opening the menu and switching ROMs: always writes while the game has
 * a save file; while it doesn't, only once the game has changed its RAM,
 * so a game that never saves gets no file. Returns 1 if it wrote. */
int sram_save_checkpoint(void)
{
	if (!mbc.batt || !sramfile || !ram.loaded || !mbc.ramsize)
		return 0;
	if (!sram_file_exists)
		return sram_save_if_changed();
	sram_save();
	return 1;
}


/* Path of the most recent state_save()/state_load() attempt, success or
 * not - loader_get_last_state_path() exposes it so a frontend can log
 * exactly what was tried when ok==0 (fopen() failing is otherwise silent:
 * no errno/GetLastError equivalent survives across this toolchain's CRT
 * in a way worth relying on, so the path itself is the most useful thing
 * to report). */
static char s_lastStatePath[512];

const char *loader_get_last_state_path(void)
{
	return s_lastStatePath;
}

/* Slot 0 is statefile; the other slots keep the legacy saveprefix.NNN
 * names. Returns a malloc()ed path. */
static char *state_name(int n)
{
	char *name;
	if (n == 0) return strdup(statefile);
	name = malloc(strlen(saveprefix) + 5);
	sprintf(name, "%s.%03d", saveprefix, n);
	return name;
}

int state_save(int n)
{
	FILE *f;
	char *name;
	int ok = 0;

	if (n < 0) n = saveslot;
	if (n < 0) n = 0;
	name = state_name(n);
	snprintf(s_lastStatePath, sizeof s_lastStatePath, "%s", name);

	if ((f = fopen(name, "wb")))
	{
		savestate(f);
		fclose(f);
		ok = 1;
	}
	free(name);
	return ok;
}


int state_load(int n)
{
	FILE *f;
	char *name;
	int ok = 0;

	if (n < 0) n = saveslot;
	if (n < 0) n = 0;
	name = state_name(n);

	/* Slot 0: while there is no .state, read the legacy .000 instead
	 * (never written or removed). A .state that is there but can't be
	 * opened is a failed load, not a reason to fall back. */
	if (n == 0)
	{
#ifdef UNDER_CE
		int state = sys_file_state(name);
		if (state == SYS_FILE_ABSENT)
#else
		if (!(f = fopen(name, "rb")) || fclose(f))
#endif
		{
			free(name);
			name = malloc(strlen(saveprefix) + 5);
			sprintf(name, "%s.%03d", saveprefix, 0);
			SAVE_LOG("state_load: no .state yet, trying %s", name);
		}
	}
	snprintf(s_lastStatePath, sizeof s_lastStatePath, "%s", name);

	if ((f = fopen(name, "rb")))
	{
		loadstate(f);
		fclose(f);
		vram_dirty();
		pal_dirty();
		sound_dirty();
		mem_updatemap();
		ok = 1;
	}
	free(name);
	return ok;
}

/* Writes the clock only while the game already has a .srm or a .rtc, so
 * a game that never saves gets neither. A clock cartridge without
 * battery-backed RAM (mbc.batt == 0, e.g. type 0x0F) has no .srm to go
 * by, so its clock is written as before. */
void rtc_save()
{
	if (!rtc.batt) return;
	if (!rtc_file_exists && !sram_file_exists && mbc.batt)
	{
		SAVE_LOG("rtc_save: no .srm/.rtc yet, not written");
		return;
	}
	rtc_write_file();
}

void rtc_load()
{
	FILE *f;
	char *path = rtcfile;
	int state = SYS_FILE_PRESENT, batt = rtc.batt, n;

	/* Start every ROM from a stopped-at-zero clock, so a clock game with
	 * no .rtc doesn't carry on with the previous game's clock. */
	memset(&rtc, 0, sizeof rtc);
	rtc.batt = batt;
	rtc_file_exists = 0;
	rtc_save_ok = 1;

	if (!rtc.batt || !rtcfile) return;

#ifdef UNDER_CE
	state = sys_file_state(rtcfile);
	if (state == SYS_FILE_ABSENT && legacyrtcfile)
	{
		state = sys_file_state(legacyrtcfile);
		path = legacyrtcfile;
	}
	if (state == SYS_FILE_UNKNOWN)
	{
		SAVE_LOG("rtc_load: can't tell whether %s exists - the clock is not saved for this ROM", path);
		rtc_save_ok = 0;
		return;
	}
#endif
	if (state == SYS_FILE_ABSENT)
	{
		SAVE_LOG("rtc_load: no .rtc yet: %s", rtcfile);
		return;
	}

	if (!(f = fopen(path, "r")))
	{
#ifdef UNDER_CE
		SAVE_LOG("rtc_load: %s is there but can't be opened (err=%lu) - the clock is not saved for this ROM",
			path, sys_last_error());
		rtc_save_ok = 0;
#endif
		return;
	}
	n = rtc_load_internal(f);
	fclose(f);
	if (n != 8)
	{
		SAVE_LOG("rtc_load: read only %d of 8 values from %s - the clock is not saved for this ROM",
			n, path);
		rtc_save_ok = 0;
		return;
	}
	rtc_file_exists = 1;
	SAVE_LOG("rtc_load: read %s", path);
}

#define FREENULL(X) do { free(X); X = 0; } while(0)
void loader_unload()
{
	/* Switching ROMs: the .rtc goes with the .srm when that is written,
	 * otherwise on its own (rtc_save() still needs a .srm or .rtc). */
	if (!sram_save_checkpoint())
		rtc_save();
	ram.loaded = 0;
	sram_file_exists = rtc_file_exists = 0;
	sram_shadow_valid = 0;
	if (sram_shadow) FREENULL(sram_shadow);
	if (romfile) FREENULL(romfile);
	if (sramfile) FREENULL(sramfile);
	if (rtcfile) FREENULL(rtcfile);
	if (statefile) FREENULL(statefile);
	if (legacysramfile) FREENULL(legacysramfile);
	if (legacyrtcfile) FREENULL(legacyrtcfile);
	if (saveprefix) FREENULL(saveprefix);
	if (rom.bank) FREENULL(rom.bank);
	if (ram.sbank) FREENULL(ram.sbank);
	if (bootrom.bank) FREENULL(bootrom.bank);
	mbc.type = mbc.romsize = mbc.ramsize = mbc.batt = 0;
}

static char *base(char *s)
{
	char *p;
	p = strrchr(s, '/');
#ifdef ALT_PATH_SEP
	/* Windows-style paths (this port's CE frontend passes romfile as a
	 * full "\Storage Card\..." path, never '/') - a real-hardware log
	 * caught this function returning the whole path unstripped because
	 * it only ever looked for '/', which then got glued onto savedir
	 * as one path (e.g. "...\PopGB/\Storage Card\GB\Name.000"),
	 * never matching any real file. ALT_PATH_SEP is already this
	 * codebase's own marker for "this target uses backslash paths"
	 * (see path.c). */
	{
		char *q = strrchr(s, '\\');
		if (!p || (q && q > p)) p = q;
	}
#endif
	if (p) return p+1;
	return s;
}

static char *ldup(char *s)
{
	int i;
	char *n, *p;
	p = n = malloc(strlen(s));
	for (i = 0; s[i]; i++) if (isalnum(s[i])) *(p++) = tolower(s[i]);
	*p = 0;
	return n;
}

static void cleanup()
{
	if (!sram_save_if_changed())
		rtc_save();
	/* IDEA - if error, write emergency savestate..? */
}

static char *joinname(const char *prefix, const char *ext)
{
	char *n = malloc(strlen(prefix) + strlen(ext) + 1);
	strcpy(n, prefix);
	strcat(n, ext);
	return n;
}

int loader_init(char *s)
{
	char *name, *newname = 0, *p, *newprefix;

	sys_checkdir(savedir, 1); /* needs to be writable */

	romfile = s;
	if(rom_load()) return -1;
	bootrom_load();
	vid_settitle(rom.name);
	if (savename && *savename)
	{
		if (savename[0] == '-' && savename[1] == 0)
			name = ldup(rom.name);
		else name = strdup(savename);
	}
	else if (romfile && *base(romfile) && strcmp(romfile, "-"))
	{
		name = strdup(base(romfile));
		newname = strdup(name);
		p = strchr(name, '.');
		if (p) *p = 0;
	}
	else name = ldup(rom.name);
	if (!newname) newname = strdup(name);

	saveprefix = malloc(strlen(savedir) + strlen(name) + 2);
	sprintf(saveprefix, "%s/%s", savedir, name);
	newprefix = malloc(strlen(savedir) + strlen(newname) + 2);
	sprintf(newprefix, "%s/%s", savedir, newname);

	sramfile = joinname(newprefix, ".srm");
	rtcfile = joinname(newprefix, ".rtc");
	statefile = joinname(newprefix, ".state");
	legacysramfile = joinname(saveprefix, ".sav");
	legacyrtcfile = joinname(saveprefix, ".rtc");
	if (!strcmp(legacyrtcfile, rtcfile)) FREENULL(legacyrtcfile);

	free(newprefix);
	free(newname);
	free(name);

	sram_load();
	rtc_load();

	atexit(cleanup);
	return 0;
}

rcvar_t loader_exports[] =
{
	RCV_STRING("bootrom_dmg", &bootroms[0], "bootrom for DMG games"),
	RCV_STRING("bootrom_cgb", &bootroms[1], "bootrom for CGB games"),
	RCV_STRING("savedir", &savedir, "save directory"),
	RCV_STRING("savename", &savename, "base filename for saves"),
	RCV_INT("saveslot", &saveslot, "which savestate slot to use"),
	RCV_BOOL("forcebatt", &forcebatt, "save SRAM even on carts w/o battery"),
	RCV_BOOL("nobatt", &nobatt, "never save SRAM"),
	RCV_BOOL("forcedmg", &forcedmg, "force DMG mode for CGB carts"),
	RCV_BOOL("gbamode", &gbamode, "simulate cart being used on a GBA"),
	RCV_INT("memfill", &memfill, ""),
	RCV_INT("memrand", &memrand, ""),
	RCV_END
};









