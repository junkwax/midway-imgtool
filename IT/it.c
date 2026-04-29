/****************************************************************
*
* Title:				Image tool (IT)
* Software:			Shawn Liptak
* Initiated:		6/2/92
*
* Modified:			12/10/93 - Started Watcom C / DOS4GW version
*
* COPYRIGHT (C) 1992,1993,1994 WILLIAMS ELECTRONICS GAMES, INC.
*
*.Last mod - 3/4/94 20:01
****************************************************************/



#include	<string.h>
#include	<stdlib.h>
#include	<stdio.h>
#include	<time.h>
#include	<sys/types.h>
#include	<sys/stat.h>
#include	<signal.h>
#include	"compat.h"
#include	<SDL.h>
#include	"shim_vid.h"
#include	"shim_file.h"
#include	"imgui_overlay.h"


long	conv_ftoi(float *f);
long	conv_radtoi(float *f);

char	imgenv_s[80];
char	tgaenv_s[80];
char	mdlenv_s[80];
char	usr1env_s[80];
char	usr2env_s[80];
char	usr3env_s[80];

extern long	*mempool_p;
extern long	mempoolsz;


sig_atomic_t	sigcnt;

void	cbreak(int sig_no)
{
	++sigcnt;
}


/* asm entry point — itos.asm defines _osmain (MSVC _cdecl convention).
   The original Watcom trailing-underscore has been renamed in itos.asm.
   Now replaced by C++ main loop below. */
/* void osmain(char *c_p); */

int	main(int argc,char *argv[])
{
//	int	i;
//	FILE	*f_p;
//	char	buf[80];

	char	*c_p;

	/* Populate exe_dir so shim_file.c can remap "c:\bin\" paths */
	{
		char full[MAX_PATH];
#ifdef _WIN32
		if (GetModuleFileNameA(NULL, full, sizeof(full))) {
			char *slash = strrchr(full, '\\');
			if (slash) { *slash = '\0'; strncpy(exe_dir, full, MAX_PATH-1); }
		}
#else
		ssize_t len = readlink("/proc/self/exe", full, sizeof(full) - 1);
		if (len < 0 && argc > 0) {  /* fallback: use argv[0] */
			strncpy(full, argv[0], sizeof(full) - 1);
			len = (ssize_t)strlen(full);
		}
		if (len > 0) {
			full[len] = '\0';
			char *slash = strrchr(full, '/');
			if (slash) { *slash = '\0'; strncpy(exe_dir, full, MAX_PATH-1); }
		}
#endif
	}


	if (c_p = getenv("IMGDIR")) strcpy(imgenv_s, c_p);
	if (c_p = getenv("TGADIR")) strcpy(tgaenv_s, c_p);
	if (c_p = getenv("MODELS")) strcpy(mdlenv_s, c_p);
	if (c_p = getenv("ITUSR1")) strcpy(usr1env_s, c_p);
	if (c_p = getenv("ITUSR2")) strcpy(usr2env_s, c_p);
	if (c_p = getenv("ITUSR3")) strcpy(usr3env_s, c_p);


	/* ---- Init SDL2 ---- */
	SDL_SetMainReady();
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		MessageBoxA(NULL, SDL_GetError(), "SDL_Init Error", MB_OK | MB_ICONERROR);
		return 1;
	}

	SDL_Window *window = SDL_CreateWindow("Imgtool",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		1024, 768, SDL_WINDOW_RESIZABLE);
	if (!window) return 1;

	SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
		SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
	if (!renderer) return 1;

	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

	/* Create a 640x480 canvas texture (matches original VGA resolution) */
	SDL_Texture *canvas_tex = SDL_CreateTexture(renderer,
		SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 480);

	/* ---- Init ImGui overlay ---- */
	imgui_overlay_init(window, renderer, canvas_tex);

	/* ---- Main loop ----
	 *
	 * Idle-friendly: we use SDL_WaitEventTimeout to block when nothing's
	 * happening, instead of spinning at 100% CPU on one core. The timeout
	 * keeps ImGui's hover-tooltip delays, popup fade animations and any
	 * mouse-button-held repaints (paint dragging, anipoint dragging) feel-
	 * ing smooth — a 16ms timeout is one-frame-at-60Hz, so worst case the
	 * UI catches up within a single frame.
	 *
	 * We force-render every iteration regardless of whether the wait timed
	 * out or returned an event; ImGui needs at least one frame to settle
	 * after every input change, and reactive elements like tooltips fade
	 * across multiple frames. */
	int running = 1;
	while (running) {
		SDL_Event e;
		/* Block up to 16ms waiting for the first event. */
		if (SDL_WaitEventTimeout(&e, 16)) {
			do {
				imgui_overlay_process_event(&e);
				if (e.type == SDL_QUIT)
					imgui_overlay_request_quit();
			} while (SDL_PollEvent(&e));
		}

		SDL_SetRenderDrawColor(renderer, 0x06, 0x06, 0x06, 0xFF);
		SDL_RenderClear(renderer);

		imgui_overlay_newframe();
		imgui_overlay_render();
		imgui_overlay_present();

		SDL_RenderPresent(renderer);

		if (imgui_overlay_should_quit())
			running = 0;
	}

	imgui_overlay_shutdown();
	SDL_DestroyTexture(canvas_tex);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();

	return 0;
}

long	conv_ftoi(float *f)
{
	return( (long) *f );
}


long	conv_radtoi(float *f)
{
	return( (long) (*f*512/3.141592654) );
}



// EOF
