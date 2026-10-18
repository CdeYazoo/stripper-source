/** vim: set ts=4 sw=4 et tw=99:
 *
 * === Stripper for Metamod:Source ===
 * Copyright (C) 2005-2009 David "BAILOPAN" Anderson
 * No warranties of any kind.
 * Based on the original concept of Stripper2 by botman
 *
 * License: see LICENSE.TXT
 * ===================================
 */
#include <new>
#include <stdio.h>
#include <string.h>
#if !defined(_WIN32)
#include <strings.h>
#include <time.h>
#endif
#include "support.h"
#include "parser.h"
#if defined WIN32
#include <windows.h>
#include <direct.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

Stripper g_Stripper;
stripper_game_t stripper_game;

#if defined _WIN32
char *strchrnul(const char *str, int c)
{
	while(*str)
	{
		if (c == *str) break;
		str++;
	}
	return const_cast<char*>(str);
}
#define EXPORT extern "C" __declspec(dllexport)
#elif defined __GNUC__
#if __GNUC__ == 4
#define EXPORT extern "C" __attribute__ ((visibility("default")))
#else
#define EXPORT extern "C"
#endif
#endif

/* Monotonic milliseconds, used only for the timing log in parse_map. */
static double
now_ms()
{
#if defined WIN32 || defined _WIN32
	LARGE_INTEGER freq, count;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&count);
	return (double)count.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

static const char*
parse_map(const char* map, const char* entities)
{
	char path[256];

	double t_start = now_ms();
	g_Stripper.SetEntityList(entities);
	double t_parsed = now_ms();
	int files_applied = 0;
	double slowest_ms = 0.0;
	char slowest_name[128];
	slowest_name[0] = '\0';

	stripper_game.path_format(path,
		sizeof(path),
		"%s/%s/global_filters.cfg",
		stripper_game.game_path,
		stripper_game.stripper_cfg_path);
	double t_file = now_ms();
	if (!g_Stripper.ApplyFileFilter(path))
	{
		stripper_game.log_message("Could not find global filter file: %s", path);
	}
	else
	{
		double took = now_ms() - t_file;
		files_applied++;
		if (took >= slowest_ms)
		{
			slowest_ms = took;
			snprintf(slowest_name, sizeof(slowest_name), "global_filters.cfg");
		}
	}

	for (const char* pPart = map; *pPart++ != '\0';)
	{
		pPart = strchrnul(pPart, '_');
		stripper_game.path_format(path, sizeof(path), "%s/%s/maps/%.*s.cfg",
			stripper_game.game_path, stripper_game.stripper_cfg_path, pPart - map, map);
		/* silently skips files that don't exist */
		t_file = now_ms();
		if (g_Stripper.ApplyFileFilter(path))
		{
			double took = now_ms() - t_file;
			files_applied++;
			if (took >= slowest_ms)
			{
				slowest_ms = took;
				snprintf(slowest_name, sizeof(slowest_name), "%.*s.cfg", (int)(pPart - map), map);
			}
		}
	}

	double t_filtered = now_ms();
	const char *result = g_Stripper.ToString();
	double t_done = now_ms();

	stripper_game.log_message(
		"Timing for \"%s\": lump %d KB, parse %.1f ms, %d config file(s) %.1f ms (slowest: %s %.1f ms), output %.1f ms, total %.1f ms",
		map,
		(int)(strlen(entities) / 1024),
		t_parsed - t_start,
		files_applied,
		t_filtered - t_parsed,
		slowest_name[0] ? slowest_name : "none",
		slowest_ms,
		t_done - t_filtered,
		t_done - t_start);

	return result;
}

static const char*
ent_string()
{
	return g_Stripper.ToString();
}

static void
command_dump()
{
	char path[255];

	stripper_game.path_format(path,
		sizeof(path),
		"%s/%s/dumps",
		stripper_game.game_path,
		stripper_game.stripper_path);

#ifdef WIN32
	DWORD attr = GetFileAttributes(path);
	if ((attr = INVALID_FILE_ATTRIBUTES) || (!(attr & FILE_ATTRIBUTE_DIRECTORY)))
	{
		unlink(path);
		mkdir(path);
}
#else
	struct stat s;

	if (stat(path, &s) != 0)
	{
		mkdir(path, 0775);
	}
	else
	{
		if (!(S_ISDIR(s.st_mode)))
		{
			unlink(path);
			mkdir(path, 0775);
		}
	}
#endif

	int num = 0;
	char file[255];

	char szMapName[128];
	stripper_game.get_map_name(szMapName, sizeof(szMapName));

	do
	{
		stripper_game.path_format(file,
			sizeof(file),
			"%s/%s.%04d.cfg",
			path,
			szMapName,
			num);
		FILE* fp = fopen(file, "rt");
		if (!fp)
			break;
		fclose(fp);
	} while (++num);

	FILE* fp = fopen(file, "wt");

	if (!fp)
	{
		stripper_game.log_message("Failed to create dump file %s", file);
		return;
	}

	fprintf(fp, "%s", g_Stripper.ToString());
	fclose(fp);

	stripper_game.log_message("Logged map %s to file %s", szMapName, file);
}

static void
plugin_unload()
{
}

static stripper_core_t stripper_core =
{
	parse_map,
	ent_string,
	command_dump,
	plugin_unload
};

EXPORT void
LoadStripper(const stripper_game_t* game, stripper_core_t* core)
{
	memcpy(&stripper_game, game, sizeof(stripper_game_t));
	memcpy(core, &stripper_core, sizeof(stripper_core_t));
}

/* Overload a few things to prevent libstdc++ linking */
#if defined __linux__ || defined __APPLE__
extern "C" void __cxa_pure_virtual(void)
{
}

void* operator new(size_t size)
{
	return malloc(size);
}

void* operator new[](size_t size)
{
	return malloc(size);
}

void operator delete(void* ptr)
{
	free(ptr);
}

void operator delete[](void* ptr)
{
	free(ptr);
}
#endif
