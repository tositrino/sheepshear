/*
 *  video.cpp - Video/graphics emulation
 *
 *  SheepShaver (C) 1997-2002 Marc Hellwig and Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * TODO
 * - check for supported modes ???
 * - window mode "hardware" cursor hotspot
 */

#include <stdio.h>
#include <string.h>

#include "sysdeps.h"
#include "video.h"
#include "video_defs.h"
#include "main.h"
#include "adb.h"
#include "macos_util.h"
#include "user_strings.h"
#include "version.h"
#include "thunks.h"

#define DEBUG 0
#include "debug.h"


// Global variables
bool video_activated = false;		// Flag: video display activated, mouse and keyboard data valid
uint32 screen_base = 0;				// Frame buffer base address
int cur_mode;						// Number of current video mode (index in VModes array)
int display_type = DIS_INVALID;		// Current display type
rgb_color mac_pal[256];
uint8 remap_mac_be[256];
uint8 MacCursor[68] = {16, 1};	// Mac cursor image


bool keyfile_valid;		// Flag: Keyfile is valid, enable full-screen modes


/*
 *  Video mode information (constructed by VideoInit())
 */

struct VideoInfo VModes[64];


/*
 *  Driver local variables
 */

VidLocals *private_data = NULL;	// Pointer to driver local variables (there is only one display, so this is ok)

static long save_conf_id = APPLE_W_640x480;
static long save_conf_mode = APPLE_8_BIT;


// Function pointers of imported functions
typedef int16 (*iocic_ptr)(void *, int16);
static uint32 iocic_tvect = 0;
static inline int16 IOCommandIsComplete(void *arg1, int16 arg2)
{
	return (int16)CallMacOS2(iocic_ptr, iocic_tvect, arg1, arg2);
}
typedef int16 (*vslnewis_ptr)(void *, uint32, uint32 *);
static uint32 vslnewis_tvect = 0;
static inline int16 VSLNewInterruptService(void *arg1, uint32 arg2, uint32 *arg3)
{
	return (int16)CallMacOS3(vslnewis_ptr, vslnewis_tvect, arg1, arg2, arg3);
}
typedef int16 (*vsldisposeis_ptr)(uint32);
static uint32 vsldisposeis_tvect = 0;
static inline int16 VSLDisposeInterruptService(uint32 arg1)
{
	return (int16)CallMacOS1(vsldisposeis_ptr, vsldisposeis_tvect, arg1);
}
typedef int16 (*vsldois_ptr)(uint32);
static uint32 vsldois_tvect = 0;
int16 VSLDoInterruptService(uint32 arg1)
{
	return (int16)CallMacOS1(vsldois_ptr, vsldois_tvect, arg1);
}
typedef void (*nqdmisc_ptr)(uint32, void *);
static uint32 nqdmisc_tvect = 0;
void NQDMisc(uint32 arg1, void *arg2)
{
	CallMacOS2(nqdmisc_ptr, nqdmisc_tvect, arg1, arg2);
}


// Prototypes
static int16 set_gamma(VidLocals *csSave, uint32 gamma);


/*
 *  Tell whether window/screen is activated or not (for mouse/keyboard polling)
 */
 
bool VideoActivated(void)
{
	return video_activated;	
}


/*
 *  Create RGB snapshot of current screen
 */

bool VideoSnapshot(int xsize, int ysize, uint8 *p)
{
	if (display_type == DIS_WINDOW) {
		uint8 *screen = (uint8 *)private_data->saveBaseAddr;
		uint32 row_bytes = VModes[cur_mode].viRowBytes;	
		uint32 y2size = VModes[cur_mode].viYsize;
		uint32 x2size = VModes[cur_mode].viXsize;
		for (int j=0;j<ysize;j++) {
			for (int i=0;i<xsize;i++) {
				*p++ = mac_pal[screen[uint32(float(j)*float(y2size)/float(ysize))*row_bytes+uint32(float(i)*float(x2size)/float(xsize))]].red;
				*p++ = mac_pal[screen[uint32(float(j)*float(y2size)/float(ysize))*row_bytes+uint32(float(i)*float(x2size)/float(xsize))]].green;
				*p++ = mac_pal[screen[uint32(float(j)*float(y2size)/float(ysize))*row_bytes+uint32(float(i)*float(x2size)/float(xsize))]].blue;
			}
		}
		return true;
	}
	return false;
}


/*
 *  Video driver open routine
 */

static int16 VideoOpen(uint32 pb, VidLocals *csSave)
{
	D(bug("Video Open\n"));

	// Set up VidLocals
	csSave->saveBaseAddr = screen_base;
	csSave->saveData = VModes[cur_mode].viAppleID;// First mode ...
	csSave->saveMode = VModes[cur_mode].viAppleMode;
	csSave->savePage = 0;
	csSave->saveVidParms = 0;			// Add the right table
	csSave->gammaTable = NULL;			// No gamma table yet
	csSave->maxGammaTableSize = 0;
	csSave->luminanceMapping = false;
	csSave->cursorX = 0;
	csSave->cursorY = 0;
	csSave->cursorVisible = 0;
	csSave->cursorSet = 0;

	// Activate default gamma table
	set_gamma(csSave, 0);

	// Install and activate interrupt service
	SheepVar32 theServiceID = 0;
	VSLNewInterruptService(csSave->regEntryID, FOURCC('v','b','l',' '), (uint32 *)theServiceID.addr());
	csSave->vslServiceID = theServiceID.value();
	D(bug(" Interrupt ServiceID %08lx\n", csSave->vslServiceID));
	csSave->interruptsEnabled = true;

	return noErr;
}


/*
 *  Video driver control routine
 */

static int16 set_gamma(VidLocals *csSave, uint32 gamma)
{
	GammaTbl *clientGamma = (GammaTbl *)gamma;
	GammaTbl *gammaTable = csSave->gammaTable;

	if (clientGamma == NULL) {

		// No gamma table supplied, build linear ramp
		uint32 linearRampSize = sizeof(GammaTbl) + 256 - 2;	
		uint8 *correctionData;

		// Allocate new gamma table if existing gamma table is smaller than required.
		if (linearRampSize > csSave->maxGammaTableSize) {
			delete[] csSave->gammaTable;
			csSave->gammaTable = (GammaTbl *)new uint8[linearRampSize];
			csSave->maxGammaTableSize = linearRampSize;
			gammaTable = csSave->gammaTable;
		}
		
		gammaTable->gVersion = 0;			// A version 0 style of the GammaTbl structure
		gammaTable->gType = 0;				// Frame buffer hardware invariant
		gammaTable->gFormulaSize = 0;		// No formula data, just correction data
		gammaTable->gChanCnt = 1;			// Apply same correction to Red, Green, & Blue
		gammaTable->gDataCnt = 256;			// gDataCnt == 2^^gDataWidth
		gammaTable->gDataWidth = 8;			// 8 bits of significant data per entry

		// Find the starting address of the correction data.  This can be computed by starting at
		// the address of gFormula[0] and adding the gFormulaSize.
		correctionData = (uint8 *)((uint32)&gammaTable->gFormulaData[0] + gammaTable->gFormulaSize);

		// Build the linear ramp
		for (int i=0; i<gammaTable->gDataCnt; i++)
			*correctionData++ = i;		

	} else {

		// User supplied a gamma table, so make sure it is a valid one
		if (clientGamma->gVersion != 0)
			return paramErr;
		if (clientGamma->gType != 0)
			return paramErr;
		if ((clientGamma->gChanCnt != 1) && (clientGamma->gChanCnt != 3))
			return paramErr;
		if (clientGamma->gDataWidth > 8)
			return paramErr;
		if (clientGamma->gDataCnt != (1 << clientGamma->gDataWidth))
			return paramErr;

		uint32 tableSize = sizeof(GammaTbl)						// fixed size header
				+ clientGamma->gFormulaSize						// add formula size
				+ clientGamma->gChanCnt * clientGamma->gDataCnt	// assume 1 byte/entry
				- 2; 											// correct gFormulaData[0] counted twice

		// Allocate new gamma table if existing gamma table is smaller than required.
		if (tableSize > csSave->maxGammaTableSize) {
			delete[] csSave->gammaTable;
			csSave->gammaTable = (GammaTbl *)new uint8[tableSize];
			csSave->maxGammaTableSize = tableSize;
			gammaTable = csSave->gammaTable;
		}

		// Copy gamma table header		
		*gammaTable = *clientGamma;
		
		// Copy the formula data (if any)
		uint8 *newData = (uint8 *)&gammaTable->gFormulaData[0];		// Point to newGamma's formula data
		uint8 *clientData = (uint8 *)&clientGamma->gFormulaData[0];	// Point to clientGamma's formula data
		for (int i=0; i<gammaTable->gFormulaSize; i++)
			*newData++ = *clientData++;

		// Copy the correction data. Convientiently, after copying the formula data, the 'newData'
		// pointer and the 'clientData' pointer are pointing to the their respective starting points
		// of their correction data.
		for (int i=0; i<gammaTable->gChanCnt; i++)
			for (int j=0; j<gammaTable->gDataCnt; j++)		
				*newData++ = *clientData++;
	}
	return noErr;
}

static int16 VideoControl(uint32 pb, VidLocals *csSave)
{
	int16 code = ReadMacInt16(pb + csCode);
	D(bug("VideoControl %d: ", code));
	uint32 param = ReadMacInt32(pb + csParam);
	switch (code) {

		case cscReset:									// VidReset
			D(bug("VidReset\n"));
			return controlErr;

		case cscKillIO:									// VidKillIO
			D(bug("VidKillIO\n"));
			return controlErr;

		case cscSetMode:								// SetVidMode
			D(bug("SetVidMode\n"));
			D(bug("mode:%04x page:%04x \n", ReadMacInt16(param + csMode),
				ReadMacInt16(param + csPage)));
			WriteMacInt32(param + csData, csSave->saveData);
			return video_mode_change(csSave, param);

		case cscSetEntries: {							// SetEntries
			D(bug("SetEntries\n"));					
			if (VModes[cur_mode].viAppleMode > APPLE_8_BIT) return controlErr;
			ColorSpec *s_pal = (ColorSpec *)Mac2HostAddr(ReadMacInt32(param + csTable));
			int16 start = ReadMacInt16(param + csStart);
			int16 count = ReadMacInt16(param + csCount);
			if (s_pal == NULL || count > 256) return controlErr;

			// Preparations for gamma correction
			bool do_gamma = false;
			uint8 *red_gamma = NULL;
			uint8 *green_gamma = NULL;
			uint8 *blue_gamma = NULL;
			int gamma_data_width = 0;
			if (display_type == DIS_SCREEN && csSave->gammaTable != NULL) {	// Windows are gamma-corrected by BeOS
				do_gamma = true;
				GammaTbl *gamma = csSave->gammaTable;
				gamma_data_width = gamma->gDataWidth;
				red_gamma = (uint8 *)&gamma->gFormulaData + gamma->gFormulaSize;
				if (gamma->gChanCnt == 1) {
					green_gamma = blue_gamma = red_gamma;
				} else {
					green_gamma = red_gamma + gamma->gDataCnt;
					blue_gamma = red_gamma + 2 * gamma->gDataCnt;
				}
			}

			// Set palette
			rgb_color *d_pal;
			if (start == -1) {			// Indexed
				for (int i=0; i<=count; i++) {
					d_pal = &(mac_pal[(*s_pal).value]);
					uint8 red = (*s_pal).red >> 8;
					uint8 green = (*s_pal).green >> 8;
					uint8 blue = (*s_pal).blue >> 8;
					if (csSave->luminanceMapping)
						red = green = blue = (red * 0x4ccc + green * 0x970a + blue * 0x1c29) >> 16;
					if (do_gamma) {
						red = red_gamma[red >> (8 - gamma_data_width)];
						green = green_gamma[green >> (8 - gamma_data_width)];
						blue = blue_gamma[blue >> (8 - gamma_data_width)];
					}
					(*d_pal).red = red;
					(*d_pal).green = green;
					(*d_pal).blue = blue;
					s_pal++;
				}
			} else {								// Sequential
				d_pal = &(mac_pal[start]);
				for (int i=0; i<=count; i++) {
					uint8 red = (*s_pal).red >> 8;
					uint8 green = (*s_pal).green >> 8;
					uint8 blue = (*s_pal).blue >> 8;
					if (csSave->luminanceMapping)
						red = green = blue = (red * 0x4ccc + green * 0x970a + blue * 0x1c29) >> 16;
					if (do_gamma) {
						red = red_gamma[red >> (8 - gamma_data_width)];
						green = green_gamma[green >> (8 - gamma_data_width)];
						blue = blue_gamma[blue >> (8 - gamma_data_width)];
					}
					(*d_pal).red = red;
					(*d_pal).green = green;
					(*d_pal).blue = blue;
					d_pal++; s_pal++;
				}
			}
			video_set_palette();
			return noErr;
		}

		case cscSetGamma:							// SetGamma
			D(bug("SetGamma\n"));
			return set_gamma(csSave, ReadMacInt32(param));

		case cscGrayPage: {							// GrayPage
			D(bug("GrayPage\n"));
			uint32 *screen = (uint32 *)csSave->saveBaseAddr;
			uint32 pattern;
			uint32 row_bytes = VModes[cur_mode].viRowBytes;	
			switch (VModes[cur_mode].viAppleMode) {
				case APPLE_8_BIT:
					pattern=0xff00ff00;
					for (int i=0;i<VModes[cur_mode].viYsize;i++) {
						for (int j=0;j<(VModes[cur_mode].viXsize>>2);j++)
							screen[j] = pattern;
						pattern = ~pattern;
						screen = (uint32 *)((uint32)screen + row_bytes);
					}
					break;
				case APPLE_16_BIT:
					pattern=0xffff0000;
					for (int i=0;i<VModes[cur_mode].viYsize;i++) {
						for (int j=0;j<(VModes[cur_mode].viXsize>>1);j++)
							screen[j]=pattern;
						pattern = ~pattern;
						screen = (uint32 *)((uint32)screen + row_bytes);
					}
					break;
				case APPLE_32_BIT:
					pattern=0xffffffff;
					for (int i=0;i<VModes[cur_mode].viYsize;i++) {
						for (int j=0;j<VModes[cur_mode].viXsize;j++) {
							screen[j]=pattern;
							pattern = ~pattern;
						}
						screen = (uint32 *)((uint32)screen + row_bytes);
					}
					break;
			}
			return noErr;
		}

		case cscSetGray:							// SetGray
			D(bug("SetGray %02x\n", ReadMacInt8(param)));
			csSave->luminanceMapping = ReadMacInt8(param);
			return noErr;

		case cscSetInterrupt:						// SetInterrupt
			D(bug("SetInterrupt\n"));
			csSave->interruptsEnabled = !ReadMacInt8(param);
			return noErr;

		case cscDirectSetEntries:					// DirectSetEntries
			D(bug("DirectSetEntries\n"));
			return controlErr;

		case cscSetDefaultMode:						// SetDefaultMode
			D(bug("SetDefaultMode\n"));
			return controlErr;

		case cscSwitchMode:
			D(bug("cscSwitchMode (Display Manager support) \nMode:%02x ID:%04x Page:%d\n",
			  ReadMacInt16(param + csMode), ReadMacInt32(param + csData), ReadMacInt16(param + csPage)));
			return video_mode_change(csSave, param);

		case cscSavePreferredConfiguration:
			D(bug("SavePreferredConfiguration\n"));
			save_conf_id = ReadMacInt32(param + csData);
			save_conf_mode = ReadMacInt16(param + csMode);
			return noErr;

		case cscSetHardwareCursor: {
//			D(bug("SetHardwareCursor\n"));
			csSave->cursorSet = false;
			bool changed = false;

			// Get cursor data even on a screen, to set the right cursor image when switching back to a window
			// Image
			uint32 cursor = ReadMacInt32(param);	// Pointer to CursorImage
			uint32 pmhandle = ReadMacInt32(cursor + ciCursorPixMap);
			if (pmhandle == 0 || ReadMacInt32(pmhandle) == 0)
				return controlErr;
			uint32 pixmap = ReadMacInt32(pmhandle);
			if (memcmp(MacCursor + 4, Mac2HostAddr(ReadMacInt32(pixmap)), 32)) {
				memcpy(MacCursor + 4, Mac2HostAddr(ReadMacInt32(pixmap)), 32);
				changed = true;
			}

			// Mask
			uint32 bmhandle = ReadMacInt32(cursor + ciCursorBitMask);
			if (bmhandle == 0 || ReadMacInt32(bmhandle) == 0)
				return controlErr;
			uint32 bitmap = ReadMacInt32(bmhandle);
			if (memcmp(MacCursor + 4 + 32, Mac2HostAddr(ReadMacInt32(bitmap)), 32)) {
				memcpy(MacCursor + 4 + 32, Mac2HostAddr(ReadMacInt32(bitmap)), 32);
				changed = true;
			}

			// Hotspot (!! this doesn't work)
			MacCursor[2] = ReadMacInt8(0x885);
			MacCursor[3] = ReadMacInt8(0x887);

			// Set new cursor image
			if (display_type == DIS_SCREEN)
				return controlErr;
			if (changed)
				video_set_cursor();

			csSave->cursorSet = true;
			return noErr;
		}

		case cscDrawHardwareCursor:
//			D(bug("DrawHardwareCursor\n"));
			csSave->cursorX = ReadMacInt32(param + csCursorX);
			csSave->cursorY = ReadMacInt32(param + csCursorY);
			csSave->cursorVisible = ReadMacInt32(param + csCursorVisible);
			return noErr;

		case 43: {	// Driver Gestalt
			uint32 sel = ReadMacInt32(pb + csParam);
			D(bug(" driver gestalt %c%c%c%c\n", sel >> 24, sel >> 16,  sel >> 8, sel));
			switch (sel) {
				case FOURCC('v','e','r','s'):
					WriteMacInt32(pb + csParam + 4, 0x01008000);
					break;
				case FOURCC('i','n','t','f'):
					WriteMacInt32(pb + csParam + 4, FOURCC('c','a','r','d'));
					break;
				case FOURCC('s','y','n','c'):
					WriteMacInt32(pb + csParam + 4, 0x01000000);
					break;
				default:
					return statusErr;
			};
			return noErr;
		}

		default:
			D(bug(" unknown control code %d\n", code));
			return controlErr;
	}
}


/*
 *  Video driver status routine
 */

// Search for given AppleID in mode table
static bool has_mode(uint32 id)
{
	VideoInfo *p = VModes;
	while (p->viType != DIS_INVALID) {
		if (p->viAppleID == id)
			return true;
		p++;
	}
	return false;
}

// Find maximum depth for given AppleID
static uint32 max_depth(uint32 id)
{
	uint32 max = APPLE_1_BIT;
	VideoInfo *p = VModes;
	while (p->viType != DIS_INVALID) {
		if (p->viAppleID == id && p->viAppleMode > max)
			max = p->viAppleMode;
		p++;
	}
	return max;
}

static int16 VideoStatus(uint32 pb, VidLocals *csSave)
{
	int16 code = ReadMacInt16(pb + csCode);
	D(bug("VideoStatus %d: ", code));
	uint32 param = ReadMacInt32(pb + csParam);
	switch (code) {

		case cscGetMode:							// GetMode
			D(bug("GetMode\n"));
			WriteMacInt32(param + csBaseAddr, csSave->saveBaseAddr);
			WriteMacInt16(param + csMode, csSave->saveMode);
			WriteMacInt16(param + csPage, csSave->savePage);
			D(bug("return: mode:%04x page:%04x ", ReadMacInt16(param + csMode),
				ReadMacInt16(param + csPage)));
			D(bug("base adress %08lx\n", ReadMacInt32(param + csBaseAddr)));
			return noErr;

		case cscGetEntries: {						// GetEntries
			D(bug("GetEntries\n"));	
			ColorSpec *d_pal = (ColorSpec *)Mac2HostAddr(ReadMacInt32(param + csTable));
			int16 start = ReadMacInt16(param + csStart);
			int16 count = ReadMacInt16(param + csCount);
			rgb_color *s_pal;
			if ((VModes[cur_mode].viAppleMode == APPLE_32_BIT)||
				(VModes[cur_mode].viAppleMode == APPLE_16_BIT)) {
				D(bug("ERROR: GetEntries in direct mode \n"));
				return statusErr;
			}
			if (start >= 0) {	// indexed get
				s_pal = &(mac_pal[start]);
				for (uint16 i=0;i<count;i++) {
					(*d_pal).red=(uint16)((*s_pal).red)*0x101;
					(*d_pal).green=(uint16)((*s_pal).green)*0x101;
					(*d_pal).blue=(uint16)((*s_pal).blue)*0x101;
					d_pal++; s_pal++;
				}
			} else {								// selected set
				for (uint16 i=0;i<count;i++) {
					s_pal = &(mac_pal[(*d_pal).value]);
					(*d_pal).red=(uint16)((*s_pal).red)*0x101;
					(*d_pal).green=(uint16)((*s_pal).green)*0x101;
					(*d_pal).blue=(uint16)((*s_pal).blue)*0x101;
					d_pal++;
				}
			};
			return noErr;
		}

		case cscGetPageCnt:						// GetPage
			D(bug("GetPage\n"));
			WriteMacInt16(param + csPage, 1);
			return noErr;

		case cscGetPageBase:						// GetPageBase
			D(bug("GetPageBase\n"));
			WriteMacInt32(param + csBaseAddr, csSave->saveBaseAddr);
			return noErr;

		case cscGetGray:							// GetGray
			D(bug("GetGray\n"));
			WriteMacInt8(param, csSave->luminanceMapping ? 1 : 0);
			return noErr;

		case cscGetInterrupt:						// GetInterrupt
			D(bug("GetInterrupt\n"));
			WriteMacInt8(param, csSave->interruptsEnabled ? 0 : 1);
			return noErr;

		case cscGetGamma:							// GetGamma
			D(bug("GetGamma\n"));
			WriteMacInt32(param, (uint32)csSave->gammaTable);
			return statusErr;

		case cscGetDefaultMode:						// GetDefaultMode
			D(bug("GetDefaultMode\n"));
			return statusErr;

		case cscGetCurMode:							// GetCurMode
			D(bug("GetCurMode\n"));
			WriteMacInt16(param + csMode, csSave->saveMode);
			WriteMacInt32(param + csData, csSave->saveData);
			WriteMacInt16(param + csPage, csSave->savePage);
			WriteMacInt32(param + csBaseAddr, csSave->saveBaseAddr);
			
			D(bug("return: mode:%04x ID:%08lx page:%04x ", ReadMacInt16(param + csMode),
				ReadMacInt32(param + csData), ReadMacInt16(param + csPage)));
			D(bug("base adress %08lx\n", ReadMacInt32(param + csBaseAddr)));
			return noErr;

		case cscGetConnection:						// GetConnection
			D(bug("GetConnection\n"));
			WriteMacInt16(param + csDisplayType, kMultiModeCRT3Connect);
			WriteMacInt8(param + csConnectTaggedType, 6);
			WriteMacInt8(param + csConnectTaggedData, 0x23);
			WriteMacInt32(param + csConnectFlags, (1<<kAllModesValid)|(1<<kAllModesSafe));
			WriteMacInt32(param + csDisplayComponent, 0);
			return noErr;

		case cscGetModeBaseAddress:
			D(bug("GetModeBaseAddress (obsolete !) \n"));
			return statusErr;

		case cscGetPreferredConfiguration:
			D(bug("GetPreferredConfiguration \n"));
			WriteMacInt16(param + csMode, save_conf_mode);
			WriteMacInt32(param + csData, save_conf_id);
			return noErr;

		case cscGetNextResolution: {
			D(bug("GetNextResolution \n"));
			int work_id = ReadMacInt32(param + csPreviousDisplayModeID);
			switch (work_id) {
				case kDisplayModeIDCurrent:
					work_id = csSave->saveData;
					break;
				case kDisplayModeIDFindFirstResolution:
					work_id = APPLE_ID_MIN;
					while (!has_mode(work_id))
						work_id ++;
					break;
				default:
					if (!has_mode(work_id))
						return paramErr;
					work_id++;
					while (!has_mode(work_id)) {
						work_id++;
						if (work_id > APPLE_ID_MAX) {
							WriteMacInt32(param + csRIDisplayModeID, kDisplayModeIDNoMoreResolutions);
							return noErr;
						}
					}
					break;
			}
			WriteMacInt32(param + csRIDisplayModeID, work_id);
			WriteMacInt16(param + csMaxDepthMode, max_depth(work_id));
			switch (work_id) {
				case APPLE_640x480:
					WriteMacInt32(param + csHorizontalPixels, 640);
					WriteMacInt32(param + csVerticalLines, 480);
					WriteMacInt32(param + csRefreshRate, 75<<16);
					break;
				case APPLE_W_640x480:
					WriteMacInt32(param + csHorizontalPixels, 640);
					WriteMacInt32(param + csVerticalLines, 480);
					WriteMacInt32(param + csRefreshRate, 60<<16);
					break;
				case APPLE_800x600:
					WriteMacInt32(param + csHorizontalPixels, 800);
					WriteMacInt32(param + csVerticalLines, 600);
					WriteMacInt32(param + csRefreshRate, 75<<16);
					break;
				case APPLE_W_800x600:
					WriteMacInt32(param + csHorizontalPixels, 800);
					WriteMacInt32(param + csVerticalLines, 600);
					WriteMacInt32(param + csRefreshRate, 60<<16);
					break;
				case APPLE_1024x768:
					WriteMacInt32(param + csHorizontalPixels, 1024);
					WriteMacInt32(param + csVerticalLines, 768);
					WriteMacInt32(param + csRefreshRate, 75<<16);
					break;
				case APPLE_1152x900:
					WriteMacInt32(param + csHorizontalPixels, 1152);
					WriteMacInt32(param + csVerticalLines, 900);
					WriteMacInt32(param + csRefreshRate, 75<<16);
					break;
				case APPLE_1280x1024:
					WriteMacInt32(param + csHorizontalPixels, 1280);
					WriteMacInt32(param + csVerticalLines, 1024);
					WriteMacInt32(param + csRefreshRate, 75<<16);
					break;
				case APPLE_1600x1200:
					WriteMacInt32(param + csHorizontalPixels, 1600);
					WriteMacInt32(param + csVerticalLines, 1200);
					WriteMacInt32(param + csRefreshRate, 75<<16);
					break;
			}
			return noErr;
		}

		case cscGetVideoParameters:					// GetVideoParameters
			D(bug("GetVideoParameters ID:%08lx Depth:%04x\n",
				ReadMacInt32(param + csDisplayModeID),
				ReadMacInt16(param + csDepthMode)));

			// find right video mode						
			for (int i=0; VModes[i].viType!=DIS_INVALID; i++) {
				if ((ReadMacInt16(param + csDepthMode) == VModes[i].viAppleMode) &&
					(ReadMacInt32(param + csDisplayModeID) == VModes[i].viAppleID)) {
					uint32 vpb = ReadMacInt32(param + csVPBlockPtr);
					WriteMacInt32(vpb + vpBaseOffset, 0);
					WriteMacInt16(vpb + vpRowBytes, VModes[i].viRowBytes);
					WriteMacInt16(vpb + vpBounds, 0);
					WriteMacInt16(vpb + vpBounds + 2, 0);
					WriteMacInt16(vpb + vpBounds + 4, VModes[i].viYsize);
					WriteMacInt16(vpb + vpBounds + 6, VModes[i].viXsize);
					WriteMacInt16(vpb + vpVersion, 0);		// Pixel Map version number
					WriteMacInt16(vpb + vpPackType, 0);
					WriteMacInt32(vpb + vpPackSize, 0);
					WriteMacInt32(vpb + vpHRes, 0x00480000);	// horiz res of the device (ppi)
					WriteMacInt32(vpb + vpVRes, 0x00480000);	// vert res of the device (ppi)
					switch (VModes[i].viAppleMode) {
						case APPLE_1_BIT:
							WriteMacInt16(vpb + vpPixelType, 0); 
							WriteMacInt16(vpb + vpPixelSize, 1);
							WriteMacInt16(vpb + vpCmpCount, 1);
							WriteMacInt16(vpb + vpCmpSize, 1);
							WriteMacInt32(param + csDeviceType, 0); // CLUT
							break;
						case APPLE_2_BIT:
							WriteMacInt16(vpb + vpPixelType, 0); 
							WriteMacInt16(vpb + vpPixelSize, 2);
							WriteMacInt16(vpb + vpCmpCount, 1);
							WriteMacInt16(vpb + vpCmpSize, 2);
							WriteMacInt32(param + csDeviceType, 0); // CLUT
							break;
						case APPLE_4_BIT:
							WriteMacInt16(vpb + vpPixelType, 0); 
							WriteMacInt16(vpb + vpPixelSize, 4);
							WriteMacInt16(vpb + vpCmpCount, 1);
							WriteMacInt16(vpb + vpCmpSize, 4);
							WriteMacInt32(param + csDeviceType, 0); // CLUT
							break;
						case APPLE_8_BIT:
							WriteMacInt16(vpb + vpPixelType, 0); 
							WriteMacInt16(vpb + vpPixelSize, 8);
							WriteMacInt16(vpb + vpCmpCount, 1);
							WriteMacInt16(vpb + vpCmpSize, 8);
							WriteMacInt32(param + csDeviceType, 0); // CLUT
							break;
						case APPLE_16_BIT:
							WriteMacInt16(vpb + vpPixelType, 0x10); 
							WriteMacInt16(vpb + vpPixelSize, 16);
							WriteMacInt16(vpb + vpCmpCount, 3);
							WriteMacInt16(vpb + vpCmpSize, 5);
							WriteMacInt32(param + csDeviceType, 2); // DIRECT
							break;
						case APPLE_32_BIT:
							WriteMacInt16(vpb + vpPixelType, 0x10); 
							WriteMacInt16(vpb + vpPixelSize, 32);
							WriteMacInt16(vpb + vpCmpCount, 3);
							WriteMacInt16(vpb + vpCmpSize, 8);
							WriteMacInt32(param + csDeviceType, 2); // DIRECT
							break;
					}
					WriteMacInt32(param + csPageCount, 1);
					return noErr;
				}
			}
			return paramErr;

		case cscGetModeTiming:
			D(bug("GetModeTiming mode %08lx\n", ReadMacInt32(param + csTimingMode)));
			WriteMacInt32(param + csTimingFormat, kDeclROMtables);
			WriteMacInt32(param + csTimingFlags, (1<<kModeValid)|(1<<kModeSafe)|(1<<kShowModeNow));		// Mode valid, safe, default and shown in Monitors panel
			for (int i=0; VModes[i].viType!=DIS_INVALID; i++) {
				if (ReadMacInt32(param + csTimingMode) == VModes[i].viAppleID) {
					uint32 timing = timingUnknown;
					uint32 flags = (1<<kModeValid) | (1<<kShowModeNow);
					switch (VModes[i].viAppleID) {
						case APPLE_640x480:
							timing = timingVESA_640x480_75hz;
							flags |= (1<<kModeSafe);
							break;
						case APPLE_W_640x480:
							timing = timingVESA_640x480_60hz;
							flags |= (1<<kModeSafe);
							break;
						case APPLE_800x600:
							timing = timingVESA_800x600_75hz;
							flags |= (1<<kModeSafe);
							break;
						case APPLE_W_800x600:
							timing = timingVESA_800x600_60hz;
							flags |= (1<<kModeSafe);
							break;
						case APPLE_1024x768:
							timing = timingVESA_1024x768_75hz;
							break;
						case APPLE_1152x900:
							timing = timingApple_1152x870_75hz;
							break;
						case APPLE_1280x1024:
							timing = timingVESA_1280x960_75hz;
							break;
						case APPLE_1600x1200:
							timing = timingVESA_1600x1200_75hz;
							break;
						default:
							timing = timingUnknown;
							break;
					}
					WriteMacInt32(param + csTimingData, timing);
					WriteMacInt32(param + csTimingFlags, flags);
					return noErr;
				}
			}
			return paramErr;

		case cscSupportsHardwareCursor:
			D(bug("SupportsHardwareCursor\n"));
			WriteMacInt32(param, 1);
			return noErr;

		case cscGetHardwareCursorDrawState:
			D(bug("GetHardwareCursorDrawState\n"));
			WriteMacInt32(param + csCursorX, csSave->cursorX);
			WriteMacInt32(param + csCursorY, csSave->cursorY);
			WriteMacInt32(param + csCursorVisible, csSave->cursorVisible);
			WriteMacInt32(param + csCursorSet, csSave->cursorSet);
			return noErr;

		default:
			D(bug(" unknown status code %d\n", code));
			return statusErr;
	}
}


/*
 *  Video driver close routine
 */

static int16 VideoClose(uint32 pb, VidLocals *csSave)
{
	D(bug("VideoClose\n"));

	// Delete interrupt service
	csSave->interruptsEnabled = false;
	VSLDisposeInterruptService(csSave->vslServiceID);

	return noErr;
}


/*
 *  Native (PCI) driver entry
 */

int16 VideoDoDriverIO(void *spaceID, void *commandID, void *commandContents, uint32 commandCode, uint32 commandKind)
{
//	D(bug("VideoDoDriverIO space %p, command %p, contents %p, code %d, kind %d\n", spaceID, commandID, commandContents, commandCode, commandKind));
	int16 err = noErr;

	switch (commandCode) {
		case kInitializeCommand:
		case kReplaceCommand:
			if (private_data != NULL)	// Might be left over from a reboot
				delete private_data->gammaTable;
			delete private_data;

			iocic_tvect = (uint32)FindLibSymbol("\021DriverServicesLib", "\023IOCommandIsComplete");
			D(bug("IOCommandIsComplete TVECT at %08lx\n", iocic_tvect));
			if (iocic_tvect == 0) {
				printf("FATAL: VideoDoDriverIO(): Can't find IOCommandIsComplete()\n");
				err = -1;
				break;
			}
			vslnewis_tvect = (uint32)FindLibSymbol("\020VideoServicesLib", "\026VSLNewInterruptService");
			D(bug("VSLNewInterruptService TVECT at %08lx\n", vslnewis_tvect));
			if (vslnewis_tvect == 0) {
				printf("FATAL: VideoDoDriverIO(): Can't find VSLNewInterruptService()\n");
				err = -1;
				break;
			}
			vsldisposeis_tvect = (uint32)FindLibSymbol("\020VideoServicesLib", "\032VSLDisposeInterruptService");
			D(bug("VSLDisposeInterruptService TVECT at %08lx\n", vsldisposeis_tvect));
			if (vsldisposeis_tvect == 0) {
				printf("FATAL: VideoDoDriverIO(): Can't find VSLDisposeInterruptService()\n");
				err = -1;
				break;
			}
			vsldois_tvect = (uint32)FindLibSymbol("\020VideoServicesLib", "\025VSLDoInterruptService");
			D(bug("VSLDoInterruptService TVECT at %08lx\n", vsldois_tvect));
			if (vsldois_tvect == 0) {
				printf("FATAL: VideoDoDriverIO(): Can't find VSLDoInterruptService()\n");
				err = -1;
				break;
			}
			nqdmisc_tvect = (uint32)FindLibSymbol("\014InterfaceLib", "\007NQDMisc");
			D(bug("NQDMisc TVECT at %08lx\n", nqdmisc_tvect));
			if (nqdmisc_tvect == 0) {
				printf("FATAL: VideoDoDriverIO(): Can't find NQDMisc()\n");
				err = -1;
				break;
			}

			private_data = new VidLocals;
			private_data->gammaTable = NULL;
			memcpy(private_data->regEntryID, (uint8 *)commandContents + 2, 16);	// DriverInitInfo.deviceEntry
			private_data->interruptsEnabled = false;	// Disable interrupts
			break;

		case kFinalizeCommand:
		case kSupersededCommand:
			if (private_data != NULL)
				delete private_data->gammaTable;
			delete private_data;
			private_data = NULL;
			break;

		case kOpenCommand:
			err = VideoOpen((uint32)commandContents, private_data);
			break;

		case kCloseCommand:
			err = VideoClose((uint32)commandContents, private_data);
			break;

		case kControlCommand:
			err = VideoControl((uint32)commandContents, private_data);
			break;

		case kStatusCommand:
			err = VideoStatus((uint32)commandContents, private_data);
			break;

		case kReadCommand:
		case kWriteCommand:
			break;

		case kKillIOCommand:
			err = abortErr;
			break;

		default:
			err = paramErr;
			break;
	}

	if (commandKind == kImmediateIOCommandKind)
		return err;
	else
		return IOCommandIsComplete(commandID, err);
}
