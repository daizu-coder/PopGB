

#ifndef __LOADER_H__
#define __LOADER_H__


typedef struct loader_s
{
	char *rom;
	char *base;
	char *sram;
	char *state;
	int ramloaded;
} loader_t;


extern loader_t loader;


int rom_load();
int sram_load();
int sram_save();

/* Save checkpoints (see loader.c): sram_save_if_changed() writes only
 * when the cartridge RAM differs from the save file (exit), and
 * sram_save_checkpoint() always writes while the game has a save file
 * but otherwise only once the RAM has changed (menu, ROM switch). Both
 * return 1 if they wrote - the .rtc is then written with it - and 0 if
 * not, in which case the caller can still write the clock alone with
 * rtc_save(). */
int sram_save_if_changed(void);
int sram_save_checkpoint(void);
void rtc_save(void);
void rtc_load(void);

int loader_init(char *s);
void loader_unload(void);
char *loader_get_error();
void loader_set_error(char *fmt, ...);

/* Return 1 on success, 0 if the state file couldn't be opened - callers
 * that need to tell the user whether it actually worked (see the CE
 * frontend's main menu) check this instead of assuming success, the same
 * lesson an earlier prototype learned the hard way (its own Save/Load
 * gave no feedback at first). */
int state_save(int n);
int state_load(int n);

/* Path attempted by the most recent state_save()/state_load() call - see
 * loader.c for why this exists (diagnosing an fopen() failure that is
 * otherwise silent). */
const char *loader_get_last_state_path(void);

#endif


