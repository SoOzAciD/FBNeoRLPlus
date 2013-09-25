#include "libretro.h"
#include "burner.h"
#include "input/inp_keys.h"
#include "state.h"
#include <string.h>
#include <stdio.h>

#include <vector>
#include <string>
#include <ctype.h>

#include "cd/cd_interface.h"

#define FBA_VERSION "v0.2.97.30" // Sept 16, 2013 (SVN)

// FBARL ---
bool g_opt_bUseUNIBIOS = false;

int iniRead();
int getBoolOption(FILE* fp, char* option, bool *bOption);
// ---

static unsigned int BurnDrvGetIndexByName(const char* name);

#define STAT_NOFIND	0
#define STAT_OK		1
#define STAT_CRC	   2
#define STAT_SMALL	3
#define STAT_LARGE	4

struct ROMFIND
{
	unsigned int nState;
	int nArchive;
	int nPos;
   BurnRomInfo ri;
};

static std::vector<std::string> g_find_list_path;
static ROMFIND g_find_list[1024];
static unsigned g_rom_count;

#define AUDIO_SAMPLERATE 32000
#define AUDIO_SEGMENT_LENGTH 534 // <-- Hardcoded value that corresponds well to 32kHz audio.

static uint32_t g_fba_frame[1024 * 1024];
static int16_t g_audio_buf[AUDIO_SEGMENT_LENGTH * 2];

// libretro globals

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t input_cb;
static retro_audio_sample_batch_t audio_batch_cb;
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }
void retro_set_environment(retro_environment_t cb) { environ_cb = cb; }

char g_rom_dir[1024];
static bool driver_inited;

void retro_get_system_info(struct retro_system_info *info)
{
   info->library_name = "FB Alpha";
   info->library_version = FBA_VERSION;
   info->need_fullpath = true;
   info->block_extract = true;
   info->valid_extensions = "iso|ISO|zip|ZIP";
}

/////
static void poll_input();
static bool init_input();

// FBA stubs
unsigned ArcadeJoystick;

int bDrvOkay;
int bRunPause;
bool bAlwaysProcessKeyboardInput;

bool bDoIpsPatch;
void IpsApplyPatches(UINT8 *, char *) {}

TCHAR szAppHiscorePath[MAX_PATH];
TCHAR szAppSamplesPath[MAX_PATH];
TCHAR szAppBurnVer[16];

CDEmuStatusValue CDEmuStatus;

const char* isowavLBAToMSF(const int LBA) { return ""; }
int isowavMSFToLBA(const char* address) { return 0; }
TCHAR* GetIsoPath() { return NULL; }
INT32 CDEmuInit() { return 0; }
INT32 CDEmuExit() { return 0; }
INT32 CDEmuStop() { return 0; }
INT32 CDEmuPlay(UINT8 M, UINT8 S, UINT8 F) { return 0; }
INT32 CDEmuLoadSector(INT32 LBA, char* pBuffer) { return 0; }
UINT8* CDEmuReadTOC(INT32 track) { return 0; }
UINT8* CDEmuReadQChannel() { return 0; }
INT32 CDEmuGetSoundBuffer(INT16* buffer, INT32 samples) { return 0; }

static int nDIPOffset;

static void InpDIPSWGetOffset (void)
{
	BurnDIPInfo bdi;
	nDIPOffset = 0;

	for(int i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
	{
		if (bdi.nFlags == 0xF0)
		{
			nDIPOffset = bdi.nInput;
			break;
		}
	}
}

void InpDIPSWResetDIPs (void)
{
	int i = 0;
	BurnDIPInfo bdi;
	struct GameInp * pgi = NULL;

	InpDIPSWGetOffset();

	while (BurnDrvGetDIPInfo(&bdi, i) == 0)
	{
		if (bdi.nFlags == 0xFF)
		{
			pgi = GameInp + bdi.nInput + nDIPOffset;
			if (pgi)
				pgi->Input.Constant.nConst = (pgi->Input.Constant.nConst & ~bdi.nMask) | (bdi.nSetting & bdi.nMask);	
		}
		i++;
	}
}

int InputSetCooperativeLevel(const bool bExclusive, const bool bForeGround) { return 0; }

void Reinitialise(void)
{
#if 0 // ?!
	int width, height;
	BurnDrvGetVisibleSize(&width, &height);
	unsigned drv_flags = BurnDrvGetFlags();
	if (drv_flags & BDF_ORIENTATION_VERTICAL)
		nBurnPitch = height * sizeof(uint16_t);
	else
		nBurnPitch = width * sizeof(uint16_t);

	if (environ_cb)
	{
		BurnDrvGetVisibleSize(&width, &height);
		retro_geometry geom = { width, height, width, height };
		environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
		environ_cb(RETRO_ENVIRONMENT_SET_PITCH, &nBurnPitch);
	}
#endif
}

// Non-idiomatic (OutString should be to the left to match strcpy())
// Seems broken to not check nOutSize.
char* TCHARToANSI(const TCHAR* pszInString, char* pszOutString, int /*nOutSize*/)
{
   if (pszOutString)
   {
      strcpy(pszOutString, pszInString);
      return pszOutString;
   }

   return (char*)pszInString;
}

int QuoteRead(char **, char **, char*) { return 1; }
char *LabelCheck(char *, char *) { return 0; }
const int nConfigMinVersion = 0x020921;

//void initlog() {
//	FILE* fp = fopen("/dev_hdd0/game/FBAL00123/USRDIR/debug.txt", "w");
//	if(fp) { fclose(fp); *&fp = NULL; }
//}

//void dlog(char* x) {
//	FILE* fp = fopen("/dev_hdd0/game/FBAL00123/USRDIR/debug.txt", "a");
//	if(fp) { fprintf(fp, x); fclose(fp); *&fp = NULL; }
//}

// addition to support loading of roms without crc check
static int find_rom_by_name(char *name, const ZipEntry *list, unsigned elems)
{
	unsigned i = 0;
	for (i = 0; i < elems; i++)
	{
		if( strcmp(list[i].szName, name) == 0 ) 
		{
			return i; 
		}
	}
	//char msg[256] = "";
	//sprintf(msg, "Not found: %s (name = %s)\n", list[i].szName, name);
	//dlog(msg);

	return -1;
}

static int find_rom_by_crc(uint32_t crc, const ZipEntry *list, unsigned elems)
{
	unsigned i = 0;
   for (i = 0; i < elems; i++)
   {
      if (list[i].nCrc == crc)
	  {
         return i;
	  }
   }
   //char msg[256] = "";
   //sprintf(msg, "Not found: 0x%X (crc: 0x%X)\n", list[i].nCrc, crc);
   //dlog(msg);
   
   return -1;
}

static void free_archive_list(ZipEntry *list, unsigned count)
{
   if (list)
   {
      for (unsigned i = 0; i < count; i++)
         free(list[i].szName);
      free(list);
   }
}

static int archive_load_rom(uint8_t *dest, int *wrote, int i)
{
   if (i < 0 || i >= g_rom_count)
      return 1;

   int archive = g_find_list[i].nArchive;

   if (ZipOpen((char*)g_find_list_path[archive].c_str()) != 0)
      return 1;

   BurnRomInfo ri = {0};
   BurnDrvGetRomInfo(&ri, i);

   if (ZipLoadFile(dest, ri.nLen, wrote, g_find_list[i].nPos) != 0)
   {
      ZipClose();
      return 1;
   }

   ZipClose();
   return 0;
}

// This code is very confusing. The original code is even more confusing :(
static bool open_archive()
{
	memset(g_find_list, 0, sizeof(g_find_list));

	// FBA wants some roms ... Figure out how many.
	g_rom_count = 0;
	while (!BurnDrvGetRomInfo(&g_find_list[g_rom_count].ri, g_rom_count))
		g_rom_count++;

	g_find_list_path.clear();
	
	// Check if we have said archives.
	// Check if archives are found. These are relative to g_rom_dir.
	char *rom_name;
	for (unsigned index = 0; index < 32; index++)
	{
		if (BurnDrvGetZipName(&rom_name, index))
			continue;

		fprintf(stderr, "[FBA] Archive: %s\n", rom_name);

		char path[1024];
#ifdef _XBOX
		snprintf(path, sizeof(path), "%s\\%s", g_rom_dir, rom_name);
#else
		snprintf(path, sizeof(path), "%s/%s", g_rom_dir, rom_name);
#endif

		if (ZipOpen(path) != 0)
		{
			fprintf(stderr, "[FBA] Failed to find archive: %s\n", path);
			return false;
		}
		ZipClose();

		g_find_list_path.push_back(path);
	}

	for (unsigned z = 0; z < g_find_list_path.size(); z++)
	{
		if (ZipOpen((char*)g_find_list_path[z].c_str()) != 0)
		{
			fprintf(stderr, "[FBA] Failed to open archive %s\n", g_find_list_path[z].c_str());
			return false;
		}

		ZipEntry *list = NULL;
		int count;
		ZipGetList(&list, &count);

		

		// Try to map the ROMs FBA wants to ROMs we find inside our pretty archives ...
		for (unsigned i = 0; i < g_rom_count; i++)
		{
			if (g_find_list[i].nState == STAT_OK)
				continue;

			if (g_find_list[i].ri.nType == 0 || g_find_list[i].ri.nLen == 0 || g_find_list[i].ri.nCrc == 0)
			{
				g_find_list[i].nState = STAT_OK;
				continue;
			}

			int index = -1;

			// USE UNI-BIOS...
			if(g_opt_bUseUNIBIOS) 
			{		 
				char *szPossibleName=NULL;
				BurnDrvGetRomName(&szPossibleName, i, 0);
				if(strcmp(szPossibleName, "asia-s3.rom") == 0)
				{
					if(index < 0) { index = find_rom_by_name((char*)"uni-bios_3_0.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0xA97C89A9, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_2_3o.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x601720AE, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_2_3.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x27664EB5, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_2_2.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x2D50996A, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_2_1.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x8DABF76B, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_2_0.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x0C12C2AD, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_1_3.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0xB24B44A0, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_1_2o.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0xE19D3CE9, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_1_2.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x4FA698E9, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_1_1.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x5DDA0D84, list, count); }
					if(index < 0) {	index = find_rom_by_name((char*)"uni-bios_1_0.rom", list, count); }
					if(index < 0) {	index = find_rom_by_crc(0x0CE453A0, list, count); }
					
					// uni-bios not found, try to find regular bios
					if(index < 0) {	index = find_rom_by_crc(g_find_list[i].ri.nCrc, list, count); }

				} else {
					index = find_rom_by_crc(g_find_list[i].ri.nCrc, list, count);
				}
			} else {
				index = find_rom_by_crc(g_find_list[i].ri.nCrc, list, count);
			}

			if (index < 0)
				continue;

			// Yay, we found it!
			g_find_list[i].nArchive = z;
			g_find_list[i].nPos = index;
			g_find_list[i].nState = STAT_OK;

			if (list[index].nLen < g_find_list[i].ri.nLen)
				g_find_list[i].nState = STAT_SMALL;
			else if (list[index].nLen > g_find_list[i].ri.nLen)
				g_find_list[i].nState = STAT_LARGE;
		}

		free_archive_list(list, count);
		ZipClose();
	}

	// Going over every rom to see if they are properly loaded before we continue ...
	for (unsigned i = 0; i < g_rom_count; i++)
	{
		if (g_find_list[i].nState != STAT_OK)
		{
			//char msg[256] = "";
			//sprintf(msg, "[FBA] ROM index %i was not found ... CRC: 0x%08x\n", i, g_find_list[i].ri.nCrc);
			//dlog(msg);

			fprintf(stderr, "[FBA] ROM index %i was not found ... CRC: 0x%08x\n", i, g_find_list[i].ri.nCrc);

			if(!(g_find_list[i].ri.nType & BRF_OPT)) {
				return false;
			}
		}
	}

	BurnExtLoadRom = archive_load_rom;
	return true;
}

void retro_init()
{
	iniRead();
	//initlog();

	BurnLibInit();
}

void retro_deinit()
{
   if (driver_inited)
      BurnDrvExit();
   driver_inited = false;
   BurnLibExit();
}

static bool g_reset;
void retro_reset() { g_reset = true; }

void retro_run()
{
   int width, height;
   BurnDrvGetVisibleSize(&width, &height);
   pBurnDraw = (uint8_t*)g_fba_frame;

   poll_input();

   //nBurnLayer = 0xff;
   pBurnSoundOut = g_audio_buf;
   nBurnSoundRate = AUDIO_SAMPLERATE;
   //nBurnSoundLen = AUDIO_SEGMENT_LENGTH;
   nCurrentFrame++;


   BurnDrvFrame();
   unsigned drv_flags = BurnDrvGetFlags();
   uint32_t height_tmp = height;
   size_t pitch_size = nBurnBpp == 2 ? sizeof(uint16_t) : sizeof(uint32_t);
   if (drv_flags & (BDF_ORIENTATION_VERTICAL | BDF_ORIENTATION_FLIPPED))
   {
      nBurnPitch = height * pitch_size;
      height = width;
      width = height_tmp;
   }
   else
      nBurnPitch = width * pitch_size;

   video_cb(g_fba_frame, width, height, nBurnPitch);
   audio_batch_cb(g_audio_buf, nBurnSoundLen);
}

static uint8_t *write_state_ptr;
static const uint8_t *read_state_ptr;
static unsigned state_size;

static int burn_write_state_cb(BurnArea *pba)
{
   memcpy(write_state_ptr, pba->Data, pba->nLen);
   write_state_ptr += pba->nLen;
   return 0;
}

static int burn_read_state_cb(BurnArea *pba)
{
   memcpy(pba->Data, read_state_ptr, pba->nLen);
   read_state_ptr += pba->nLen;
   return 0;
}

static int burn_dummy_state_cb(BurnArea *pba)
{
   state_size += pba->nLen;
   return 0;
}

size_t retro_serialize_size()
{
   if (state_size)
      return state_size;

   BurnAcb = burn_dummy_state_cb;
   state_size = 0;
   BurnAreaScan(ACB_VOLATILE | ACB_WRITE, 0);
   return state_size;
}

bool retro_serialize(void *data, size_t size)
{
   if (size != state_size)
      return false;

   BurnAcb = burn_write_state_cb;
   write_state_ptr = (uint8_t*)data;
   BurnAreaScan(ACB_VOLATILE | ACB_WRITE, 0);

   return true;
}

bool retro_unserialize(const void *data, size_t size)
{
   if (size != state_size)
      return false;
   BurnAcb = burn_read_state_cb;
   read_state_ptr = (const uint8_t*)data;
   BurnAreaScan(ACB_VOLATILE | ACB_READ, 0);

   return true;
}

void retro_cheat_reset() {}
void retro_cheat_set(unsigned, bool, const char*) {}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   int width, height;
   BurnDrvGetVisibleSize(&width, &height);
   int maximum = width > height ? width : height;
   struct retro_game_geometry geom = { width, height, maximum, maximum };

   struct retro_system_timing timing = { (nBurnFPS / 100.0), (nBurnFPS / 100.0) * AUDIO_SEGMENT_LENGTH };

   info->geometry = geom;
   info->timing   = timing;
}

int VidRecalcPal()
{
   return BurnRecalcPal();
}

static bool fba_init(unsigned driver, const char *game_zip_name)
{
   nBurnDrvActive = driver;

   if (!open_archive())
      return false;

   nBurnBpp = 2;
   nFMInterpolation = 3;
   nInterpolation = 3;

   BurnDrvInit();

   int width, height;
   BurnDrvGetVisibleSize(&width, &height);
   unsigned drv_flags = BurnDrvGetFlags();
   size_t pitch_size = nBurnBpp == 2 ? sizeof(uint16_t) : sizeof(uint32_t);
   if (drv_flags & BDF_ORIENTATION_VERTICAL)
      nBurnPitch = height * pitch_size;
   else
      nBurnPitch = width * pitch_size;

   unsigned rotation;
   switch (drv_flags & (BDF_ORIENTATION_FLIPPED | BDF_ORIENTATION_VERTICAL))
   {
      case BDF_ORIENTATION_VERTICAL:
         rotation = 1;
         break;

      case BDF_ORIENTATION_FLIPPED:
         rotation = 2;
         break;

      case BDF_ORIENTATION_VERTICAL | BDF_ORIENTATION_FLIPPED:
         rotation = 3;
         break;

      default:
         rotation = 0;
   }

   if(
         (strcmp("gunbird2", game_zip_name) == 0) ||
         (strcmp("s1945ii", game_zip_name) == 0) ||
         (strcmp("s1945iii", game_zip_name) == 0) ||
         (strcmp("dragnblz", game_zip_name) == 0) ||
         (strcmp("gnbarich", game_zip_name) == 0) ||
         (strcmp("mjgtaste", game_zip_name) == 0) ||
         (strcmp("tgm2", game_zip_name) == 0) ||
         (strcmp("tgm2p", game_zip_name) == 0) ||
         (strcmp("soldivid", game_zip_name) == 0) ||
         (strcmp("daraku", game_zip_name) == 0) ||
         (strcmp("sbomber", game_zip_name) == 0) ||
         (strcmp("sbombera", game_zip_name) == 0) 

         )
   {
      nBurnBpp = 4;
   }
   fprintf(stderr, "Game: %s\n", game_zip_name);

   environ_cb(RETRO_ENVIRONMENT_SET_ROTATION, &rotation);

   VidRecalcPal();

#ifdef FRONTEND_SUPPORTS_RGB565
   if(nBurnBpp == 4)
   {
      enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

      if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) 
         fprintf(stderr, "Frontend supports XRGB888 - will use that instead of XRGB1555.\n");
   }
   else
   {
      enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;

      if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) 
         fprintf(stderr, "Frontend supports RGB565 - will use that instead of XRGB1555.\n");
   }
#endif

   return true;
}

#if defined(FRONTEND_SUPPORTS_RGB565)
static unsigned int HighCol16(int r, int g, int b, int  /* i */)
{
   return (((r << 8) & 0xf800) | ((g << 3) & 0x07e0) | ((b >> 3) & 0x001f));
}
#else
static unsigned int HighCol15(int r, int g, int b, int  /* i */)
{
   return (((r << 7) & 0x7c00) | ((g << 2) & 0x03e0) | ((b >> 3) & 0x001f));
}
#endif


static void init_video()
{
}

static void extract_basename(char *buf, const char *path, size_t size)
{
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';

   char *ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char *base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
      buf[0] = '\0';
}

bool analog_controls_enabled = false;

bool retro_load_game(const struct retro_game_info *info)
{
   bool retval = false;
   char basename[128];
   extract_basename(basename, info->path, sizeof(basename));
   extract_directory(g_rom_dir, info->path, sizeof(g_rom_dir));

   unsigned i = BurnDrvGetIndexByName(basename);
   if (i < nBurnDrvCount)
   {
      pBurnSoundOut = g_audio_buf;
      nBurnSoundRate = AUDIO_SAMPLERATE;
      nBurnSoundLen = AUDIO_SEGMENT_LENGTH;

      if (!fba_init(i, basename))
         return false;

      driver_inited = true;
      analog_controls_enabled = init_input();

      retval = true;
   }
   else
      fprintf(stderr, "[FBA] Cannot find driver.\n");


   return retval;
}

bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

void retro_unload_game(void) {}

unsigned retro_get_region() { return RETRO_REGION_NTSC; }

void *retro_get_memory_data(unsigned) { return 0; }
size_t retro_get_memory_size(unsigned) { return 0; }

unsigned retro_api_version() { return RETRO_API_VERSION; }

void retro_set_controller_port_device(unsigned, unsigned) {}

// Input stuff.

// Ref GamcPlayer() in ../gamc.cpp
struct key_map
{
   const char *bii_name;
   unsigned nCode[2];
};
static uint8_t keybinds[0x5000][2]; 

//#define BIND_MAP_COUNT 151
UINT32 nBindMapCount = 0;

#define RETRO_DEVICE_ID_JOYPAD_RESET      16
#define RETRO_DEVICE_ID_JOYPAD_SERVICE    17
#define RETRO_DEVICE_ID_JOYPAD_DIAGNOSTIC 18
#define RETRO_DEVICE_ID_JOYPAD_DIP_A      19
#define RETRO_DEVICE_ID_JOYPAD_DIP_B      20
#define RETRO_DEVICE_ID_JOYPAD_TEST       21

// Macro references: gami.cpp 
// Note: Had to edit gami.cpp since strings had a multiplication symbol '�' instead of just an 'x'
void BindInputMacros()
{
	struct GameInp* pgi = GameInp + nGameInpCount; // start from macros

	//FILE* fp = NULL;
	//fp = fopen("/dev_hdd0/game/FBAL00123/USRDIR/cores/macros_dbg.txt", "w");
	for (UINT32 i = 0; i < nMacroCount; i++, pgi++) 
	{
		if (pgi->nInput & GIT_GROUP_MACRO) 
		{
			switch (pgi->nInput) 
			{
				case GIT_MACRO_AUTO:	// Auto-assigned macros
				{
					// ---------------------------------------------------------------------------------
					// Player 1 Macros

					if(strcmp(pgi->Macro.szName, "P1 3x Punch")==0)
					{
						pgi->Macro.nMode = 1;
						pgi->Macro.Switch.nCode = nBindMapCount;
						nBindMapCount++;

						keybinds[pgi->Macro.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_L;
						keybinds[pgi->Macro.Switch.nCode][1] = 0;
						//if(fp) { fprintf(fp, "[count: %d] P1 3x Punch (nCode): [ %d ] [ 0x%X ]\n", i, pgi->Macro.Switch.nCode, pgi->Macro.Switch.nCode); }
					}

					if(strcmp(pgi->Macro.szName, "P1 3x Kick")==0)
					{
						pgi->Macro.nMode = 1;
						pgi->Macro.Switch.nCode = nBindMapCount;
						nBindMapCount++;

						keybinds[pgi->Macro.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_L2;
						keybinds[pgi->Macro.Switch.nCode][1] = 0;						
						//if(fp) { fprintf(fp, "[count: %d] P1 3x Kick (nCode): [ %d ] [ 0x%X ]\n", i, pgi->Macro.Switch.nCode, pgi->Macro.Switch.nCode);	}
					}

					// ---------------------------------------------------------------------------------
					// Player 2 Macros

					if(strcmp(pgi->Macro.szName, "P2 3x Punch")==0)
					{
						pgi->Macro.nMode = 1;
						pgi->Macro.Switch.nCode = nBindMapCount;
						nBindMapCount++;

						keybinds[pgi->Macro.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_L;
						keybinds[pgi->Macro.Switch.nCode][1] = 1;
						//if(fp) { fprintf(fp, "[count: %d] P2 3x Punch (nCode): [ %d ] [ 0x%X ]\n", i, pgi->Macro.Switch.nCode, pgi->Macro.Switch.nCode); }
					}

					if(strcmp(pgi->Macro.szName, "P2 3x Kick")==0)
					{
						pgi->Macro.nMode = 1;
						pgi->Macro.Switch.nCode = nBindMapCount;
						nBindMapCount++;

						keybinds[pgi->Macro.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_L2;
						keybinds[pgi->Macro.Switch.nCode][1] = 1;						
						//if(fp) { fprintf(fp, "[count: %d] P2 3x Kick (nCode): [ %d ] [ 0x%X ]\n", i, pgi->Macro.Switch.nCode, pgi->Macro.Switch.nCode);	}
					}

					break;
				}
				case GIT_MACRO_CUSTOM:	// Custom macros
				{					
					break;
				}
				default:												// Unknown -- ignore
				{
					continue;
				}
			}
		}
	}
	
	//if(fp) { fclose(fp); }

}

static const char *print_label(unsigned i)
{
   switch(i)
   {
      case RETRO_DEVICE_ID_JOYPAD_B:
         return "RetroPad Button B";
      case RETRO_DEVICE_ID_JOYPAD_Y:
         return "RetroPad Button Y";
      case RETRO_DEVICE_ID_JOYPAD_SELECT:
         return "RetroPad Button Select";
      case RETRO_DEVICE_ID_JOYPAD_START:
         return "RetroPad Button Start";
      case RETRO_DEVICE_ID_JOYPAD_UP:
         return "RetroPad D-Pad Up";
      case RETRO_DEVICE_ID_JOYPAD_DOWN:
         return "RetroPad D-Pad Down";
      case RETRO_DEVICE_ID_JOYPAD_LEFT:
         return "RetroPad D-Pad Left";
      case RETRO_DEVICE_ID_JOYPAD_RIGHT:
         return "RetroPad D-Pad Right";
      case RETRO_DEVICE_ID_JOYPAD_A:
         return "RetroPad Button A";
      case RETRO_DEVICE_ID_JOYPAD_X:
         return "RetroPad Button X";
      case RETRO_DEVICE_ID_JOYPAD_L:
         return "RetroPad Button L";
      case RETRO_DEVICE_ID_JOYPAD_R:
         return "RetroPad Button R";
      case RETRO_DEVICE_ID_JOYPAD_L2:
         return "RetroPad Button L2";
      case RETRO_DEVICE_ID_JOYPAD_R2:
         return "RetroPad Button R2";
      case RETRO_DEVICE_ID_JOYPAD_L3:
         return "RetroPad Button L3";
      case RETRO_DEVICE_ID_JOYPAD_R3:
         return "RetroPad Button R3";
      case RETRO_DEVICE_ID_JOYPAD_RESET:
         return "RetroPad Reset";
      case RETRO_DEVICE_ID_JOYPAD_SERVICE:
         return "RetroPad Service";
      case RETRO_DEVICE_ID_JOYPAD_DIAGNOSTIC:
         return "RetroPad Diagnostic";
      case RETRO_DEVICE_ID_JOYPAD_DIP_A:
         return "RetroPad DIP A";
      case RETRO_DEVICE_ID_JOYPAD_DIP_B:
         return "RetroPad DIP B";
      case RETRO_DEVICE_ID_JOYPAD_TEST:
         return "RetroPad Test";
      default:
         return "No known label";
   }
}

#define MAX_INPUT_BIND			500		// this should be enough

#define BIND_MAP( _str, _input, _port )			\
   bind_map[nBindMapCount].bii_name = _str;		\
   bind_map[nBindMapCount].nCode[0] = _input;	\
   bind_map[nBindMapCount].nCode[1] = _port;	\
   nBindMapCount++;

static bool init_input()
{
   GameInpInit();
   GameInpDefault();

   bool has_analog = false;
   struct GameInp* pgi = GameInp;
   for (unsigned i = 0; i < nGameInpCount; i++, pgi++)
   {
      if (pgi->nType == BIT_ANALOG_REL)
      {
         has_analog = true;
         break;
      }
   }

   //needed for Neo Geo button mappings (and other drivers in future)
   const char * parentrom	= BurnDrvGetTextA(DRV_PARENT);
   const char * boardrom	= BurnDrvGetTextA(DRV_BOARDROM);
   const char * drvname		= BurnDrvGetTextA(DRV_NAME);
   INT32	genre			= BurnDrvGetGenreFlags();
   INT32	hardware		= BurnDrvGetHardwareCode();

   fprintf(stderr, "has_analog: %d\n"	, has_analog);
   fprintf(stderr, "parentrom: %s\n"	, parentrom);
   fprintf(stderr, "boardrom: %s\n"		, boardrom);
   fprintf(stderr, "drvname: %s\n"		, drvname);
   fprintf(stderr, "genre: %d\n"		, genre);
   fprintf(stderr, "hardware: %d\n"		, hardware);

   /* initialization */
   struct BurnInputInfo bii;
   memset(&bii, 0, sizeof(bii));

   // Bind to nothing.
   for (unsigned i = 0; i < 0x5000; i++) {
      keybinds[i][0] = 0xff;
   }

   pgi = GameInp;

   key_map bind_map[MAX_INPUT_BIND];

   BIND_MAP("P1 Coin"			, RETRO_DEVICE_ID_JOYPAD_SELECT, 0);
   BIND_MAP("P1 Start"			, RETRO_DEVICE_ID_JOYPAD_START, 0);
   BIND_MAP("Start 1"			, RETRO_DEVICE_ID_JOYPAD_START, 0);
   BIND_MAP("P1 Up"				, RETRO_DEVICE_ID_JOYPAD_UP, 0);
   BIND_MAP("P1 Down"			, RETRO_DEVICE_ID_JOYPAD_DOWN, 0);
   BIND_MAP("P1 Left"			, RETRO_DEVICE_ID_JOYPAD_LEFT, 0);
   BIND_MAP("P1 Right"			, RETRO_DEVICE_ID_JOYPAD_RIGHT, 0);
   BIND_MAP("P1 Attack"			, RETRO_DEVICE_ID_JOYPAD_Y, 0);
   BIND_MAP("Accelerate"		, RETRO_DEVICE_ID_JOYPAD_B, 0);
   BIND_MAP("Brake"				, RETRO_DEVICE_ID_JOYPAD_Y, 0);
   BIND_MAP("Gear"				, RETRO_DEVICE_ID_JOYPAD_A, 0);

   /* for Forgotten Worlds, etc */
   BIND_MAP("P1 Turn"			, RETRO_DEVICE_ID_JOYPAD_A, 0);
   BIND_MAP("P1 Jump"			, RETRO_DEVICE_ID_JOYPAD_B, 0);
   BIND_MAP("P1 Pin"			, RETRO_DEVICE_ID_JOYPAD_A, 0);
   BIND_MAP("P1 Select"			, RETRO_DEVICE_ID_JOYPAD_A, 0);
   BIND_MAP("P1 Use"			, RETRO_DEVICE_ID_JOYPAD_X, 0);
   BIND_MAP("P1 Weak Punch"		, RETRO_DEVICE_ID_JOYPAD_Y, 0);
   BIND_MAP("P1 Medium Punch"	, RETRO_DEVICE_ID_JOYPAD_X, 0);
   BIND_MAP("P1 Strong Punch"	, RETRO_DEVICE_ID_JOYPAD_R, 0);
   BIND_MAP("P1 Weak Kick"		, RETRO_DEVICE_ID_JOYPAD_B, 0);
   BIND_MAP("P1 Medium Kick"	, RETRO_DEVICE_ID_JOYPAD_A, 0);
   BIND_MAP("P1 Strong Kick"	, RETRO_DEVICE_ID_JOYPAD_R2, 0);
   BIND_MAP("P1 Rotate Left"	, RETRO_DEVICE_ID_JOYPAD_B, 0);
   BIND_MAP("P1 Rotate Right"	, RETRO_DEVICE_ID_JOYPAD_A, 0);
   BIND_MAP("P1 Punch"			, RETRO_DEVICE_ID_JOYPAD_Y, 0);
   BIND_MAP("P1 Kick"			, RETRO_DEVICE_ID_JOYPAD_B, 0);
   BIND_MAP("P1 Special"		, RETRO_DEVICE_ID_JOYPAD_A, 0);
   BIND_MAP("P1 Shot"			, RETRO_DEVICE_ID_JOYPAD_B, 0);
   BIND_MAP("P1 Shot (auto)"	, RETRO_DEVICE_ID_JOYPAD_X, 0);

   /* Simpsons - Konami */
   BIND_MAP("P1 Button 1"		, RETRO_DEVICE_ID_JOYPAD_Y, 0);

   /* Simpsons - Konami */
   BIND_MAP("P1 Button 2"		, RETRO_DEVICE_ID_JOYPAD_B, 0);
   BIND_MAP("P1 Button 3"		, RETRO_DEVICE_ID_JOYPAD_A, 0);
   BIND_MAP("P1 Button 4"		, RETRO_DEVICE_ID_JOYPAD_X, 0);

   /* Progear */
   BIND_MAP("P1 Auto"			, RETRO_DEVICE_ID_JOYPAD_A, 0);

   /* Punisher */
   BIND_MAP("P1 Super"			, RETRO_DEVICE_ID_JOYPAD_A, 0);
   BIND_MAP("P1 Answer 1"		, RETRO_DEVICE_ID_JOYPAD_Y, 0);
   BIND_MAP("P1 Answer 2"		, RETRO_DEVICE_ID_JOYPAD_X, 0);
   BIND_MAP("P1 Answer 3"		, RETRO_DEVICE_ID_JOYPAD_B, 0);
   BIND_MAP("P1 Answer 4"		, RETRO_DEVICE_ID_JOYPAD_A, 0);
   BIND_MAP("P1 Shot 1"			, RETRO_DEVICE_ID_JOYPAD_B, 0);

   /* Pang 3 */
   BIND_MAP("P1 Shot 1"			, RETRO_DEVICE_ID_JOYPAD_B, 0);

   /* Pang 3 */
   BIND_MAP("P1 Shot 2"			, RETRO_DEVICE_ID_JOYPAD_A, 0);
   BIND_MAP("P1 Bomb"			, RETRO_DEVICE_ID_JOYPAD_A, 0);
   BIND_MAP("P1 Special"		, RETRO_DEVICE_ID_JOYPAD_A, 0);

   /* for Ghouls 'n Ghosts */
   BIND_MAP("P1 Fire"			, RETRO_DEVICE_ID_JOYPAD_Y, 0);

   /* TMNT */
   BIND_MAP("P1 Fire 1"			, RETRO_DEVICE_ID_JOYPAD_Y, 0);

   /* Space Harrier */
   BIND_MAP("Fire 1"			, RETRO_DEVICE_ID_JOYPAD_Y, 0);

   /* Space Harrier */
   BIND_MAP("Fire 2"			, RETRO_DEVICE_ID_JOYPAD_B, 0);

   /* Space Harrier */
   BIND_MAP("Fire 3"			, RETRO_DEVICE_ID_JOYPAD_A, 0);

   /* TMNT */
   BIND_MAP("P1 Fire 2"			, RETRO_DEVICE_ID_JOYPAD_B, 0);

   /* Strider */
   BIND_MAP("P1 Fire 3"			, RETRO_DEVICE_ID_JOYPAD_A, 0);

   /* Strider */
   BIND_MAP("Coin 1"			, RETRO_DEVICE_ID_JOYPAD_SELECT, 0);

   /* Neo Geo */
   BIND_MAP("P1 Button A"		, RETRO_DEVICE_ID_JOYPAD_Y, 0);
   BIND_MAP("P1 Button B"		, RETRO_DEVICE_ID_JOYPAD_B, 0);
   BIND_MAP("P1 Button C"		, RETRO_DEVICE_ID_JOYPAD_X, 0);
   BIND_MAP("P1 Button D"		, RETRO_DEVICE_ID_JOYPAD_A, 0);
   BIND_MAP("P2 Coin"			, RETRO_DEVICE_ID_JOYPAD_SELECT, 1);
   BIND_MAP("P2 Start"			, RETRO_DEVICE_ID_JOYPAD_START, 1);
   BIND_MAP("P2 Up"				, RETRO_DEVICE_ID_JOYPAD_UP, 1);
   BIND_MAP("P2 Down"			, RETRO_DEVICE_ID_JOYPAD_DOWN, 1);
   BIND_MAP("P2 Left"			, RETRO_DEVICE_ID_JOYPAD_LEFT, 1);
   BIND_MAP("P2 Right"			, RETRO_DEVICE_ID_JOYPAD_RIGHT, 1);
   BIND_MAP("P2 Attack"			, RETRO_DEVICE_ID_JOYPAD_Y, 1);

   // for Forgotten Worlds, etc.
   BIND_MAP("P2 Turn"			, RETRO_DEVICE_ID_JOYPAD_A, 1);
   BIND_MAP("P2 Jump"			, RETRO_DEVICE_ID_JOYPAD_B, 1);
   BIND_MAP("P2 Pin"			, RETRO_DEVICE_ID_JOYPAD_A, 1);
   BIND_MAP("P2 Select"			, RETRO_DEVICE_ID_JOYPAD_A, 1);
   BIND_MAP("P2 Use"			, RETRO_DEVICE_ID_JOYPAD_X, 1);
   BIND_MAP("P2 Weak Punch"		, RETRO_DEVICE_ID_JOYPAD_Y, 1);
   BIND_MAP("P2 Medium Punch"	, RETRO_DEVICE_ID_JOYPAD_X, 1);
   BIND_MAP("P2 Strong Punch"	, RETRO_DEVICE_ID_JOYPAD_R, 1);
   BIND_MAP("P2 Weak Kick"		, RETRO_DEVICE_ID_JOYPAD_B, 1);
   BIND_MAP("P2 Medium Kick"	, RETRO_DEVICE_ID_JOYPAD_A, 1);
   BIND_MAP("P2 Strong Kick"	, RETRO_DEVICE_ID_JOYPAD_R2, 1);
   BIND_MAP("P2 Rotate Left"	, RETRO_DEVICE_ID_JOYPAD_B, 1);
   BIND_MAP("P2 Rotate Right"	, RETRO_DEVICE_ID_JOYPAD_A, 1);
   BIND_MAP("P2 Punch"			, RETRO_DEVICE_ID_JOYPAD_Y, 1);
   BIND_MAP("P2 Kick"			, RETRO_DEVICE_ID_JOYPAD_B, 1);
   BIND_MAP("P2 Special"		, RETRO_DEVICE_ID_JOYPAD_A, 1);
   BIND_MAP("P2 Shot"			, RETRO_DEVICE_ID_JOYPAD_B, 1);

   /* Simpsons - Konami */
   BIND_MAP("P2 Button 1"		, RETRO_DEVICE_ID_JOYPAD_Y, 1);
   BIND_MAP("P2 Button 2"		, RETRO_DEVICE_ID_JOYPAD_B, 1);

   /* Various */
   BIND_MAP("P2 Button 3"		, RETRO_DEVICE_ID_JOYPAD_A, 1);
   BIND_MAP("P2 Button 4"		, RETRO_DEVICE_ID_JOYPAD_X, 1);

   /* Progear */
   BIND_MAP("P2 Auto"			, RETRO_DEVICE_ID_JOYPAD_A, 1);
   BIND_MAP("P2 Shot (auto)"	, RETRO_DEVICE_ID_JOYPAD_X, 1);

   /* Punisher */
   BIND_MAP("P2 Super"			, RETRO_DEVICE_ID_JOYPAD_A, 1);
   BIND_MAP("P2 Answer 1"		, RETRO_DEVICE_ID_JOYPAD_Y, 1);
   BIND_MAP("P2 Answer 2"		, RETRO_DEVICE_ID_JOYPAD_X, 1);
   BIND_MAP("P2 Answer 3"		, RETRO_DEVICE_ID_JOYPAD_B, 1);
   BIND_MAP("P2 Answer 4"		, RETRO_DEVICE_ID_JOYPAD_A, 1);

   /* Pang 3 */
   BIND_MAP("P2 Shot 1"			, RETRO_DEVICE_ID_JOYPAD_B, 1);
   BIND_MAP("P2 Shot 2"			, RETRO_DEVICE_ID_JOYPAD_A, 1);
   BIND_MAP("P2 Bomb"			, RETRO_DEVICE_ID_JOYPAD_A, 1);
   BIND_MAP("P2 Special"		, RETRO_DEVICE_ID_JOYPAD_A, 1);

   /* Ghouls 'n Ghosts */
   BIND_MAP("P2 Fire"			, RETRO_DEVICE_ID_JOYPAD_Y, 1);

   /* TMNT */
   BIND_MAP("P2 Fire 1"			, RETRO_DEVICE_ID_JOYPAD_Y, 1);
   BIND_MAP("P2 Fire 2"			, RETRO_DEVICE_ID_JOYPAD_B, 1);

   /* Strider */
   BIND_MAP("P2 Fire 3"			, RETRO_DEVICE_ID_JOYPAD_A, 1);
   BIND_MAP("Coin 2"			, RETRO_DEVICE_ID_JOYPAD_SELECT, 1);

   /* Neo Geo */
   BIND_MAP("P2 Button A"		, RETRO_DEVICE_ID_JOYPAD_Y, 1);
   BIND_MAP("P2 Button B"		, RETRO_DEVICE_ID_JOYPAD_B, 1);
   BIND_MAP("P2 Button C"		, RETRO_DEVICE_ID_JOYPAD_X, 1);
   BIND_MAP("P2 Button D"		, RETRO_DEVICE_ID_JOYPAD_A, 1);
   BIND_MAP("P3 Coin"			, RETRO_DEVICE_ID_JOYPAD_SELECT, 2);
   BIND_MAP("P3 Start"			, RETRO_DEVICE_ID_JOYPAD_START, 2);
   BIND_MAP("P3 Up"				, RETRO_DEVICE_ID_JOYPAD_UP, 2);
   BIND_MAP("P3 Down"			, RETRO_DEVICE_ID_JOYPAD_DOWN, 2);
   BIND_MAP("P3 Left"			, RETRO_DEVICE_ID_JOYPAD_LEFT, 2);
   BIND_MAP("P3 Right"			, RETRO_DEVICE_ID_JOYPAD_RIGHT, 2);
   BIND_MAP("P3 Attack"			, RETRO_DEVICE_ID_JOYPAD_Y, 2);
   BIND_MAP("P3 Jump"			, RETRO_DEVICE_ID_JOYPAD_B, 2);
   BIND_MAP("P3 Pin"			, RETRO_DEVICE_ID_JOYPAD_A, 2);
   BIND_MAP("P3 Select"			, RETRO_DEVICE_ID_JOYPAD_A, 2);
   BIND_MAP("P3 Use"			, RETRO_DEVICE_ID_JOYPAD_X, 2);

   /* Simpsons - Konami */
   BIND_MAP("P3 Button 1"		, RETRO_DEVICE_ID_JOYPAD_Y, 2);
   BIND_MAP("P3 Button 2"		, RETRO_DEVICE_ID_JOYPAD_B, 2);
   BIND_MAP("P3 Button 3"		, RETRO_DEVICE_ID_JOYPAD_A, 2);
   BIND_MAP("P3 Button 4"		, RETRO_DEVICE_ID_JOYPAD_X, 2);

   /* TMNT */
   BIND_MAP("P3 Fire 1"			, RETRO_DEVICE_ID_JOYPAD_Y, 2);
   BIND_MAP("P3 Fire 2"			, RETRO_DEVICE_ID_JOYPAD_B, 2);

   /* Strider */
   BIND_MAP("P3 Fire 3"			, RETRO_DEVICE_ID_JOYPAD_A, 2);
   BIND_MAP("Coin 3"			, RETRO_DEVICE_ID_JOYPAD_SELECT, 2);
   BIND_MAP("P4 Coin"			, RETRO_DEVICE_ID_JOYPAD_SELECT, 3);
   BIND_MAP("P4 Start"			, RETRO_DEVICE_ID_JOYPAD_START, 3);
   BIND_MAP("P4 Up"				, RETRO_DEVICE_ID_JOYPAD_UP, 3);
   BIND_MAP("P4 Down"			, RETRO_DEVICE_ID_JOYPAD_DOWN, 3);
   BIND_MAP("P4 Left"			, RETRO_DEVICE_ID_JOYPAD_LEFT, 3);
   BIND_MAP("P4 Right"			, RETRO_DEVICE_ID_JOYPAD_RIGHT, 3);
   BIND_MAP("P4 Attack"			, RETRO_DEVICE_ID_JOYPAD_Y, 3);
   BIND_MAP("P4 Jump"			, RETRO_DEVICE_ID_JOYPAD_B, 3);   
   BIND_MAP("P4 Pin"			, RETRO_DEVICE_ID_JOYPAD_A, 3);
   BIND_MAP("P4 Select"			, RETRO_DEVICE_ID_JOYPAD_A, 3);
   BIND_MAP("P4 Use"			, RETRO_DEVICE_ID_JOYPAD_X, 3);

   /* Simpsons */
   BIND_MAP("P4 Button 1"		, RETRO_DEVICE_ID_JOYPAD_Y, 3);
   BIND_MAP("P4 Button 2"		, RETRO_DEVICE_ID_JOYPAD_B, 3);
   BIND_MAP("P4 Button 3"		, RETRO_DEVICE_ID_JOYPAD_A, 3);
   BIND_MAP("P4 Button 4"		, RETRO_DEVICE_ID_JOYPAD_X, 3);

   /* TMNT */
   BIND_MAP("P4 Fire 1"			, RETRO_DEVICE_ID_JOYPAD_Y, 3);
   BIND_MAP("P4 Fire 2"			, RETRO_DEVICE_ID_JOYPAD_B, 3);
   BIND_MAP("P4 Fire 3"			, RETRO_DEVICE_ID_JOYPAD_A, 3);
   BIND_MAP("Coin 4"			, RETRO_DEVICE_ID_JOYPAD_SELECT, 3);
   BIND_MAP("Missile"			, RETRO_DEVICE_ID_JOYPAD_A, 3);

   /* Afterburner */
   BIND_MAP("Vulcan"			, RETRO_DEVICE_ID_JOYPAD_B, 3);
   BIND_MAP("Throttle"			, RETRO_DEVICE_ID_JOYPAD_Y, 3);
   BIND_MAP("Reset"				, RETRO_DEVICE_ID_JOYPAD_RESET, 0);
   BIND_MAP("Service"			, RETRO_DEVICE_ID_JOYPAD_SERVICE, 0);
   BIND_MAP("Diagnostic"		, RETRO_DEVICE_ID_JOYPAD_DIAGNOSTIC, 0);
   BIND_MAP("Test"				, RETRO_DEVICE_ID_JOYPAD_TEST, 0);

   // P1 Macros
   BIND_MAP("P1 3x Punch"		, RETRO_DEVICE_ID_JOYPAD_L, 0);
   BIND_MAP("P1 3x Kick"		, RETRO_DEVICE_ID_JOYPAD_L2, 0);

   // P2 Macros
   BIND_MAP("P2 3x Punch"		, RETRO_DEVICE_ID_JOYPAD_L, 1);
   BIND_MAP("P2 3x Kick"		, RETRO_DEVICE_ID_JOYPAD_L2, 1);

   for(unsigned int i = 0; i < nGameInpCount; i++, pgi++)
   {
      /* TODO: Cyberbots: Full Metal Madness */
      /* TODO: Armored Warriors */
      BurnDrvGetInputInfo(&bii, i);

      bool value_found = false;
      for(int j = 0; j < nBindMapCount; j++)
      {
         if((strcmp(bii.szName,"P1 Select") ==0) && (boardrom && (strcmp(boardrom,"neogeo") == 0)))
         {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
            keybinds[pgi->Input.Switch.nCode][1] = 0;
            value_found = true;
         }
         else if((strcmp(bii.szName,"P1 Shot") ==0) && (parentrom && strcmp(parentrom,"avsp") == 0 || strcmp(drvname,"avsp") == 0))
         {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_A;
            keybinds[pgi->Input.Switch.nCode][1] = 0;
            value_found = true;
         }
         else if((strcmp(bii.szName,"P2 Select") ==0) && (boardrom && (strcmp(boardrom,"neogeo") == 0)))
         {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
            keybinds[pgi->Input.Switch.nCode][1] = 1;
            value_found = true;
         }
         else if((parentrom && strcmp(parentrom,"avsp") == 0 || strcmp(drvname,"avsp") == 0) && (strcmp(bii.szName,"P2 Shot") ==0))
         {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_A;
            keybinds[pgi->Input.Switch.nCode][1] = 1;
            value_found = true;
         }
         else if(strcmp(bii.szName, bind_map[j].bii_name) == 0)
         {
            keybinds[pgi->Input.Switch.nCode][0] = bind_map[j].nCode[0];
            keybinds[pgi->Input.Switch.nCode][1] = bind_map[j].nCode[1];
            value_found = true;
         }
         else
            value_found = false;

         if(value_found)
         {
            fprintf(stderr, "%s - assigned to key: %s, port: %d.\n", bii.szName, print_label(keybinds[pgi->Input.Switch.nCode][0]),keybinds[pgi->Input.Switch.nCode][1]);
            break;
         }
      }

      if(!value_found)
         fprintf(stderr, "WARNING! Button unaccounted for: [%s].\n", bii.szName);
   }

   BindInputMacros();

   return has_analog;
}

//#define DEBUG_INPUT
//

static inline int CinpJoyAxis(int i, int axis)
{
   switch(axis)
   {
      case 0:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
               RETRO_DEVICE_ID_ANALOG_X);
      case 1:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
               RETRO_DEVICE_ID_ANALOG_Y);
      case 2:
         return 0;
      case 3:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
               RETRO_DEVICE_ID_ANALOG_X);
      case 4:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
               RETRO_DEVICE_ID_ANALOG_Y);
      case 5:
         return 0;
      case 6:
         return 0;
      case 7:
         return 0;
   }
   return 0;
}

static inline int CinpMouseAxis(int i, int axis)
{
   return 0;
}

static inline int CinpState(int i)
{
   return keybinds[i][0];
}

void poll_input_macros()
{
	struct GameInp* pgi = GameInp + nGameInpCount;

	// Process Macros

	for (INT32 i = 0; i < nMacroCount; i++, pgi++) 
	{
		
		INT32	id		= keybinds[pgi->Macro.Switch.nCode][0];
		UINT32	port	= keybinds[pgi->Macro.Switch.nCode][1];
		bool	state	= input_cb(port, RETRO_DEVICE_JOYPAD, 0, id);

		// Digital input

		if (pgi->Macro.nMode && state)	// Macro is defined
		{
			for (INT32 j = 0; j < 4; j++) {
				if (pgi->Macro.pVal[j]) {
					*(pgi->Macro.pVal[j]) = pgi->Macro.nVal[j];
				}
			}
		}
	}
}

static void poll_input(void)
{
   poll_cb();

   struct GameInp* pgi = GameInp;
   unsigned controller_binds_count = nGameInpCount;

   for (int i = 0; i < controller_binds_count; i++, pgi++)
   {
      int nAdd = 0;

      if ((pgi->nInput & GIT_GROUP_SLIDER) == 0)                           // not a slider
         continue;

      if (pgi->nInput == GIT_KEYSLIDER)
      {
         // Get states of the two keys
			if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
				nAdd -= 0x100;
			if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
				nAdd += 0x100;
      }

      // nAdd is now -0x100 to +0x100

      // Change to slider speed
      nAdd *= pgi->Input.Slider.nSliderSpeed;
      nAdd /= 0x100;

      if (pgi->Input.Slider.nSliderCenter)
      {                                          // Attact to center
         int v = pgi->Input.Slider.nSliderValue - 0x8000;
         v *= (pgi->Input.Slider.nSliderCenter - 1);
         v /= pgi->Input.Slider.nSliderCenter;
         v += 0x8000;
         pgi->Input.Slider.nSliderValue = v;
      }

      pgi->Input.Slider.nSliderValue += nAdd;
      // Limit slider
      if (pgi->Input.Slider.nSliderValue < 0x0100)
         pgi->Input.Slider.nSliderValue = 0x0100;
      if (pgi->Input.Slider.nSliderValue > 0xFF00)
         pgi->Input.Slider.nSliderValue = 0xFF00;
   }

   pgi = GameInp;

   for (unsigned i = 0; i < controller_binds_count; i++, pgi++)
   {
      switch (pgi->nInput)
      {
         case GIT_CONSTANT: // Constant value
            {
               pgi->Input.nVal = pgi->Input.Constant.nConst;
               *(pgi->Input.pVal) = pgi->Input.nVal;
            }
            break;
         case GIT_SWITCH:
            {
               // Digital input
               INT32 id = keybinds[pgi->Input.Switch.nCode][0];
               unsigned port = keybinds[pgi->Input.Switch.nCode][1];

               bool state = false;

               if (g_reset || id > 15)
               {
                  if(g_reset && id == RETRO_DEVICE_ID_JOYPAD_RESET)
                  {
                     state = true;
                     id = RETRO_DEVICE_ID_JOYPAD_RESET;
                     g_reset = false;
                  }
                  else
                  {
                     bool button_combo_down = 
                        input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2) &&
                        input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2) &&
                        input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L) &&
                        input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
                     bool service_pressed = ((id == RETRO_DEVICE_ID_JOYPAD_SERVICE) &&
                           input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT)
                           && button_combo_down);
                     bool diag_pressed    = ((id == RETRO_DEVICE_ID_JOYPAD_DIAGNOSTIC) &&
                           input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START)
                           && button_combo_down);
                     bool reset_pressed   = ((id == RETRO_DEVICE_ID_JOYPAD_RESET) &&
                           input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT)
                           && button_combo_down);
                     bool dip_a_pressed   = ((id == RETRO_DEVICE_ID_JOYPAD_DIP_A) &&
                           input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT)
                           && button_combo_down);
                     bool dip_b_pressed   = ((id == RETRO_DEVICE_ID_JOYPAD_DIP_B) &&
                           input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP)
                           && button_combo_down);
                     bool test_pressed   = ((id == RETRO_DEVICE_ID_JOYPAD_TEST) &&
                           input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN)
                           && button_combo_down);

                     state = diag_pressed || service_pressed || reset_pressed || dip_a_pressed
                        || dip_b_pressed || test_pressed;
                  }

                  Reinitialise();
               }
               else
                  state = input_cb(port, RETRO_DEVICE_JOYPAD, 0, id);

               //fprintf(stderr, "GIT_SWITCH: %s, port: %d, pressed: %d.\n", print_label(id), port, state);

               if (pgi->nType & BIT_GROUP_ANALOG)
               {
                  // Set analog controls to full
                  if (state)
                     pgi->Input.nVal = 0xFFFF;
                  else
                     pgi->Input.nVal = 0x0001;
#ifdef LSB_FIRST
                  *(pgi->Input.pShortVal) = pgi->Input.nVal;
#else
                  *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               }
               else
               {
                  // Binary controls
                  if (state)
                     pgi->Input.nVal = 1;
                  else
                     pgi->Input.nVal = 0;
                  *(pgi->Input.pVal) = pgi->Input.nVal;
               }
               break;
            }
         case GIT_KEYSLIDER:						// Keyboard slider
            //fprintf(stderr, "GIT_JOYSLIDER\n");
            {
               int nSlider = pgi->Input.Slider.nSliderValue;
               if (pgi->nType == BIT_ANALOG_REL) {
                  nSlider -= 0x8000;
                  nSlider >>= 4;
               }

               pgi->Input.nVal = (unsigned short)nSlider;
#ifdef LSB_FIRST
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
         case GIT_MOUSEAXIS:						// Mouse axis
            {
               pgi->Input.nVal = (UINT16)(CinpMouseAxis(pgi->Input.MouseAxis.nMouse, pgi->Input.MouseAxis.nAxis) * nAnalogSpeed);
#ifdef LSB_FIRST
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
            }
            break;
         case GIT_JOYAXIS_FULL:
            {				// Joystick axis
               INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);

               if (pgi->nType == BIT_ANALOG_REL) {
                  nJoy *= nAnalogSpeed;
                  nJoy >>= 13;

                  // Clip axis to 8 bits
                  if (nJoy < -32768) {
                     nJoy = -32768;
                  }
                  if (nJoy >  32767) {
                     nJoy =  32767;
                  }
               } else {
                  nJoy >>= 1;
                  nJoy += 0x8000;

                  // Clip axis to 16 bits
                  if (nJoy < 0x0001) {
                     nJoy = 0x0001;
                  }
                  if (nJoy > 0xFFFF) {
                     nJoy = 0xFFFF;
                  }
               }

               pgi->Input.nVal = (UINT16)nJoy;
#ifdef LSB_FIRST
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
         case GIT_JOYAXIS_NEG:
            {				// Joystick axis Lo
               INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);
               if (nJoy < 32767)
               {
                  nJoy = -nJoy;

                  if (nJoy < 0x0000)
                     nJoy = 0x0000;
                  if (nJoy > 0xFFFF)
                     nJoy = 0xFFFF;

                  pgi->Input.nVal = (UINT16)nJoy;
               }
               else
                  pgi->Input.nVal = 0;

#ifdef LSB_FIRST
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
         case GIT_JOYAXIS_POS:
            {				// Joystick axis Hi
               INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);
               if (nJoy > 32767)
               {

                  if (nJoy < 0x0000)
                     nJoy = 0x0000;
                  if (nJoy > 0xFFFF)
                     nJoy = 0xFFFF;

                  pgi->Input.nVal = (UINT16)nJoy;
               }
               else
                  pgi->Input.nVal = 0;

#ifdef LSB_FIRST
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
      }
   }

   poll_input_macros();

}

static unsigned int BurnDrvGetIndexByName(const char* name)
{
   unsigned int ret = ~0U;
   for (unsigned int i = 0; i < nBurnDrvCount; i++) {
      nBurnDrvActive = i;
      if (strcmp(BurnDrvGetText(DRV_NAME), name) == 0) {
         ret = i;
         break;
      }
   }
   return ret;
}

#ifdef ANDROID
#include <wchar.h>

size_t mbstowcs(wchar_t *pwcs, const char *s, size_t n)
{
   if (pwcs == NULL)
      return strlen(s);
   return mbsrtowcs(pwcs, &s, n, NULL);
}

size_t wcstombs(char *s, const wchar_t *pwcs, size_t n)
{
   return wcsrtombs(s, &pwcs, n, NULL);
}

#endif


// FBARL

#define FBARL_INI_PATH "/dev_hdd0/game/FBAL00123/USRDIR/FBA_RL.ini"

int getBoolOption(FILE* fp, char* option, bool *bOption)
{
	if(!fp) return 0;

	rewind(fp);

	char* pszLine = NULL;
	pszLine = (char*)malloc(2048+1);
	memset(pszLine, 0, 2048+1);
	
	while (!feof(fp)) 
	{
		fgets(pszLine, 2048, fp);

		char* pch = NULL;
		pch = strstr(pszLine, option);

		if(pch) {
			char* pszOption =  NULL;
			pszOption = (char*)malloc(2048+1);
			memset(pszOption, 0, 2048+1);
				
			strncpy(pszOption, pch+(strlen(option)), 2048);

			pch = strrchr(pszOption, '"');
			pszOption[(int)(pch-pszOption)] = 0;

			if(pszLine) { free(pszLine); *&pszLine = NULL; }

			if(!pszOption){
				// debug...
				//FILE* fpd = fopen("/dev_hdd0/game/FBAL00123/USRDIR/inidebug.txt", "w");
				//if(fpd) 
				//{
				//	fprintf(fpd, "error: getBoolOption() option--> %s \n", option);
				//	fclose(fpd);
				//	fpd = NULL;
				//}
				return 0;
			}

			if( strcmp(pszOption, "yes")==0 || 
				strcmp(pszOption, "YES")==0 ||
				strcmp(pszOption, "Yes")==0 ||
				strcmp(pszOption, "true")==0 ||
				strcmp(pszOption, "True")==0 ||
				strcmp(pszOption, "TRUE")==0 ||
				strcmp(pszOption, "1")==0 )
			{
				if(pszOption) { free(pszOption); *&pszOption = NULL; }
				*bOption = true;
				return 1;
			}
			if( strcmp(pszOption, "no")==0 || 
				strcmp(pszOption, "NO")==0 ||
				strcmp(pszOption, "No")==0 ||
				strcmp(pszOption, "false")==0 ||
				strcmp(pszOption, "False")==0 ||
				strcmp(pszOption, "FALSE")==0 ||
				strcmp(pszOption, "0")==0 )
			{
				if(pszOption) { free(pszOption); *&pszOption = NULL; }
				*bOption = false;
				return 1;
			}


		}
	}
	return 0;
}

int iniRead()
{
	FILE* fp = NULL;
	fp = fopen(FBARL_INI_PATH, "r");
	if(fp) 
	{
		// -----------------------------------------------------------------------------------------
		// Enable / Disable Neo-Geo UNI-BIOS
		// -----------------------------------------------------------------------------------------
		if(!getBoolOption(fp, "use_ng_unibios:\"", &g_opt_bUseUNIBIOS)) {fclose(fp);return 0;}
		// -----------------------------------------------------------------------------------------

		fclose(fp);
		return 1;
	} else {
		return 0;
	}
	return 0;
}
