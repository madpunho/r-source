/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996  Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1997-1999  Robert Gentleman, Ross Ihaka and the
 *                            R Development Core Team
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
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

         /* See ../unix/system.txt for a description of functions */

#ifdef HAVE_CONFIG_H
#include <Rconfig.h>
#endif

#include "Defn.h"
#include "Fileio.h"
#include "Graphics.h"		/* KillAllDevices() [nothing else?] */
#include "graphapp/ga.h"
#include "console.h"
#include "rui.h"
#include "getline/getline.h"
/*#include "devga.h"*/
/*#include <windows.h>*/
#include "run.h"
#include "Startup.h"

int SaveAction = SA_DEFAULT;
int RestoreAction = SA_NORESTORE;
int LoadSiteFile = True;
int LoadInitFile = True;
int DebugInitFile = False;

UImode  CharacterMode;
int   ConsoleAcceptCmd;
void closeAllHlpFiles();
void UnLoad_Unzip_Dll();


/* used to avoid some flashing during cleaning up */
int   AllDevicesKilled = 0;
int   setupui(void);
void  delui(void);

int   UserBreak = 0;

/* callbacks */
static void (*R_CallBackHook) ();
static void R_DoNothing() {}
static void (*my_R_Busy)(int);

/*
 *   Called at I/O, during eval etc to process GUI events.
 */

void ProcessEvents(void)
{
    while (peekevent()) doevent();
    if (UserBreak) {
	UserBreak = 0;
	/* error("user break\n"); */
	raise(SIGINT);
    }
    R_CallBackHook();
}


/*
 *  1) FATAL MESSAGES AT STARTUP
 */

void R_Suicide(char *s)
{
    char  pp[1024];

    sprintf(pp, "Fatal error: %s\n", s);
    R_ShowMessage(pp);
    R_CleanUp(SA_SUICIDE);
}

/*
 *  2. CONSOLE I/O
 */

/*
 * I realized that we are supporting 4 different type of input.
 * 1) from the gui console;
 * 2) from a character mode console (interactive);
 * 3) from a pipe under --ess, i.e, interactive.
 * 4) from a file or from a pipe (not interactive)
 *
 * Hence, it is better to have a different function for every
 * situation.
 * Same, it is true for output (but in this case, 3==4)
 *
 * BTW, 3 and 4 are different on input  since fgets,ReadFile...
 * "blocks" =>
 * (e.g.) you cannot give focus to the graphics device if
 * you are wating for input. For this reason, under 3,
 * fgets is runned in a different thread (Windows is wonderful,
 * I never used 'threads', hence, after made this running
 * I was very, very happy "Wuah, fgets in a thread!!!")
 */


/*1: from GUI console */
static int R_is_running = 0;

void Rconsolesetwidth(int cols)
{
    if(R_is_running && setWidthOnResize)
	R_SetOptionWidth(cols);
}

static int GuiReadConsole(char *prompt, char *buf, int len, int addtohistory)
{
    char *p;
    char *NormalPrompt =
	(char *) CHAR(STRING(GetOption(install("prompt"), R_NilValue))[0]);

    if(!R_is_running) {
	R_is_running = 1;
	Rconsolesetwidth(consolecols(RConsole));
    }
    ConsoleAcceptCmd = !strcmp(prompt, NormalPrompt);
    consolereads(RConsole, prompt, buf, len, addtohistory);
    for (p = buf; *p; p++)
	if (*p == EOF)
	    *p = '\001';
    ConsoleAcceptCmd = 0;
    return 1;
}

static void GuiWriteConsole(char *buf,int len)
{
    char *p;

    for (p = buf; *p; p++)
	if (*p == '\001')
	    *p = EOF;
    consolewrites(RConsole, buf);
}

/*2: from character console with getline */
static char LastLine[512];

int CharReadConsole(char *prompt, char *buf, int len, int addtohistory)
{
    static char *gl = NULL;
    int   i;

    if (!gl) {
	strcat(LastLine, prompt);
	gl = getline(LastLine);
	LastLine[0] = '\0';
	if (addtohistory)
	    gl_histadd(gl);
    }
    for (i = 0; *gl && (*gl != '\n') && (i < len - 2); gl++, i++)
	buf[i] = *gl;
    buf[i] = '\n';
    buf[i + 1] = '\0';
    if (!*gl || (*gl == '\n'))
	gl = NULL;
    return 1;
}

void CharWriteConsole(char *buf, int len)
{
    char *p = strrchr(buf, '\n');

    if (p)
	strcpy(LastLine, p + 1);
    else
	strcat(LastLine, buf);
    printf("%s", buf);
}

/*3: from a pipe under --ess*/

/*
 * Variables used to communicate between thread and main process
 */
static int lineavailable, lengthofbuffer;
static char *inputbuffer;

static DWORD CALLBACK
threadedfgets(LPVOID unused)
{
    inputbuffer = fgets(inputbuffer, lengthofbuffer, stdin);
    lineavailable = 1;
    return 0;
}

static int
PipeReadConsole(char *prompt, char *buf, int len, int addhistory)
{
    HANDLE rH;
    int   id;

    if (!R_Slave) {
	fputs(prompt, stdout);
	fflush(stdout);
    }
    lineavailable = 0;
    lengthofbuffer = len;
    inputbuffer = buf;
    rH = CreateThread(NULL, 0, threadedfgets, NULL, 0, &id);
    if (!rH) {
	/* failure! Use standard fgets. */
	inputbuffer = fgets(buf, len, stdin);
	lineavailable = 1;
    }
    while (!lineavailable)
	doevent();
    if (rH)
	CloseHandle(rH);
    if (!inputbuffer)
	return 0;
    else
	return 1;
}

/*4: non-interactive 
*/
static int
FileReadConsole(char *prompt, char *buf, int len, int addhistory)
{
    if (!R_Slave) {
	fputs(prompt, stdout);
	fflush(stdout);
    }
    if (!fgets(buf, len, stdin))
	return 0;
    if (!R_Slave)
	fputs(buf, stdout);
    return 1;
}

static void
FileWriteConsole(char *buf, int len)
{
    printf("%s", buf);
}


static int (*TrueReadConsole) (char *, char *, int, int);
static void (*TrueWriteConsole) (char *, int);

 /* Fill a text buffer with user typed console input. */
int
R_ReadConsole(char *prompt, unsigned char *buf, int len, int addtohistory)
{
    ProcessEvents();
    return TrueReadConsole(prompt, buf, len, addtohistory);
}

	/* Write a text buffer to the console. */
	/* All system output is filtered through this routine. */

void R_WriteConsole(char *buf, int len)
{
    ProcessEvents();
    TrueWriteConsole(buf, len);
}


	/* Indicate that input is coming from the console */

void R_ResetConsole()
{
}


	/* Stdio support to ensure the console file buffer is flushed */

void R_FlushConsole()
{
    if (CharacterMode == RTerm) fflush(stdin);
    else if (CharacterMode == RGui) consoleflush(RConsole);
}


	/* Reset stdin if the user types EOF on the console. */

void R_ClearerrConsole()
{
    if (CharacterMode == RTerm)  clearerr(stdin);
}


/*
 *  3) ACTIONS DURING (LONG) COMPUTATIONS
 */

void GuiBusy(int which)
{
	if (which == 1) gsetcursor(RConsole, WatchCursor);
	if (which == 0) gsetcursor(RConsole, ArrowCursor);
}

void CharBusy(int which)
{
}

void R_Busy(int which)
{
    my_R_Busy(which);
}


/*
 *  4) INITIALIZATION AND TERMINATION ACTIONS
 */

/*
   R_CleanUp is invoked at the end of the session to give the user the
   option of saving their data.
   If ask == SA_SAVEASK the user should be asked if possible (and this
   option should not occur in non-interactive use).
   If ask = SA_SAVE or SA_NOSAVE the decision is known.
   If ask = SA_DEFAULT use the SaveAction set at startup.
   In all these cases run .Last() unless quitting is cancelled.
   If ask = SA_SUICIDE, no save, no .Last, possibly other things.
 */

void R_dot_Last(void);		/* in main.c */

void R_CleanUp(int saveact)
{
    if(saveact == SA_DEFAULT) /* The normal case apart from R_Suicide */
	saveact = SaveAction;

    if(saveact == SA_SAVEASK) {
	if(R_Interactive) {
	    switch (R_yesnocancel("Save workspace image?")) {
	    case YES:
		saveact = SA_SAVE;
		break;
	    case NO:
		saveact = SA_NOSAVE;
		break;
	    case CANCEL:
		jump_to_toplevel();
		break;
	    }
	} else saveact = SaveAction;
    }

    switch (saveact) {
    case SA_SAVE:
	R_dot_Last();
	if(R_DirtyImage) R_SaveGlobalEnv();
	break;
    case SA_NOSAVE:
	R_dot_Last();
	break;
    case SA_SUICIDE:
    default:
    }
    CleanEd();
    closeAllHlpFiles();
    KillAllDevices();
    AllDevicesKilled = 1;
    if (CharacterMode == RGui)
	savehistory(RConsole, ".Rhistory");
    UnLoad_Unzip_Dll();
    exitapp();
}

/*
 *  7) PLATFORM DEPENDENT FUNCTIONS
 */

    /* 
       This function can be used to display the named files with the 
       given titles and overall title.  On GUI platforms we could 
       use a read-only window to display the result.  Here we just 
       make up a temporary file and invoke a pager on it.
    */

    /*
     *     nfile   = number of files
     *     file    = array of filenames
     *     headers = the `headers' args of file.show. Printed before each file.
     *     wtitle  = title for window: the `title' arg of file.show
     *     del     = flag for whether files should be deleted after use
     *     pager   = pager to be used.
     */

int R_ShowFiles(int nfile, char **file, char **headers, char *wtitle,
		int del, char *pager)
{
    int   i;
    char  buf[1024];
    WIN32_FIND_DATA fd;

    if (nfile > 0) {
	if (pager == NULL || strlen(pager) == 0)
	    pager = "internal";
	for (i = 0; i < nfile; i++) {
	    if (FindFirstFile(file[i], &fd) != INVALID_HANDLE_VALUE) {
		if (!strcmp(pager, "internal")) {
		    newpager(wtitle, file[i], headers[i], del);
		} else if (!strcmp(pager, "console")) {
		    DWORD len = 1;
		    HANDLE f = CreateFile(file[i], GENERIC_READ,
					  FILE_SHARE_WRITE,
					  NULL, OPEN_EXISTING, 0, NULL);
		    if (f != INVALID_HANDLE_VALUE) {
			while (ReadFile(f, buf, 1023, &len, NULL) && len) {
			    buf[len] = '\0';
			    R_WriteConsole(buf,strlen(buf));
			}
			CloseHandle(f);
			if (del) DeleteFile(file[i]);
		    }
		    else {
			sprintf(buf,"Unable to open file '%s'", file[i]);
			warning(buf);
		    }
		} else {
		    sprintf(buf, "%s  %s", pager, file[i]);
		    runcmd(buf, 0, 1, "");
		}
	    } else {
		sprintf(buf, "file.show(): file %s does not exist\n", file[i]);
		warning(buf);
	    }
	}
	return 0;
    }
    return 1;
}


/* Prompt the user for a file name.  Return the length of */
/* the name typed.  On Gui platforms, this should bring up */
/* a dialog box so a user can choose files that way. */

extern int DialogSelectFile(char *buf, int len); /* from rui.c */

int R_ChooseFile(int new, char *buf, int len)
{
    return (DialogSelectFile(buf, len));
}

/* code for R_ShowMessage, R_YesNoCancel */

void (*pR_ShowMessage)(char *s);
void R_ShowMessage(char *s)
{
    (*pR_ShowMessage)(s);
}


static void char_message(char *s)
{
    if (!s) return;
    R_WriteConsole(s, strlen(s));
}

static int char_yesnocancel(char *s)
{
    char  a[3], ss[128];

    sprintf(ss, "%s [y/n/c]: ", s);
    R_ReadConsole(ss, a, 3, 0);
    switch (a[0]) {
    case 'y':
    case 'Y':
	return YES;
    case 'n':
    case 'N':
	return NO;
    default:
	return CANCEL;
    }
}


	/*--- Initialization Code ---*/

static char RHome[MAX_PATH + 7];
static char UserRHome[MAX_PATH + 7];
static char RUser[MAX_PATH];
char *getRHOME(); /* in rhome.c */
void setStartTime();

/* Process ~/.Renviron, if it exists */
#include "opt.h"

/* like putenv, but allocate storage */
static void Putenv(char *str)
{
    char *buf;
    buf = (char *) malloc((strlen(str) + 1) * sizeof(char));
    strcpy(buf, str);
    putenv(buf);
}

static void processRenviron()
{
    char *opt[2], optf[MAX_PATH], buf[80];
    int   ok;

    sprintf(optf, "%s/.Renviron", getenv("R_USER"));
    if (!optopenfile(optf))
	return;
    while ((ok = optread(opt, '='))) {
	sprintf(buf, "%s=%s", opt[0], opt[1]);
	Putenv(buf);
    }
    optclosefile();
}

void R_SetWin32(Rstart Rp)
{
    R_Home = Rp->rhome;
    sprintf(RHome, "R_HOME=%s", R_Home);
    putenv(RHome);
    strcpy(UserRHome, "R_USER=");
    strcat(UserRHome, Rp->home);
    putenv(UserRHome);

    CharacterMode = Rp->CharacterMode;
    TrueReadConsole = Rp->ReadConsole;
    TrueWriteConsole = Rp->WriteConsole;
    R_CallBackHook = Rp->CallBack;
    pR_ShowMessage = Rp->message;
    R_yesnocancel = Rp->yesnocancel;
    my_R_Busy = Rp->busy;
    /* Process ~/.Renviron, if it exists. */
    if(!Rp->NoRenviron) 
	processRenviron();
    /* in case caller uses getline */
    gl_events_hook = ProcessEvents;
    _controlfp(_MCW_EM, _MCW_EM);
}


/* Remove and process NAME=VALUE command line arguments */

static void env_command_line(int *pac, char **argv)
{
    int ac = *pac, newac = 1; /* Remember argv[0] is process name */
    char **av = argv;

    while(--ac) {
	if(strchr(*++av, '='))
	    Putenv(*av);
	else
	    argv[newac++] = *av;
    }
    *pac = newac;
}

int cmdlineoptions(int ac, char **av)
{
    int   i;
    char *p;
    char  s[1024];
    structRstart rstart;
    Rstart Rp = &rstart;

#ifdef HAVE_TIMES
    setStartTime();
#endif

    R_DefParams(Rp);
    for (i = 1; i < ac; i++)
	if (!strcmp(av[i], "--no-environ") || !strcmp(av[i], "--vanilla"))
		Rp->NoRenviron = True;

/* Here so that --ess and similar can change */
    Rp->CallBack = R_DoNothing;
    if (CharacterMode == RTerm) {
	if (isatty(0)) {
	    Rp->R_Interactive = True;
	    LastLine[0] = 0;
	    Rp->ReadConsole = CharReadConsole;
	    Rp->WriteConsole = CharWriteConsole;
	} else {
	    Rp->R_Interactive = False;
	    R_Consolefile = stdout; /* used for errors */
	    R_Outputfile = stdout;  /* used for sink-able output */
	    Rp->ReadConsole = FileReadConsole;
	    Rp->WriteConsole = FileWriteConsole;
	}
	Rp->message = char_message;
	Rp->yesnocancel = char_yesnocancel;
	Rp->busy = CharBusy;
    } else {
	Rp->R_Interactive = True;
	Rp->ReadConsole = GuiReadConsole;
	Rp->WriteConsole = GuiWriteConsole;
	Rp->message = askok;
	Rp->yesnocancel = askyesnocancel;
	Rp->busy = GuiBusy;
    }

    pR_ShowMessage = Rp->message; /* used here */
    TrueWriteConsole = Rp->WriteConsole;
    R_CallBackHook = Rp->CallBack;

    /* process environment variables
     * precedence:  command-line, .Renviron, inherited
     */
    if(!Rp->NoRenviron) {
	processRenviron();
	Rp->NoRenviron = True;
    }
    env_command_line(&ac, av);
    R_SizeFromEnv(Rp);

    R_common_command_line(&ac, av, Rp);

    while (--ac) {
	if (**++av == '-') {
	    if (!strcmp(*av, "--no-environ")) {
		Rp->NoRenviron = True;
	    } else if (!strcmp(*av, "--ess")) {
/* Assert that we are interactive even if input is from a file */
		Rp->R_Interactive = True;
		Rp->ReadConsole = PipeReadConsole;
	    } else if (!strcmp(*av, "--mdi")) {
		MDIset = 1;
	    } else if (!strcmp(*av, "--sdi") || !strcmp(*av, "--no-mdi")) {
		MDIset = -1;
	    } else {
		sprintf(s, "WARNING: unknown option %s\n", *av);
		R_ShowMessage(s);
		break;
	    }
	} else {
	    sprintf(s, "ARGUMENT '%s' __ignored__\n", *av);
	    R_ShowMessage(s);
	}
    }
    Rp->rhome = getRHOME();

/*
 * try R_USER then HOME then working directory
 */
    if (getenv("R_USER")) {
	strcpy(RUser, getenv("HOME"));
	p = RUser + (strlen(RUser) - 1);
	if (*p == '/' || *p == '\\') *p = '\0';
    } else {
	if (getenv("HOME")) {
	    strcpy(RUser, getenv("HOME"));
	    p = RUser + (strlen(RUser) - 1);
	    if (*p == '/' || *p == '\\') *p = '\0';
	} else
	    GetCurrentDirectory(MAX_PATH, RUser);
    }
    Rp->home = RUser;
    R_SetParams(Rp);

/*
 *  Since users' expectations for save/no-save will differ, we decided
 *  that they should be forced to specify in the non-interactive case.
 */
    if (!R_Interactive && SaveAction != SA_SAVE && SaveAction != SA_NOSAVE)
	R_Suicide("you must specify `--save', `--no-save' or `--vanilla'");

    return 0;
}
