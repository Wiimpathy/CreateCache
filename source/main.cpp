#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <algorithm>
#include <new>
#include <gccore.h>
#include <sys/param.h>  // MAXPATHLEN devkit r29+
#include <wiiuse/wpad.h>
#include <dirent.h>
#include <fat.h>
#include <zlib.h>

#include "texture.hpp"
#include "pngu.h"

#define VERSION "Create Cache 0.2"

extern "C" {
extern void __exception_setreload(int t);
}

static void *xfb = NULL;
GXRModeObj *rmode;

char logfile[25];

struct SWFCHeader
{
	u8 newFmt : 1;	// Was 0 in beta
	u8 full : 1;
	u8 cmpr : 1;
	u8 zipped : 1;
	u8 backCover : 1;
	u16 width;
	u16 height;
	u8 maxLOD;
	u16 backWidth;
	u16 backHeight;
	u8 backMaxLOD;
public:
	u32 getWidth(void) const { return width * 4; }
	u32 getHeight(void) const { return height * 4; }
	u32 getBackWidth(void) const { return backWidth * 4; }
	u32 getBackHeight(void) const { return backHeight * 4; }
	SWFCHeader(void)
	{
		memset(this, 0, sizeof *this);
	}
	SWFCHeader(const TexData &tex, bool f, bool z, const TexData &backTex = TexData())
	{
		newFmt = 1;
		full = f ? 1 : 0;
		cmpr = tex.format == GX_TF_CMPR ? 1 : 0;
		zipped = z ? 1 : 0;
		width = tex.width / 4;
		height = tex.height / 4;
		maxLOD = tex.maxLOD;
		backCover = !!backTex.data ? 1 : 0;
		backWidth = backTex.width / 4;
		backHeight = backTex.height / 4;
		backMaxLOD = backTex.maxLOD;
	}
};

void ClearScreen()
{
	printf("\x1b[2J");
	printf("\x1b[2;0H");
}

void Quit()
{
	printf("\nExiting now...");
	sleep(5);
	exit(0);
}

bool FileExist(const char *path)
{
	FILE * f;
	f = fopen(path, "rb");
	if(f)
	{
		fclose(f);
		return true;
	}
	return false;
}

bool DirExist(const char *path)
{
	DIR *dir;
	dir = opendir(path);
	if(dir)
	{
		closedir(dir);
		return true;
	}
	return false;
}

void CreateCache(const char *name, int indent, bool compress, bool skip, bool usb)
{
	int i = 0, count = 0;
	u8 textureFmt = compress ? GX_TF_CMPR : GX_TF_RGB565;

	DIR *dir = NULL;
	struct dirent *entry = NULL;

	if (!(dir = opendir(name)))
	{
		printf("\nError opening %s", name);
		sleep(5);
		return;
	}

    while ((entry = readdir(dir)) != NULL) {
		char path[MAXPATHLEN];

		WPAD_ScanPads();
		PAD_ScanPads();
		VIDEO_WaitVSync();

		u32 btnsWii = WPAD_ButtonsDown(0);
		u32 btnsGC = PAD_ButtonsDown(0);

		if(btnsGC & PAD_BUTTON_START || btnsGC & PAD_TRIGGER_Z || btnsWii & WPAD_BUTTON_HOME)
		{
			VIDEO_WaitVSync();
			VIDEO_WaitVSync();
			printf("\nAction cancelled!\n");
			ClearScreen();
			Quit();
		}

        if (entry->d_type == DT_DIR)  // Folder : open recursively
		{
			// Skip parent and current directory
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
				continue;
			snprintf(path, sizeof(path), "%s/%s", name, entry->d_name);

			char *cachepath = (char*)malloc(MAXPATHLEN+1);
			sprintf(cachepath, "%s:/wiiflow/cache%s" ,  usb ? "usb" : "sd", path+1);
			//printf("cachepath: %s\n", cachepath);
			
			// Create cache subdirectory if it doesn't exist
			DIR *dircache = NULL;
			dircache = opendir(cachepath);
			if (dircache) closedir(dircache);
			else mkdir(cachepath,S_IRWXU);

			free(cachepath);

			CreateCache(path, indent + 2, compress, skip, usb);
        }
		else // Files : create cache files.
		{
			snprintf(path, sizeof(path), "%s/%s", name, entry->d_name);

			// Checks for /wiiflow/boxcovers root folder (Wii covers). Count > 1 means subdirectory ie. plugins'covers 
			count = 0;
			for (i = 0; path[i] != '\0'; i++) 
			{
				if(path[i] == '/')
					count++;
			}
			
			// Skips cache creation if we're in /wiiflow/boxcovers only
			if(skip && count <= 1)
			{
				continue;
				//printf("Root, Wii cover : skip it!\n");
			}
			else
			{
				// Get absolute boxcover path
				char *fullpath = (char*)malloc(MAXPATHLEN+1);
				sprintf(fullpath, "%s:/wiiflow/boxcovers%s", usb ? "usb" : "sd", path+1);

				// Strip extension
				char *ext = NULL;
				ext = strrchr(path, '.');
				size_t ext_length = strlen(ext);
				size_t path_length = strlen(path);
				path[path_length - ext_length] = 0;

				// Cache file absolute path
				char wfcpath[MAXPATHLEN];
				if(usb)
					sprintf(wfcpath, "usb:/wiiflow/cache%s.wfc", path+1);
				else
					sprintf(wfcpath, "sd:/wiiflow/cache%s.wfc", path+1);

				//printf("wfcpath: %s\n", wfcpath);

				// Don't overwrite existing .wfc files.
				if(FileExist(wfcpath))
				{
					printf("%s already exists!\n", wfcpath);
					free(fullpath);
					continue;
				}
				else
				{
					// Open image and convert texture to desired format
					TexData tex;
					u32 bufSize = 0;
					bool m_compressCache = false; // FIXME : always false, not implemented.
					bool TexError = false;
					uLongf zBufferSize = 0;
					u8 *zBuffer = NULL;
					FILE *file = NULL;

					tex.thread = true;

					printf("\nConverting : %s\n", fullpath);
					TexError = TexHandle.fromImageFile(tex, fullpath, textureFmt, 32);

					bufSize = fixGX_GetTexBufferSize(tex.width, tex.height, tex.format, tex.maxLOD > 0 ? GX_TRUE : GX_FALSE, tex.maxLOD);
					zBufferSize = bufSize;
					zBuffer = tex.data;

					// No error detected, save the .wfc file.
					if(!TexError)
					{
						file = fopen (wfcpath, "wb");
						if(file != NULL && !TexError)
						{
							SWFCHeader header(tex, 1, m_compressCache);
							fwrite(&header, 1, sizeof header, file);
							fwrite(zBuffer, 1, zBufferSize, file);
							fclose(file);
							printf("%s saved.\n", wfcpath);
						}
					}
						// Free the texture and path
						TexHandle.Cleanup(tex);
						free(fullpath);
				}
			}
		}
	}
	closedir(dir);
}

void format_elapsed_time(char *time_str, double elapsed) {
	int h, m, s, ms;

	h = m = s = ms = 0;
	ms = elapsed * 1000; // promote the fractional part to milliseconds
	h = ms / 3600000;
	ms -= (h * 3600000);
	m = ms / 60000;
	ms -= (m * 60000);
	s = ms / 1000;
	ms -= (s * 1000);
	sprintf(time_str, "%02ih:%02im:%02is", h, m, s);
}

//---------------------------------------------------------------------------------
int main(int argc, char **argv) {
//---------------------------------------------------------------------------------

	// Initialise the video system
	VIDEO_Init();

	// Exit after crash
	__exception_setreload(8);
	
	// This function initialises the attached controllers
	WPAD_Init();
	
	// Obtain the preferred video mode from the system
	// This will correspond to the settings in the Wii menu
	rmode = VIDEO_GetPreferredMode(NULL);

	// Allocate memory for the display in the uncached region
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	
	// Initialise the console, required for printf
	console_init(xfb,20,20,rmode->fbWidth,rmode->xfbHeight,rmode->fbWidth*VI_DISPLAY_PIX_SZ);
	
	// Set up the video registers with the chosen mode
	VIDEO_Configure(rmode);
	
	// Tell the video hardware where our display memory is
	VIDEO_SetNextFramebuffer(xfb);
	
	// Make the display visible
	VIDEO_SetBlack(FALSE);

	// Flush the video register changes to the hardware
	VIDEO_Flush();

	// Wait for Video setup to complete
	VIDEO_WaitVSync();
	if(rmode->viTVMode&VI_NON_INTERLACE) VIDEO_WaitVSync();

	// Initialise the fat devices
	fatInitDefault();
	
	while(1) {
		int done = 0;
		int i = 0;
		bool compressTex = true;
		bool isUSB = false;
		bool skipWii = true;

		while(!done)
		{
			printf("\x1b[2J");
			printf("\x1b[3;1H%s\n", VERSION);
			printf("\x1b[5;1HThis homebrew creates .wfc files for WiiFlow. (plugins only!)\n");
			printf("\x1b[6;1HScanning the boxcovers and converting them take a long time.\n");

			printf("\x1b[11;1H");
			printf("\n%s Compress Textures   : << %s >>\n", i ? " " : ">", compressTex ? "YES" : "NO");
			printf("\n%s Covers directory    : << %s >>\n", (i == 1) ? ">" : " ", isUSB ? "USB" : "SD");
			printf("\n%s Skip Wii/GC covers  : << %s >>\n", (i == 2) ? ">" : " ", skipWii ? "YES" : "NO");

			printf("\x1b[24;0HUp/Down    : Select option.\n");
			printf("\x1b[25;0HLeft/Right : Modify option.\n");
			printf("\x1b[26;0HGC Start / Wiimote + : Begin cache creation.\n");
			printf("GC Z / Wiimote Home  : Exit.\n");

			WPAD_ScanPads();
			PAD_ScanPads();
			VIDEO_WaitVSync();

			u32 btnsWii = WPAD_ButtonsDown(0);
			u32 btnsGC = PAD_ButtonsDown(0);

			if(btnsGC & PAD_BUTTON_UP || btnsWii & WPAD_BUTTON_UP)
			{
				i--;
			}
			else if(btnsGC & PAD_BUTTON_DOWN || btnsWii & WPAD_BUTTON_DOWN)
			{
				i++;
			}
			else if(btnsGC & PAD_BUTTON_LEFT || btnsWii & WPAD_BUTTON_LEFT || btnsGC & PAD_BUTTON_RIGHT || btnsWii & WPAD_BUTTON_RIGHT)
			{
				if(i==0)
				{
					compressTex = !compressTex;
				}
				else if(i==1)
				{
					isUSB = !isUSB;
				}
				else if(i==2)
				{
					skipWii = !skipWii;
				}
			}
			else if(btnsGC & PAD_BUTTON_START || btnsWii & WPAD_BUTTON_PLUS)
			{
				done = 1;
				break;
			}
			else if(btnsGC & PAD_TRIGGER_Z || btnsWii & WPAD_BUTTON_HOME)
			{
				VIDEO_WaitVSync();
				VIDEO_WaitVSync();
				ClearScreen();
				Quit();
			}

			if(i>2 || i<0)
				i=0;
		}

		// Go to covers folder
		if(isUSB)
		{
			if(DirExist("usb:/wiiflow/boxcovers/"))
			{
				chdir("usb:/wiiflow/boxcovers/");
			}
			else
			{
				ClearScreen();
				printf("\nusb:/wiiflow/boxcovers/ not found!");
				Quit();
			}
		}
		else
		{
			if(DirExist("sd:/wiiflow/boxcovers/"))
			{
				chdir("sd:/wiiflow/boxcovers/");
			}
			else
			{
				ClearScreen();
				printf("\nsd:/wiiflow/boxcovers/ not found!");
				Quit();
			}
		}

		// Clear screen
		ClearScreen();

		printf("The cache creation will begin shortly. Press Home/Start to cancel at any time.\n");
		sleep(6);

		// Create log file
		sprintf(logfile, "%s:/log_cachecreate.txt",  isUSB ? "usb" : "sd");
		FILE *log;
		log = fopen(logfile, "wb");
		if (log == NULL)
		{
			ClearScreen();
			printf("Error! can't open log file %s\n", logfile);
			sleep(3);
		}
		fclose(log);

		// Timer to estimate duration
		time_t t0, t1;
		t0 = time(NULL);

		// Start browsing boxcovers and creating cache files
		CreateCache(".", 0, compressTex, skipWii, isUSB);

		// Get and Display elapsed time
		t1 = time(NULL);
		double elapsed = difftime(t1, t0);
		char Timetxt[MAXPATHLEN];

		format_elapsed_time(Timetxt, elapsed);
		ClearScreen();
		printf("Cache creation finished.\n");
		printf("It took about %s\n", Timetxt);

		// Append elapsed time to log file
		log = fopen(logfile, "a");
		if (log == NULL)
		{
			ClearScreen();
			printf("Error! can't open log file %s\n", logfile);
			sleep(3);
		}
		else
		{
			fprintf(log, "Cache generated in  %s\n", Timetxt);
		}
		fclose(log);

		Quit();

		// Wait for the next frame
		VIDEO_WaitVSync();
	}
	return 0;
}
 
