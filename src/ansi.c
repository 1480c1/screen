/* Copyright (c) 1993
 *      Juergen Weigert (jnweiger@immd4.informatik.uni-erlangen.de)
 *      Michael Schroeder (mlschroe@immd4.informatik.uni-erlangen.de)
 * Copyright (c) 1987 Oliver Laumann
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *  
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING); if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 ****************************************************************
 */

#include "rcs.h"
RCS_ID("$Id$ FAU")

#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#ifndef sun	/* we want to know about TIOCPKT. */
# include <sys/ioctl.h>
#endif

#include "config.h"
#include "screen.h"
#include "extern.h"

extern struct win *windows;	/* linked list of all windows */
extern struct win *fore;
extern struct display *display, *displays;

extern int  force_vt;
extern int  all_norefresh;	/* => display */
extern int  ZombieKey_destroy, ZombieKey_resurrect;
extern int  real_uid, real_gid;
extern time_t Now;
extern struct NewWindow nwin_default;	/* for ResetWindow() */
extern int  nversion;

int maxwidth;			/* maximum of all widths so far */

int Z0width, Z1width;		/* widths for Z0/Z1 switching */

static struct win *curr;	/* window we are working on */
static int rows, cols;		/* window size of the curr window */

int visual_bell = 0;
int use_hardstatus = 1;
char *printcmd = 0;

char *blank;			/* line filled with spaces */
char *null;			/* line filled with '\0' */
char *OldImage, *OldAttr, *OldFont;	/* temporary buffers */

static void WinProcess __P((char **, int *));
static void WinRedisplayLine __P((int, int, int, int));
static void WinClearLine __P((int, int, int));
static int  WinRewrite __P((int, int, int, int));
static void WinSetCursor __P((void));
static int  WinResize __P((int, int));
static void WinRestore __P((void));
static int  Special __P((int));
static void DoESC __P((int, int));
static void DoCSI __P((int, int));
static void SetChar __P((int, int));
static void StartString __P((enum string_t));
static void SaveChar __P((int));
static void PrintStart __P((void));
static void PrintChar __P((int));
static void PrintFlush __P((void));
static void DesignateCharset __P((int, int));
static void MapCharset __P((int));
static void MapCharsetR __P((int));
static void SaveCursor __P((void));
static void RestoreCursor __P((void));
static void BackSpace __P((void));
static void Return __P((void));
static void LineFeed __P((int));
static void ReverseLineFeed __P((void));
static void InsertAChar __P((int, int));
static void InsertChar __P((int));
static void DeleteChar __P((int));
static void DeleteLine __P((int));
static void InsertLine __P((int));
static void ScrollUpMap __P((int));
static void ScrollDownMap __P((int));
static void Scroll __P((char *, int, int, char *));
static void ForwardTab __P((void));
static void BackwardTab __P((void));
static void ClearScreen __P((void));
static void ClearFromBOS __P((void));
static void ClearToEOS __P((void));
static void ClearFullLine __P((void));
static void ClearToEOL __P((void));
static void ClearFromBOL __P((void));
static void ClearInLine __P((int, int, int));
static void CursorRight __P((int));
static void CursorUp __P((int));
static void CursorDown __P((int));
static void CursorLeft __P((int));
static void ASetMode __P((int));
static void SelectRendition __P((void));
static void RestorePosAttrFont __P((void));
static void FillWithEs __P((void));
static void UpdateLine __P((char *, char *, char *, int, int, int ));
static void FindAKA __P((void));
static void Report __P((char *, int, int));
static void FixLine __P((void));
static void ScrollRegion __P((int));


/*
 *  The window layer functions
 */

struct LayFuncs WinLf =
{
  WinProcess,
  0,
  WinRedisplayLine,
  WinClearLine,
  WinRewrite,
  WinSetCursor,
  WinResize,
  WinRestore
};

static void
WinProcess(bufpp, lenp)
char **bufpp;
int *lenp;
{
  int addlf, l2 = 0, f, *ilen, l = *lenp;
  char *ibuf, *p, *buf = *bufpp;
  
  fore = D_fore;
  /* if w_wlock is set, only one user may write, else we check acls */
  if (fore->w_ptyfd < 0)
    {
      while ((*lenp)-- > 0)
        {
	  f = *(*bufpp)++;
	  if (f == ZombieKey_destroy)
	    {
	      debug2("Turning undead: %d(%s)\n", fore->w_number, fore->w_title);
	      KillWindow(fore);
	      l2--;
	      break;
	    }
	  if (f == ZombieKey_resurrect)
	    {
	      SetCurr(fore);

	      debug1("Resurrecting Zombie: %d\n", fore->w_number);
	      LineFeed(2);
	      RemakeWindow(fore);
	      l2++;
	      break;
	    }
	}
      if (!l2)
        {
	  char b1[10], b2[10];

	  b1[AddXChar(b1, ZombieKey_destroy)] = '\0';
	  b2[AddXChar(b2, ZombieKey_resurrect)] = '\0';
	  Msg(0, "Press %s to destroy or %s to resurrect window", b1, b2);
	}
      *bufpp += *lenp;
      *lenp = 0;
      return;
    }
#ifdef MULTIUSER
  if ((fore->w_wlock == WLOCK_OFF) ? 
      AclCheckPermWin(D_user, ACL_WRITE, fore) :
      (D_user != fore->w_wlockuser))
    {
      SetCurr(fore);
      Special('\007');
      *bufpp += *lenp;
      *lenp = 0;
      return;
    }
#endif /* MULTIUSER */
#ifdef PSEUDOS
  if (W_UWP(fore))
    {
      /* we send the user input to our pseudowin */
      ibuf = fore->w_pwin->p_inbuf; ilen = &fore->w_pwin->p_inlen;
      f = sizeof(fore->w_pwin->p_inbuf) - *ilen;
    }
  else
#endif /* PSEUDOS */
    {
      /* we send the user input to the window */
      ibuf = fore->w_inbuf; ilen = &fore->w_inlen;
      f = sizeof(fore->w_inbuf) - *ilen;
    }

  buf = *bufpp;

  while (l)
    {
      l2 = l;
      addlf = 0;
      if (fore->w_autolf)
	{
	  for (p = buf; l2; p++, l2--)
	    if (*p == '\r')
	      {
		l2--;
		addlf = 1;
		break;
	      }
	  l2 = l - l2;
	}
      if (l2 + addlf > f)
	{
	  debug1("Yuck! pty buffer full (%d chars missing). lets beep\n", l - f);
	  SetCurr(fore);
	  Special('\007');
	  l = l2 = f;
	  addlf = 0;
	}
      if (l2 > 0)
	{
	  bcopy(buf, ibuf + *ilen, l2);
	  *ilen += l2;
	  f -= l2;
	  buf += l2;
	  l -= l2;
	  if (f && addlf)
	    {
	      ibuf[(*ilen)++] = '\n';
	      f--;
	    }
	}
    }
  *bufpp += *lenp;
  *lenp = 0;
}

static void
WinRedisplayLine(y, from, to, isblank)
int y, from, to, isblank;
{
  if (y < 0)
    return;
  fore = D_fore;
  DisplayLine(isblank ? blank: null, null, null, fore->w_image[y],
              fore->w_attr[y], fore->w_font[y], y, from, to);
}

static int
WinRewrite(y, x1, x2, doit)
int y, x1, x2, doit;
{
  register int cost, dx;
  register char *p, *f, *i;

  fore = D_fore;
  dx = x2 - x1;
  if (doit)
    {
      i = fore->w_image[y] + x1;
      while (dx-- > 0)
	PUTCHAR(*i++);
      return(0);
    }
  p = fore->w_attr[y] + x1;
  f = fore->w_font[y] + x1;

  cost = dx = x2 - x1;
  if (D_insert)
    cost += D_EIcost + D_IMcost;
  while(dx-- > 0)
    {
      if (*p++ != D_attr || *f++ != D_font)
	return EXPENSIVE;
    }
  return cost;
}

static void
WinClearLine(y, xs, xe)
int y, xs, xe;
{
  fore = D_fore;
  DisplayLine(fore->w_image[y], fore->w_attr[y], fore->w_font[y],
	      blank, null, null, y, xs, xe);
}

static void
WinSetCursor()
{
  fore = D_fore;
  GotoPos(fore->w_x, fore->w_y);
}

static int
WinResize(wi, he)
int wi, he;
{
  fore = D_fore;
  if (fore)
    ChangeWindowSize(fore, wi, he);
  return 0;
}

static void
WinRestore()
{
  fore = D_fore;
  ChangeScrollRegion(fore->w_top, fore->w_bot);
  KeypadMode(fore->w_keypad);
  CursorkeysMode(fore->w_cursorkeys);
  SetFlow(fore->w_flow & FLOW_NOW);
  InsertMode(fore->w_insert);
  ReverseVideo(fore->w_revvid);
  CursorInvisible(fore->w_curinv);
  fore->w_active = 1;
}

/* 
 *  Activate - make fore window active
 *  norefresh = -1 forces a refresh, disregard all_norefresh then.
 */
void
Activate(norefresh)
int norefresh;
{
  debug1("Activate(%d)\n", norefresh);
  if (display == 0)
    return;
  RemoveStatus();
  fore = D_fore;
  if (fore)
    {
      ASSERT(fore->w_display == display);
      fore->w_active = D_layfn == &WinLf;
      if (fore->w_monitor != MON_OFF)
	fore->w_monitor = MON_ON;
      fore->w_bell = BELL_OFF;
      if (ResizeDisplay(fore->w_width, fore->w_height))
	{
	  debug2("Cannot resize from (%d,%d)", D_width, D_height);
	  debug2(" to (%d,%d) -> resize window\n", fore->w_width, fore->w_height);
	  DoResize(D_width, D_height);
	}
    }
  Redisplay(norefresh + all_norefresh);
}

void
ResetWindow(p)
register struct win *p;
{
  register int i;

  p->w_wrap = nwin_default.wrap;
  p->w_origin = 0;
  p->w_insert = 0;
  p->w_revvid = 0;
  p->w_curinv = 0;
  p->w_autolf = 0;
  p->w_keypad = 0;
  p->w_cursorkeys = 0;
  p->w_top = 0;
  p->w_bot = p->w_height - 1;
  p->w_saved = 0;
  p->w_Attr = 0;
  p->w_Font = 0;
  p->w_FontR = 0;
  p->w_x = p->w_y = 0;
  p->w_state = LIT;
  p->w_StringType = NONE;
  bzero(p->w_tabs, p->w_width);
  for (i = 8; i < p->w_width; i += 8)
    p->w_tabs[i] = 1;
  ResetCharsets(p);
}

void
ResetCharsets(p)
register struct win *p;
{
  int i;

  p->w_Charset = G0;
  p->w_CharsetR = G2;
  for (i = G0; i <= G3; i++)
    p->w_charsets[i] = ASCII;
  p->w_gr = nwin_default.gr;
  p->w_c1 = nwin_default.c1;
  p->w_ss = 0;
#ifdef KANJI
  switch(p->w_kanji)
    {
    case SJIS:
      p->w_CharsetR = G1;
      p->w_charsets[G1] = KANA;
      p->w_gr = 1;
      p->w_c1 = 0;
      break;
    case EUC:
      p->w_CharsetR = G1;
      p->w_charsets[G1] = KANJI;
      p->w_charsets[G2] = KANA;
      p->w_gr = 1;
      p->w_c1 = 1;
      break;
    default:
      break;
    }
#endif
  p->w_Font = p->w_charsets[p->w_Charset];
  p->w_FontR = p->w_charsets[p->w_CharsetR];
}

static void
FixLine()
{
  if (curr->w_Attr && curr->w_attr[curr->w_y] == null)
    {
      if ((curr->w_attr[curr->w_y] = (char *)malloc(curr->w_width + 1)) == 0)
	{
	  curr->w_attr[curr->w_y] = null;
	  curr->w_Attr = 0;
	  Msg(0, "Warning: no space for attr - turned off");
	}
      else
	bzero(curr->w_attr[curr->w_y], curr->w_width + 1);
    }
  if ((curr->w_Font || curr->w_FontR) && curr->w_font[curr->w_y] == null)
    {
      if ((curr->w_font[curr->w_y] = (char *)malloc(curr->w_width + 1)) == 0)
	{
	  curr->w_font[curr->w_y] = null;
	  curr->w_Font = curr->w_charsets[curr->w_ss ? curr->w_ss : curr->w_Charset] = 0;
	  curr->w_FontR = curr->w_charsets[curr->w_ss ? curr->w_ss : curr->w_CharsetR] = 0;
	  Msg(0, "Warning: no space for font - turned off");
	}
      else
	bzero(curr->w_font[curr->w_y], curr->w_width + 1);
    }
}


/*
 *  Here comes the vt100 emulator
 */
void
WriteString(wp, buf, len)
struct win *wp;
register char *buf;
register int len;
{
  register int c, font;

  if (!len)
    return;
  if (wp->w_logfp != NULL)
    if ((int)fwrite(buf, len, 1, wp->w_logfp) < 1)
      {
	Msg(errno, "Error writing logfile");
	fclose(wp->w_logfp);
	wp->w_logfp = NULL;
      }
  /*
   * SetCurr() here may prevent output, as it may set display = 0
   */
  SetCurr(wp);
  if (display)
    {
      if (D_status && !(use_hardstatus && D_HS))
	RemoveStatus();
    }
  else
    {
      if (curr->w_tstamp.seconds)
        curr->w_tstamp.lastio = Now;

      if (curr->w_monitor == MON_ON || curr->w_monitor == MON_DONE)
	{
          debug2("ACTIVITY %d %d\n", curr->w_monitor, curr->w_bell);
          curr->w_monitor = MON_FOUND;
	}
    }
  do
    {
      c = (unsigned char)*buf++;

      /* The next part is only for speedup */
      if (curr->w_state == LIT &&
#ifdef KANJI
          curr->w_Font != KANJI && curr->w_Font != KANA && !curr->w_mbcs &&
#endif
          c >= ' ' && 
          ((c & 0x80) == 0 || ((c >= 0xa0 || !curr->w_c1) && !curr->w_gr && (display == 0 || !D_CB8))) &&
          !curr->w_insert && !curr->w_ss && curr->w_x < cols - 1)
	{
	  register int currx;
	  register char *imp, *atp, *fop, at, fo;

	  if (c == '\177')
	    continue;
	  FixLine();
	  currx = curr->w_x;
	  imp = curr->w_image[curr->w_y] + currx;
	  atp = curr->w_attr[curr->w_y] + currx;
	  fop = curr->w_font[curr->w_y] + currx;
	  at = curr->w_Attr;
	  fo = curr->w_Font;
	  if (display)
	    {
	      if (D_x != currx || D_y != curr->w_y)
		GotoPos(currx, curr->w_y);
	      if (at != D_attr)
		SetAttr(at);
	      if (fo != D_font)
		SetFont(fo);
	      if (D_insert)
		InsertMode(0);
	    }
	  while (currx < cols - 1)
	    {
	      if (display)
		AddChar(D_font != '0' ? c : D_c0_tab[c]);
	      *imp++ = c;
	      *atp++ = at;
	      *fop++ = fo;
	      currx++;
skip:	      if (--len == 0)
		break;
              c = (unsigned char)*buf++;
	      if (c == '\177')
		goto skip;
	      if (c < ' ' || ((c & 0x80) && ((c < 0xa0 && curr->w_c1) || curr->w_gr || (display && D_CB8))))
		break;
	    }
	  curr->w_x = currx;
	  if (display)
	    D_x = currx;
	  if (len == 0)
	    break;
	}
      /* end of speedup code */

      if ((c & 0x80) && display && D_CB8)
	{
	  FILE *logfp = wp->w_logfp;
	  char *cb8 = D_CB8;
	
	  wp->w_logfp = NULL;	/* a little hack */
	  D_CB8 = NULL;		/* dito */
	  WriteString(wp, cb8, (int)strlen(cb8));
	  wp->w_logfp = logfp;
	  D_CB8 = cb8;
	  c &= 0x7f;
	}
    tryagain:
      switch (curr->w_state)
	{
	case PRIN:
	  switch (c)
	    {
	    case '\033':
	      curr->w_state = PRINESC;
	      break;
	    default:
	      PrintChar(c);
	    }
	  break;
	case PRINESC:
	  switch (c)
	    {
	    case '[':
	      curr->w_state = PRINCSI;
	      break;
	    default:
	      PrintChar('\033');
	      PrintChar(c);
	      curr->w_state = PRIN;
	    }
	  break;
	case PRINCSI:
	  switch (c)
	    {
	    case '4':
	      curr->w_state = PRIN4;
	      break;
	    default:
	      PrintChar('\033');
	      PrintChar('[');
	      PrintChar(c);
	      curr->w_state = PRIN;
	    }
	  break;
	case PRIN4:
	  switch (c)
	    {
	    case 'i':
	      curr->w_state = LIT;
	      PrintFlush();
	      if (curr->w_pdisplay && curr->w_pdisplay->d_printfd >= 0)
		{
		  close(curr->w_pdisplay->d_printfd);
		  curr->w_pdisplay->d_printfd = -1;
		}
	      curr->w_pdisplay = 0;
	      break;
	    default:
	      PrintChar('\033');
	      PrintChar('[');
	      PrintChar('4');
	      PrintChar(c);
	      curr->w_state = PRIN;
	    }
	  break;
	case ASTR:
	  if (c == 0)
	    break;
	  if (c == '\033')
	    {
	      curr->w_state = STRESC;
	      break;
	    }
	  /* special xterm hack: accept SetStatus sequence. Yucc! */
	  if (!(curr->w_StringType == OSC && c < ' '))
	    if (!curr->w_c1 || c != ('\\' ^ 0xc0))
	      {
		SaveChar(c);
		break;
	      }
	  c = '\\';
	  /* FALLTHROUGH */
	case STRESC:
	  switch (c)
	    {
	    case '\\':
	      curr->w_state = LIT;
	      *curr->w_stringp = '\0';
	      switch (curr->w_StringType)
		{
		case OSC:	/* special xterm compatibility hack */
		  if (curr->w_stringp - curr->w_string < 2 ||
		      curr->w_string[0] < '0' ||
		      curr->w_string[0] > '2' ||
		      curr->w_string[1] != ';')
		    break;
		  curr->w_stringp -= 2;
		  if (curr->w_stringp > curr->w_string)
		    bcopy(curr->w_string + 2, curr->w_string, curr->w_stringp - curr->w_string);
		  *curr->w_stringp = '\0';
		  /* FALLTHROUGH */
		case APC:
		  if (curr->w_hstatus)
		    {
		      if (strcmp(curr->w_hstatus, curr->w_string) == 0)
			break;	/* not changed */
		      free(curr->w_hstatus);
		      curr->w_hstatus = 0;
		    }
		  if (curr->w_string != curr->w_stringp)
		    curr->w_hstatus = SaveStr(curr->w_string);
		  if (display)
		    RefreshStatus();
		  break;
		case GM:
		    {
		      struct display *old = display;
		      for (display = displays; display; display = display->d_next)
			if (display != old)
			  MakeStatus(curr->w_string);
		      display = old;
		    }
		  /*FALLTHROUGH*/
		case PM:
		  if (!display)
		    break;
		  MakeStatus(curr->w_string);
		  if (D_status && !(use_hardstatus && D_HS) && len > 1)
		    {
		      curr->w_outlen = len - 1;
		      bcopy(buf, curr->w_outbuf, curr->w_outlen);
		      return;
		    }
		  break;
		case DCS:
		  if (display)
		    AddStr(curr->w_string);
		  break;
		case AKA:
		  if (curr->w_title == curr->w_akabuf && !*curr->w_string)
		    break;
		  ChangeAKA(curr, curr->w_string, 20);
		  if (!*curr->w_string)
		    curr->w_autoaka = curr->w_y + 1;
		  break;
		default:
		  break;
		}
	      break;
	    case '\033':
	      SaveChar('\033');
	      break;
	    default:
	      curr->w_state = ASTR;
	      SaveChar('\033');
	      SaveChar(c);
	      break;
	    }
	  break;
	case ESC:
	  switch (c)
	    {
	    case '[':
	      curr->w_NumArgs = 0;
	      curr->w_intermediate = 0;
	      bzero((char *) curr->w_args, MAXARGS * sizeof(int));
	      curr->w_state = CSI;
	      break;
	    case ']':
	      StartString(OSC);
	      break;
	    case '_':
	      StartString(APC);
	      break;
	    case 'P':
	      StartString(DCS);
	      break;
	    case '^':
	      StartString(PM);
	      break;
	    case '!':
	      StartString(GM);
	      break;
	    case '"':
	    case 'k':
	      StartString(AKA);
	      break;
	    default:
	      if (Special(c))
	        {
		  curr->w_state = LIT;
		  break;
		}
	      debug1("not special. c = %x\n", c);
	      if (c >= ' ' && c <= '/')
		{
		  if (curr->w_intermediate)
#ifdef KANJI
		    if (curr->w_intermediate == '$')
		      c |= '$' << 8;
		    else
#endif
		    c = -1;
		  curr->w_intermediate = c;
		}
	      else if (c >= '0' && c <= '~')
		{
		  DoESC(c, curr->w_intermediate);
		  curr->w_state = LIT;
		}
	      else
		{
		  curr->w_state = LIT;
		  goto tryagain;
		}
	    }
	  break;
	case CSI:
	  switch (c)
	    {
	    case '0': case '1': case '2': case '3': case '4':
	    case '5': case '6': case '7': case '8': case '9':
	      if (curr->w_NumArgs < MAXARGS)
		{
		  curr->w_args[curr->w_NumArgs] =
		    10 * curr->w_args[curr->w_NumArgs] + (c - '0');
		}
	      break;
	    case ';':
	    case ':':
	      curr->w_NumArgs++;
	      break;
	    default:
	      if (Special(c))
		break;
	      if (c >= '@' && c <= '~')
		{
		  curr->w_NumArgs++;
		  DoCSI(c, curr->w_intermediate);
		  if (curr->w_state != PRIN)
		    curr->w_state = LIT;
		}
	      else if ((c >= ' ' && c <= '/') || (c >= '<' && c <= '?'))
		curr->w_intermediate = curr->w_intermediate ? -1 : c;
	      else
		{
		  curr->w_state = LIT;
		  goto tryagain;
		}
	    }
	  break;
	case LIT:
	default:
	  if (c < ' ')
	    {
	      if (c == '\033')
		{
		  curr->w_intermediate = 0;
		  curr->w_state = ESC;
		  if (display && D_lp_missing && (D_CIC || D_IC || D_IM))
		    UpdateLine(blank, null, null, D_bot, cols - 2, cols - 1);
		  if (curr->w_autoaka < 0)
		    curr->w_autoaka = 0;
		}
	      else
		Special(c);
	      break;
	    }
	  if (c >= 0x80 && c < 0xa0 && curr->w_c1)
	    {
	      switch (c)
		{
		case 0xc0 ^ 'D':
		case 0xc0 ^ 'E':
		case 0xc0 ^ 'H':
		case 0xc0 ^ 'M':
		case 0xc0 ^ 'N':
		case 0xc0 ^ 'O':
		  DoESC(c ^ 0xc0, 0);
		  break;
		case 0xc0 ^ '[':
		  if (display && D_lp_missing && (D_CIC || D_IC || D_IM))
		    UpdateLine(blank, null, null, D_bot, cols - 2, cols - 1);
		  if (curr->w_autoaka < 0)
		    curr->w_autoaka = 0;
		  curr->w_NumArgs = 0;
		  curr->w_intermediate = 0;
		  bzero((char *) curr->w_args, MAXARGS * sizeof(int));
		  curr->w_state = CSI;
		  break;
		case 0xc0 ^ 'P':
		  StartString(DCS);
		  break;
		default:
		  break;
		}
	      break;
	    }

	  font = c >= 0x80 ? curr->w_FontR : curr->w_Font;
#ifdef KANJI
	  if (font == KANA && curr->w_kanji == SJIS && curr->w_mbcs == 0)
	    {
	      /* Lets see if it is the first byte of a kanji */
	      debug1("%x may be first of SJIS\n", c);
	      if ((0x81 <= c && c <= 0x9f) || (0xe0 <= c && c <= 0xef))
		{
		  debug("YES!\n");
		  curr->w_mbcs = c;
		  break;
		}
	    }
	  if (font == KANJI || curr->w_mbcs)
	    {
	      int t = c;
	      if (curr->w_mbcs == 0)
		{
		  curr->w_mbcs = c;
		  break;
		}
	      if (curr->w_x == cols - 1)
		{
		  curr->w_x += curr->w_wrap ? 1 : -1;
		  debug1("Patched w_x to %d\n", curr->w_x);
		}
	      c = curr->w_mbcs;
	      if (font != KANJI)
		{
		  debug2("SJIS !! %x %x\n", c, t);
		  if (0x40 <= t && t <= 0xfc && t != 0x7f)
		    {
		      if (c <= 0x9f) c = (c - 0x81) * 2 + 0x21;
		      else c = (c - 0xc1) * 2 + 0x21;
		      if (t <= 0x7e) t -= 0x1f;
		      else if (t <= 0x9e) t -= 0x20;
		      else t -= 0x7e, c++;
		      font = KANJI;
		    }
		  else
		    {
		      /* Incomplete shift-jis - skip first byte */
		      c = t;
		      t = 0;
		    }
		  debug2("SJIS after %x %x\n", c, t);
		}
	      curr->w_mbcs = t;
	    }
	  kanjiloop:
#endif
	  if (curr->w_gr)
	    {
	      c &= 0x7f;
	      if (c < ' ')	/* this is ugly but kanji support */
		goto tryagain;	/* prevents nicer programming */
	    }
	  if (c == '\177')
	    break;
	  if (display)
	    {
	      if (D_attr != curr->w_Attr)
		SetAttr(curr->w_Attr);
	      if (D_font != font)
		SetFont(font);
	    }
	  if (curr->w_x < cols - 1)
	    {
	      if (curr->w_insert)
		InsertAChar(c, font);
	      else
		{
		  if (display)
		    PUTCHAR(c);
		  SetChar(c, font);
		  curr->w_x++;
		}
	    }
	  else if (curr->w_x == cols - 1)
	    {
	      if (display && curr->w_wrap && (D_CLP || !force_vt || D_COP))
		{
		  RAW_PUTCHAR(c);	/* don't care about D_insert */
		  SetChar(c, font);
		  curr->w_x++;
		  if (D_AM && !D_CLP)
		    {
                      SetChar(0, 0);
		      LineFeed(0);	/* terminal auto-wrapped */
		    }
		}
	      else
		{
		  if (display)
		    {
		      if (D_CLP || curr->w_y != D_bot)
			{
			  RAW_PUTCHAR(c);
			  GotoPos(curr->w_x, curr->w_y);
			}
		      else
			CheckLP(c);
		    }
		  SetChar(c, font);
		  if (curr->w_wrap)
		    curr->w_x++;
		}
	    }
	  else /* curr->w_x > cols - 1 */
	    {
              SetChar(0, 0);		/* we wrapped */
	      if (curr->w_insert)
		{
		  LineFeed(2);		/* cr+lf, handle LP */
		  InsertAChar(c, font);
		}
	      else
		{
		  if (display && D_AM && D_x != cols)	/* write char again */
		    {
		      SetAttrFont(curr->w_attr[curr->w_y][cols - 1],
                                  curr->w_font[curr->w_y][cols - 1]);
		      RAW_PUTCHAR(curr->w_image[curr->w_y][cols - 1]);
		      SetAttrFont(curr->w_Attr, font);
		      if (curr->w_y == D_bot)
			D_lp_missing = 0;	/* just wrote it */
		    }
		  LineFeed((display == 0 || D_AM) ? 0 : 2);
		  if (display)
		    PUTCHAR(c);
		  SetChar(c, font);
		  curr->w_x = 1;
		}
	    }
#ifdef KANJI
	  if (curr->w_mbcs)
	    {
	      c = curr->w_mbcs;
	      curr->w_mbcs = 0;
	      goto kanjiloop;	/* what a hack! */
	    }
#endif
	  if (curr->w_ss)
	    {
	      SetFont(curr->w_Font = curr->w_charsets[curr->w_Charset]);
	      curr->w_FontR = curr->w_charsets[curr->w_CharsetR];
	      curr->w_ss = 0;
	    }
	  break;
	}
    }
  while (--len);
  curr->w_outlen = 0;
  if (curr->w_state == PRIN)
    PrintFlush();
}

static int
Special(c)
register int c;
{
  switch (c)
    {
    case '\b':
      BackSpace();
      return 1;
    case '\r':
      Return();
      return 1;
    case '\n':
      if (curr->w_autoaka)
	FindAKA();
      LineFeed(1);
      return 1;
    case '\007':
      if (display == 0)
	curr->w_bell = BELL_ON;
      else
	{
	  if (!visual_bell)
	    PutStr(D_BL);
	  else
	    {
	      if (!D_VB)
		curr->w_bell = BELL_VISUAL;
	      else
		PutStr(D_VB);
	    }
	}
      return 1;
    case '\t':
      ForwardTab();
      return 1;
    case '\017':		/* SI */
      MapCharset(G0);
      return 1;
    case '\016':		/* SO */
      MapCharset(G1);
      return 1;
    }
  return 0;
}

static void
DoESC(c, intermediate)
int c, intermediate;
{
  debug2("DoESC: %x - inter = %x\n", c, intermediate);
  switch (intermediate)
    {
    case 0:
      switch (c)
	{
	case 'E':
	  LineFeed(2);
	  break;
	case 'D':
	  LineFeed(1);
	  break;
	case 'M':
	  ReverseLineFeed();
	  break;
	case 'H':
	  curr->w_tabs[curr->w_x] = 1;
	  break;
	case 'Z':		/* jph: Identify as VT100 */
	  Report("\033[?%d;%dc", 1, 2);
	  break;
	case '7':
	  SaveCursor();
	  break;
	case '8':
	  RestoreCursor();
	  break;
	case 'c':
	  ClearScreen();
	  ResetWindow(curr);
	  SetAttrFont(0, ASCII);
	  InsertMode(0);
	  KeypadMode(0);
	  CursorkeysMode(0);
	  ChangeScrollRegion(0, rows - 1);
	  break;
	case '=':
	  KeypadMode(curr->w_keypad = 1);
#ifndef TIOCPKT
	  NewAutoFlow(curr, 0);
#endif /* !TIOCPKT */
	  break;
	case '>':
	  KeypadMode(curr->w_keypad = 0);
#ifndef TIOCPKT
	  NewAutoFlow(curr, 1);
#endif /* !TIOCPKT */
	  break;
	case 'n':		/* LS2 */
	  MapCharset(G2);
	  break;
	case 'o':		/* LS3 */
	  MapCharset(G3);
	  break;
	case '~':
	  MapCharsetR(G1);	/* LS1R */
	  break;
	case '}':
	  MapCharsetR(G2);	/* LS2R */
	  break;
	case '|':
	  MapCharsetR(G3);	/* LS3R */
	  break;
	case 'N':		/* SS2 */
	  if (curr->w_charsets[curr->w_Charset] != curr->w_charsets[G2]
	      || curr->w_charsets[curr->w_CharsetR] != curr->w_charsets[G2])
	    {
	      curr->w_FontR = curr->w_Font = curr->w_charsets[curr->w_ss = G2];
	    }
	  else
	    curr->w_ss = 0;
	  break;
	case 'O':		/* SS3 */
	  if (curr->w_charsets[curr->w_Charset] != curr->w_charsets[G3]
	      || curr->w_charsets[curr->w_CharsetR] != curr->w_charsets[G3])
	    curr->w_FontR = curr->w_Font = curr->w_charsets[curr->w_ss = G3];
	  else
	    curr->w_ss = 0;
	  break;
	}
      break;
    case '#':
      switch (c)
	{
	case '8':
	  FillWithEs();
	  break;
	}
      break;
    case '(':
      DesignateCharset(c, G0);
      break;
    case ')':
      DesignateCharset(c, G1);
      break;
    case '*':
      DesignateCharset(c, G2);
      break;
    case '+':
      DesignateCharset(c, G3);
      break;
#ifdef KANJI
/*
 * ESC $ ( Fn: invoke multi-byte charset, Fn, to G0
 * ESC $ Fn: same as above.  (old sequence)
 * ESC $ ) Fn: invoke multi-byte charset, Fn, to G1
 * ESC $ * Fn: invoke multi-byte charset, Fn, to G2
 * ESC $ + Fn: invoke multi-byte charset, Fn, to G3
 */
    case '$':
    case '$'<<8 | '(':
      DesignateCharset(c & 037, G0);
      break;
    case '$'<<8 | ')':
      DesignateCharset(c & 037, G1);
      break;
    case '$'<<8 | '*':
      DesignateCharset(c & 037, G2);
      break;
    case '$'<<8 | '+':
      DesignateCharset(c & 037, G3);
      break;
#endif
    }
}

static void
DoCSI(c, intermediate)
int c, intermediate;
{
  register int i, a1 = curr->w_args[0], a2 = curr->w_args[1];

  if (curr->w_NumArgs > MAXARGS)
    curr->w_NumArgs = MAXARGS;
  switch (intermediate)
    {
    case 0:
      switch (c)
	{
	case 'H':
	case 'f':
	  if (a1 < 1)
	    a1 = 1;
	  if (curr->w_origin)
	    a1 += curr->w_top;
	  if (a1 > rows)
	    a1 = rows;
	  if (a2 < 1)
	    a2 = 1;
	  if (a2 > cols)
	    a2 = cols;
	  GotoPos(--a2, --a1);
	  curr->w_x = a2;
	  curr->w_y = a1;
	  if (curr->w_autoaka)
	    curr->w_autoaka = a1 + 1;
	  break;
	case 'J':
	  if (a1 < 0 || a1 > 2)
	    a1 = 0;
	  switch (a1)
	    {
	    case 0:
	      ClearToEOS();
	      break;
	    case 1:
	      ClearFromBOS();
	      break;
	    case 2:
	      ClearScreen();
	      GotoPos(curr->w_x, curr->w_y);
	      break;
	    }
	  break;
	case 'K':
	  if (a1 < 0 || a1 > 2)
	    a1 %= 3;
	  switch (a1)
	    {
	    case 0:
	      ClearToEOL();
	      break;
	    case 1:
	      ClearFromBOL();
	      break;
	    case 2:
	      ClearFullLine();
	      break;
	    }
	  break;
	case 'A':
	  CursorUp(a1 ? a1 : 1);
	  break;
	case 'B':
	  CursorDown(a1 ? a1 : 1);
	  break;
	case 'C':
	  CursorRight(a1 ? a1 : 1);
	  break;
	case 'D':
	  CursorLeft(a1 ? a1 : 1);
	  break;
	case 'm':
	  SelectRendition();
	  break;
	case 'g':
	  if (a1 == 0)
	    curr->w_tabs[curr->w_x] = 0;
	  else if (a1 == 3)
	    bzero(curr->w_tabs, cols);
	  break;
	case 'r':
	  if (!a1)
	    a1 = 1;
	  if (!a2)
	    a2 = rows;
	  if (a1 < 1 || a2 > rows || a1 >= a2)
	    break;
	  curr->w_top = a1 - 1;
	  curr->w_bot = a2 - 1;
	  ChangeScrollRegion(curr->w_top, curr->w_bot);
	  if (curr->w_origin)
	    {
	      GotoPos(0, curr->w_top);
	      curr->w_y = curr->w_top;
	      curr->w_x = 0;
	    }
	  else
	    {
	      GotoPos(0, 0);
	      curr->w_y = curr->w_x = 0;
	    }
	  break;
	case 's':
	  SaveCursor();
	  break;
	case 't':
	  if (a1 != 8)
	    break;
	  a1 = curr->w_args[2];
	  if (a1 < 1)
	    a1 = curr->w_width;
	  if (a2 < 1)
	    a2 = curr->w_height;
	  if (display && D_CWS == NULL)
	    {
	      a2 = curr->w_height;
	      if (D_CZ0 == NULL || (a1 != Z0width && a1 != Z1width))
	        a1 = curr->w_width;
 	    }
	  if (a1 == curr->w_width && a2 == curr->w_height)
	    break;
          ChangeWindowSize(curr, a1, a2);
	  SetCurr(curr);
	  if (display)
	    Activate(0);
	  break;
	case 'u':
	  RestoreCursor();
	  break;
	case 'I':
	  if (!a1)
	    a1 = 1;
	  while (a1--)
	    ForwardTab();
	  break;
	case 'Z':
	  if (!a1)
	    a1 = 1;
	  while (a1--)
	    BackwardTab();
	  break;
	case 'L':
	  InsertLine(a1 ? a1 : 1);
	  break;
	case 'M':
	  DeleteLine(a1 ? a1 : 1);
	  break;
	case 'P':
	  DeleteChar(a1 ? a1 : 1);
	  break;
	case '@':
	  InsertChar(a1 ? a1 : 1);
	  break;
	case 'h':
	  ASetMode(1);
	  break;
	case 'l':
	  ASetMode(0);
	  break;
	case 'i':
	  if (display && a1 == 5)
	    PrintStart();
	  break;
	case 'n':
	  if (a1 == 5)		/* Report terminal status */
	    Report("\033[0n", 0, 0);
	  else if (a1 == 6)		/* Report cursor position */
	    Report("\033[%d;%dR", curr->w_y + 1, curr->w_x + 1);
	  break;
	case 'c':		/* Identify as VT100 */
	  if (a1 == 0)
	    Report("\033[?%d;%dc", 1, 2);
	  break;
	case 'x':		/* decreqtparm */
	  if (a1 == 0 || a1 == 1)
	    Report("\033[%d;1;1;112;112;1;0x", a1 + 2, 0);
	  break;
	case 'p':		/* obscure code from a 97801 term */
	  if (a1 == 6 || a1 == 7)
	    CursorInvisible(curr->w_curinv = 7 - a1);
	  break;
	case 'S':		/* obscure code from a 97801 term */
	  ScrollRegion(a1 ? a1 : 1);
	  break;
	case 'T':		/* obscure code from a 97801 term */
	  ScrollRegion(a1 ? -a1 : -1);
	  break;
	}
      break;
    case '?':
      for (a2 = 0; a2 < curr->w_NumArgs; a2++)
	{
	  a1 = curr->w_args[a2];
	  debug2("\\E[?%d%c\n",a1,c);
	  if (c != 'h' && c != 'l')
	    break;
	  i = (c == 'h');
	  switch (a1)
	    {
	    case 1:	/* CKM:  cursor key mode */
	      CursorkeysMode(curr->w_cursorkeys = i);
#ifndef TIOCPKT
	      NewAutoFlow(curr, !i);
#endif /* !TIOCPKT */
	      break;
	    case 2:	/* ANM:  ansi/vt52 mode */
	      if (i)
		{
#ifdef KANJI
		  if (curr->w_kanji)
		    break;
#endif
		  curr->w_charsets[0] = curr->w_charsets[1] =
		    curr->w_charsets[2] = curr->w_charsets[2] =
		    curr->w_Font = curr->w_FontR = ASCII;
		  curr->w_Charset = 0;
		  curr->w_CharsetR = 2;
		  curr->w_ss = 0;
		}
	      break;
	    case 3:	/* COLM: column mode */
	      i = (i ? Z0width : Z1width);
	      if (curr->w_width != i && (display == 0 || (D_CZ0 || D_CWS)))
		{
		  ChangeWindowSize(curr, i, curr->w_height);
		  SetCurr(curr);	/* update rows/cols */
		  if (display)
		    Activate(0);
		}
	      break;
	 /* case 4:	   SCLM: scrolling mode */
	    case 5:	/* SCNM: screen mode */
	      /* This should be reverse video.
	       * Because it is used in some termcaps to emulate
	       * a visual bell we do this hack here.
	       */
	      if (i)
		ReverseVideo(1);
	      else
		{
		  if (display && D_CVR)
		    ReverseVideo(0);
		  else
		    if (curr->w_revvid)
		      {
		        if (display && D_VB)
			  PutStr(D_VB);
		        else
		          curr->w_bell = BELL_VISUAL;
		      }
		}
	      curr->w_revvid = i;
	      break;
	    case 6:	/* OM:   origin mode */
	      if ((curr->w_origin = i) != 0)
		{
		  curr->w_y = curr->w_top;
		  curr->w_x = 0;
		}
	      else
		curr->w_y = curr->w_x = 0;
	      if (display)
		GotoPos(curr->w_x, curr->w_y);
	      break;
	    case 7:	/* AWM:  auto wrap mode */
	      curr->w_wrap = i;
	      break;
	 /* case 8:	   ARM:  auto repeat mode */
	 /* case 9:	   INLM: interlace mode */
	 /* case 10:	   EDM:  edit mode */
	 /* case 11:	   LTM:  line transmit mode */
	 /* case 13:	   SCFDM: space compression / field delimiting */
	 /* case 14:	   TEM:  transmit execution mode */
	 /* case 16:	   EKEM: edit key execution mode */
	    case 25:	/* TCEM: text cursor enable mode */
	      CursorInvisible(curr->w_curinv = !i);
	      break;
	 /* case 40:	   132 col enable */
	 /* case 42:	   NRCM: 7bit NRC character mode */
	 /* case 44:	   margin bell enable */
	    }
	}
      break;
    case '>':
      switch (c)
	{
	case 'c':	/* secondary DA */
	  if (a1 == 0)
	    Report("\033[>%d;%d;0c", 83, nversion);	/* 83 == 'S' */
	  break;
	}
      break;
    }
}


static void
SetChar(c, f)
register int c, f;
{
  register struct win *p = curr;

  FixLine();
  p->w_image[p->w_y][p->w_x] = c;
  p->w_attr[p->w_y][p->w_x] = p->w_Attr;
  p->w_font[p->w_y][p->w_x] = f;
}

static void
StartString(type)
enum string_t type;
{
  curr->w_StringType = type;
  curr->w_stringp = curr->w_string;
  curr->w_state = ASTR;
}

static void
SaveChar(c)
int c;
{
  if (curr->w_stringp >= curr->w_string + MAXSTR - 1)
    curr->w_state = LIT;
  else
    *(curr->w_stringp)++ = c;
}

static void
PrintStart()
{
  int pi[2];

  if (printcmd == 0 && D_PO == 0)
    return;
  curr->w_pdisplay = display;
  curr->w_stringp = curr->w_string;
  curr->w_state = PRIN;
  if (printcmd == 0 || curr->w_pdisplay->d_printfd >= 0)
    return;
  if (pipe(pi))
    {
      Msg(errno, "printing pipe");
      return;
    }
  switch (fork())
    {
    case -1:
      Msg(errno, "printing fork");
      return;
    case 0:
      close(0);
      dup(pi[0]);
      closeallfiles(0);
      if (setuid(real_uid) || setgid(real_gid))
        _exit(1);
#ifdef SIGPIPE
      signal(SIGPIPE, SIG_DFL);
#endif
      execl("/bin/sh", "sh", "-c", printcmd, 0);
      _exit(1);
    default:
      break;
    }
  close(pi[0]);
  curr->w_pdisplay->d_printfd = pi[1];
}

static void
PrintChar(c)
int c;
{
  if (curr->w_stringp >= curr->w_string + MAXSTR - 1)
    PrintFlush();
  *(curr->w_stringp)++ = c;
}

static void
PrintFlush()
{
  struct display *odisp = display;

  display = curr->w_pdisplay;
  if (display && printcmd)
    {
      char *bp = curr->w_string;
      int len = curr->w_stringp - curr->w_string;
      int r;
      while (len && display->d_printfd >= 0)
	{
	  r = write(display->d_printfd, bp, len);
	  if (r <= 0)
	    {
	      Msg(errno, "printing aborted");
	      close(display->d_printfd);
	      display->d_printfd = -1;
	      break;
	    }
	  bp += r;
	  len -= r;
	}
    }
  else if (display && curr->w_stringp > curr->w_string)
    {
      PutStr(D_PO);
      AddStrn(curr->w_string, curr->w_stringp - curr->w_string);
      PutStr(D_PF);
      Flush();
    }
  curr->w_stringp = curr->w_string;
  display = odisp;
}


void
NewAutoFlow(win, on)
struct win *win;
int on;
{
  debug1("NewAutoFlow: %d\n", on);
  SetCurr(win);
  if (win->w_flow & FLOW_AUTOFLAG)
    win->w_flow = FLOW_AUTOFLAG | (FLOW_AUTO|FLOW_NOW) * on;
  else
    win->w_flow = (win->w_flow & ~FLOW_AUTO) | FLOW_AUTO * on;
  if (display)
    SetFlow(win->w_flow & FLOW_NOW);
}

static void
DesignateCharset(c, n)
int c, n;
{
  curr->w_ss = 0;
#ifdef KANJI
  if (c == ('@' & 037))
    c = KANJI;
#endif
  if (c == 'B' || c == 'J')
    c = ASCII;
  if (curr->w_charsets[n] != c)
    {
      curr->w_charsets[n] = c;
      if (curr->w_Charset == n)
	SetFont(curr->w_Font = c);
      if (curr->w_CharsetR == n)
        curr->w_FontR = c;
    }
}

static void
MapCharset(n)
int n;
{
  curr->w_ss = 0;
  if (curr->w_Charset != n)
    {
      curr->w_Charset = n;
      SetFont(curr->w_Font = curr->w_charsets[n]);
    }
}

static void
MapCharsetR(n)
int n;
{
  curr->w_ss = 0;
  if (curr->w_CharsetR != n)
    {
      curr->w_CharsetR = n;
      curr->w_FontR = curr->w_charsets[n];
    }
  curr->w_gr = 1;
}

static void
SaveCursor()
{
  curr->w_saved = 1;
  curr->w_Saved_x = curr->w_x;
  curr->w_Saved_y = curr->w_y;
  curr->w_SavedAttr = curr->w_Attr;
  curr->w_SavedCharset = curr->w_Charset;
  curr->w_SavedCharsetR = curr->w_CharsetR;
  bcopy((char *) curr->w_charsets, (char *) curr->w_SavedCharsets,
	4 * sizeof(int));
}

static void
RestoreCursor()
{
  if (curr->w_saved)
    {
      GotoPos(curr->w_Saved_x, curr->w_Saved_y);
      curr->w_x = curr->w_Saved_x;
      curr->w_y = curr->w_Saved_y;
      curr->w_Attr = curr->w_SavedAttr;
      SetAttr(curr->w_Attr);
      bcopy((char *) curr->w_SavedCharsets, (char *) curr->w_charsets,
	    4 * sizeof(int));
      curr->w_Charset = curr->w_SavedCharset;
      curr->w_CharsetR = curr->w_SavedCharsetR;
      curr->w_ss = 0;
      SetFont(curr->w_Font = curr->w_charsets[curr->w_Charset]);
    }
}

static void
BackSpace()
{
  if (curr->w_x > 0)
    {
      curr->w_x--;
    }
  else if (curr->w_wrap && curr->w_y > 0)
    {
      curr->w_x = cols - 1;
      curr->w_y--;
    }
  if (display)
    GotoPos(curr->w_x, curr->w_y);
}

static void
Return()
{
  if (curr->w_x > 0)
    {
      curr->w_x = 0;
      if (display)
        GotoPos(curr->w_x, curr->w_y);
    }
}

static void
LineFeed(out_mode)
int out_mode;
{
  /* out_mode: 0=cr+lf no-output, 1=lf, 2=cr+lf */
  if (out_mode != 1)
    curr->w_x = 0;
  if (curr->w_y != curr->w_bot)		/* Don't scroll */
    {
      if (curr->w_y < rows-1)
	curr->w_y++;
      if (out_mode && display)
	GotoPos(curr->w_x, curr->w_y);
      return;
    }
  ScrollUpMap(1);
  if (curr->w_autoaka > 1)
    curr->w_autoaka--;
  if (out_mode && display)
    {
      ScrollV(0, curr->w_top, cols - 1, curr->w_bot, 1);
      GotoPos(curr->w_x, curr->w_y);
    }
}

static void
ReverseLineFeed()
{
  if (curr->w_y == curr->w_top)
    {
      ScrollDownMap(1);
      if (!display)
	return;
      ScrollV(0, curr->w_top, cols - 1, curr->w_bot, -1);
      GotoPos(curr->w_x, curr->w_y);
    }
  else if (curr->w_y > 0)
    CursorUp(1);
}

static void
InsertAChar(c, f)
int c, f;
{
  register int y = curr->w_y, x = curr->w_x;

  if (x == cols)
    x--;
  bcopy(curr->w_image[y], OldImage, cols);
  bcopy(curr->w_attr[y], OldAttr, cols);
  bcopy(curr->w_font[y], OldFont, cols);
  bcopy(curr->w_image[y] + x, curr->w_image[y] + x + 1, cols - x - 1);
  bcopy(curr->w_attr[y] + x, curr->w_attr[y] + x + 1, cols - x - 1);
  bcopy(curr->w_font[y] + x, curr->w_font[y] + x + 1, cols - x - 1);
  SetChar(c, f);
  curr->w_x = x + 1;
  if (!display)
    return;
  if (D_CIC || D_IC || D_IM)
    {
      InsertMode(curr->w_insert);
      INSERTCHAR(c);
      if (y == D_bot)
	D_lp_missing = 0;
    }
  else
    UpdateLine(OldImage, OldAttr, OldFont, y, x, cols - 1);
}

static void
InsertChar(n)
int n;
{
  register int y = curr->w_y, x = curr->w_x;

  if (n <= 0)
    return;
  /* Hack to be compatible with the old screen versions */
  if (curr->w_insert)
    return;
  if (x == cols)
    x--;
  bcopy(curr->w_image[y], OldImage, cols);
  bcopy(curr->w_attr[y], OldAttr, cols);
  bcopy(curr->w_font[y], OldFont, cols);
  if (n > cols - x)
    n = cols - x;
  bcopy(curr->w_image[y] + x, curr->w_image[y] + x + n, cols - x - n);
  bcopy(curr->w_attr[y] + x, curr->w_attr[y] + x + n, cols - x - n);
  bcopy(curr->w_font[y] + x, curr->w_font[y] + x + n, cols - x - n);
  ClearInLine(y, x, x + n - 1);
  if (!display)
    return;
  ScrollH(y, x, curr->w_width - 1, -n, OldImage, OldAttr, OldFont);
  GotoPos(x, y);
}

static void
DeleteChar(n)
int n;
{
  register int y = curr->w_y, x = curr->w_x;

  if (x == cols)
    x--;
  bcopy(curr->w_image[y], OldImage, cols);
  bcopy(curr->w_attr[y], OldAttr, cols);
  bcopy(curr->w_font[y], OldFont, cols);
  if (n > cols - x)
    n = cols - x;
  bcopy(curr->w_image[y] + x + n, curr->w_image[y] + x, cols - x - n);
  bcopy(curr->w_attr[y] + x + n, curr->w_attr[y] + x, cols - x - n);
  bcopy(curr->w_font[y] + x + n, curr->w_font[y] + x, cols - x - n);
  ClearInLine(y, cols - n, cols - 1);
  if (!display)
    return;
  ScrollH(y, x, curr->w_width - 1, n, OldImage, OldAttr, OldFont);
  GotoPos(x, y);
}

static void
DeleteLine(n)
int n;
{
  register int old = curr->w_top;
  
  if (curr->w_y < curr->w_top || curr->w_y > curr->w_bot)
    return;
  if (n > curr->w_bot - curr->w_y + 1)
    n = curr->w_bot - curr->w_y + 1;
  curr->w_top = curr->w_y;
  ScrollUpMap(n);
  curr->w_top = old;
  if (!display)
    return;
  ScrollV(0, curr->w_y, cols - 1, curr->w_bot, n);
  GotoPos(curr->w_x, curr->w_y);
}

static void
InsertLine(n)
int n;
{
  register int old = curr->w_top;

  if (curr->w_y < curr->w_top || curr->w_y > curr->w_bot)
    return;
  if (n > curr->w_bot - curr->w_y + 1)
    n = curr->w_bot - curr->w_y + 1;
  curr->w_top = curr->w_y;
  ScrollDownMap(n);
  curr->w_top = old;
  if (!display)
    return;
  ScrollV(0, curr->w_y, cols - 1, curr->w_bot, -n);
  GotoPos(curr->w_x, curr->w_y);
}

static void
ScrollRegion(n)
int n;
{
  if (n > 0)
    ScrollUpMap(n);
  else
    ScrollDownMap(-n);
  if (!display)
    return;
  ScrollV(0, curr->w_top, cols - 1, curr->w_bot, n);
  GotoPos(curr->w_x, curr->w_y);
}

static void
ScrollUpMap(n)
int n;
{
  char tmp[256 * sizeof(char *)];
  register int i, cnt1, cnt2;
  register char **ppi, **ppa, **ppf;
#ifdef COPY_PASTE
  register int ii;
#endif

  i = curr->w_top + n;
  cnt1 = n * sizeof(char *);
  cnt2 = (curr->w_bot - i + 1) * sizeof(char *);
  ppi = curr->w_image + i;
  ppa = curr->w_attr + i;
  ppf = curr->w_font + i;
#ifdef COPY_PASTE
  for(ii = curr->w_top; ii < i; ii++)
     AddLineToHist(curr, &curr->w_image[ii], &curr->w_attr[ii], &curr->w_font[ii]);
#endif
  for (i = n; i; --i)
    {
      bclear(*--ppi, cols + 1);
      bzero(*--ppa, cols + 1);
      bzero(*--ppf, cols + 1);
    }
  Scroll((char *) ppi, cnt1, cnt2, tmp);
  Scroll((char *) ppa, cnt1, cnt2, tmp);
  Scroll((char *) ppf, cnt1, cnt2, tmp);
}

static void
ScrollDownMap(n)
int n;
{
  char tmp[256 * sizeof(char *)];
  register int i, cnt1, cnt2;
  register char **ppi, **ppa, **ppf;

  i = curr->w_top;
  cnt1 = (curr->w_bot - i - n + 1) * sizeof(char *);
  cnt2 = n * sizeof(char *);
  Scroll((char *) (ppi = curr->w_image + i), cnt1, cnt2, tmp);
  Scroll((char *) (ppa = curr->w_attr + i), cnt1, cnt2, tmp);
  Scroll((char *) (ppf = curr->w_font + i), cnt1, cnt2, tmp);
  for (i = n; i; --i)
    {
      bclear(*ppi++, cols + 1);
      bzero(*ppa++, cols + 1);
      bzero(*ppf++, cols + 1);
    }
}

static void
Scroll(cp, cnt1, cnt2, tmp)
char *cp, *tmp;
int cnt1, cnt2;
{
  if (!cnt1 || !cnt2)
    return;
  if (cnt1 <= cnt2)
    {
      bcopy(cp, tmp, cnt1);
      bcopy(cp + cnt1, cp, cnt2);
      bcopy(tmp, cp + cnt2, cnt1);
    }
  else
    {
      bcopy(cp + cnt1, tmp, cnt2);
      bcopy(cp, cp + cnt2, cnt1);
      bcopy(tmp, cp, cnt2);
    }
}

static void
ForwardTab()
{
  register int x = curr->w_x;

  if (x == cols)
    {
      LineFeed(2);
      x = 0;
    }
  if (curr->w_tabs[x] && x < cols - 1)
    x++;
  while (x < cols - 1 && !curr->w_tabs[x])
    x++;
  GotoPos(x, curr->w_y);
  curr->w_x = x;
}

static void
BackwardTab()
{
  register int x = curr->w_x;

  if (curr->w_tabs[x] && x > 0)
    x--;
  while (x > 0 && !curr->w_tabs[x])
    x--;
  GotoPos(x, curr->w_y);
  curr->w_x = x;
}

static void
ClearScreen()
{
  register int i;
  register char **ppi = curr->w_image, **ppa = curr->w_attr, **ppf = curr->w_font;

  for (i = 0; i < rows; ++i)
    {
#ifdef COPY_PASTE
      AddLineToHist(curr, ppi, ppa, ppf);
#endif
      bclear(*ppi++, cols + 1);
      bzero(*ppa++, cols + 1);
      bzero(*ppf++, cols + 1);
    }
  if (display)
    ClearDisplay();
}

static void
ClearFromBOS()
{
  register int n, y = curr->w_y, x = curr->w_x;

  if (display)
    Clear(0, 0, 0, cols - 1, x, y, 1);
  for (n = 0; n < y; ++n)
    ClearInLine(n, 0, cols - 1);
  ClearInLine(y, 0, x);
  RestorePosAttrFont();
}

static void
ClearToEOS()
{
  register int n, y = curr->w_y, x = curr->w_x;

  if (x == 0 && y == 0)
    {
      ClearScreen();
      return;
    }
  if (display)
    Clear(x, y, 0, cols - 1, cols - 1, rows - 1, 1);
  ClearInLine(y, x, cols - 1);
  for (n = y + 1; n < rows; n++)
    ClearInLine(n, 0, cols - 1);
  RestorePosAttrFont();
}

static void
ClearFullLine()
{
  register int y = curr->w_y;

  if (display)
    Clear(0, y, 0, cols - 1, cols - 1, y, 1);
  ClearInLine(y, 0, cols - 1);
  RestorePosAttrFont();
}

static void
ClearToEOL()
{
  register int y = curr->w_y, x = curr->w_x;

  if (display)
    Clear(x, y, 0, cols - 1, cols - 1, y, 1);
  ClearInLine(y, x, cols - 1);
  RestorePosAttrFont();
}

static void
ClearFromBOL()
{
  register int y = curr->w_y, x = curr->w_x;

  if (display)
    Clear(0, y, 0, cols - 1, x, y, 1);
  ClearInLine(y, 0, x);
  RestorePosAttrFont();
}

static void
ClearInLine(y, x1, x2)
int y, x1, x2;
{
  register int n;

  if (x1 == cols)
    x1--;
  if (x2 == cols - 1)
    x2++;
  if ((n = x2 - x1 + 1) != 0)
    {
      bclear(curr->w_image[y] + x1, n);
      bzero(curr->w_attr[y] + x1, n);
      bzero(curr->w_font[y] + x1, n);
    }
}

static void
CursorRight(n)
register int n;
{
  register int x = curr->w_x;

  if (x == cols)
    {
      LineFeed(2);
      x = 0;
    }
  if ((curr->w_x += n) >= cols)
    curr->w_x = cols - 1;
  GotoPos(curr->w_x, curr->w_y);
}

static void
CursorUp(n)
register int n;
{
  if (curr->w_y < curr->w_top)		/* if above scrolling rgn, */
    {
      if ((curr->w_y -= n) < 0)		/* ignore its limits      */
         curr->w_y = 0;
    }
  else
    if ((curr->w_y -= n) < curr->w_top)
      curr->w_y = curr->w_top;
  GotoPos(curr->w_x, curr->w_y);
}

static void
CursorDown(n)
register int n;
{
  if (curr->w_y > curr->w_bot)		/* if below scrolling rgn, */
    {
      if ((curr->w_y += n) > rows - 1)	/* ignore its limits      */
        curr->w_y = rows - 1;
    }
  else
    if ((curr->w_y += n) > curr->w_bot)
      curr->w_y = curr->w_bot;
  GotoPos(curr->w_x, curr->w_y);
}

static void
CursorLeft(n)
register int n;
{
  if ((curr->w_x -= n) < 0)
    curr->w_x = 0;
  GotoPos(curr->w_x, curr->w_y);
}

static void
ASetMode(on)
int on;
{
  register int i;

  for (i = 0; i < curr->w_NumArgs; ++i)
    {
      switch (curr->w_args[i])
	{
	case 4:
	  curr->w_insert = on;
	  InsertMode(on);
	  break;
	case 20:
	  curr->w_autolf = on;
	  break;
	}
    }
}

static char rendlist[] =
{
  (1 << NATTR), A_BD, A_DI, A_SO, A_US, A_BL, 0, A_RV, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, ~(A_BD|A_SO|A_DI), ~A_SO, ~A_US, ~A_BL, 0, ~A_RV
};

static void
SelectRendition()
{
  register int j, i = 0, a = curr->w_Attr;

  do
    {
      j = curr->w_args[i];
      if (j < 0 || j >= (sizeof(rendlist)/sizeof(*rendlist)))
	continue;
      j = rendlist[j];
      if (j & (1 << NATTR))
        a &= j;
      else
        a |= j;
    }
  while (++i < curr->w_NumArgs);
  SetAttr(curr->w_Attr = a);
}

static void
FillWithEs()
{
  register int i;
  register char *p, *ep;

  curr->w_y = curr->w_x = 0;
  for (i = 0; i < rows; ++i)
    {
      bzero(curr->w_attr[i], cols);
      bzero(curr->w_font[i], cols);
      p = curr->w_image[i];
      ep = p + cols;
      while (p < ep)
	*p++ = 'E';
    }
  if (display)
    Redisplay(0);
}


static void
UpdateLine(os, oa, of, y, from, to)
int from, to, y;
char *os, *oa, *of;
{
  ASSERT(display);
  DisplayLine(os, oa, of, curr->w_image[y], curr->w_attr[y],
	      curr->w_font[y], y, from, to);
  RestorePosAttrFont();
}

void
CheckLP(n_ch)
char n_ch;
{
  register int y, x;
  register char n_at, n_fo, o_ch, o_at, o_fo;

  ASSERT(display);
  x = cols - 1;
  y = D_bot;
  o_ch = curr->w_image[y][x];
  o_at = curr->w_attr[y][x];
  o_fo = curr->w_font[y][x];

  n_at = curr->w_Attr;
  n_fo = curr->w_Font;

  D_lp_image = n_ch;
  D_lp_attr = n_at;
  D_lp_font = n_fo;
  D_lp_missing = 0;
  if (n_ch == o_ch && n_at == o_at && n_fo == o_fo)
    return;
  if (n_ch != ' ' || n_at || n_fo)
    D_lp_missing = 1;
  if (o_ch != ' ' || o_at || o_fo)
    {
      if (D_attr && D_UT)
	SetAttr(0);
      if (D_DC)
	PutStr(D_DC);
      else if (D_CDC)
	CPutStr(D_CDC, 1);
      else if (D_CE)
	PutStr(D_CE);
      else
	D_lp_missing = 1;
    }
}

/*
 *  Ugly autoaka hack support:
 *    ChangeAKA() sets a new aka
 *    FindAKA() searches for an autoaka match
 */

void
ChangeAKA(p, s, l)
struct win *p;
char *s;
int l;
{
  if (l > 20)
    l = 20;
  strncpy(p->w_akachange, s, l);
  p->w_akachange[l] = 0;
  p->w_title = p->w_akachange;
  if (p->w_akachange != p->w_akabuf)
    if (p->w_akachange[0] == 0 || p->w_akachange[-1] == ':')
      p->w_title = p->w_akabuf + strlen(p->w_akabuf) + 1;
}

static void
FindAKA()
{
  register char *cp, *line;
  register struct win *wp = curr;
  register int len = strlen(wp->w_akabuf);
  int y;

  y = (wp->w_autoaka > 0 && wp->w_autoaka <= wp->w_height) ? wp->w_autoaka - 1 : wp->w_y;
  cols = wp->w_width;
 try_line:
  cp = line = wp->w_image[y];
  if (wp->w_autoaka > 0 &&  *wp->w_akabuf != '\0')
    {
      for (;;)
	{
	  if (cp - line >= cols - len)
	    {
	      if (++y == wp->w_autoaka && y < rows)
		goto try_line;
	      return;
	    }
	  if (strncmp(cp, wp->w_akabuf, len) == 0)
	    break;
	  cp++;
	}
      cp += len;
    }
  for (len = cols - (cp - line); len && *cp == ' '; len--, cp++)
    ;
  if (len)
    {
      if (wp->w_autoaka > 0 && (*cp == '!' || *cp == '%' || *cp == '^'))
	wp->w_autoaka = -1;
      else
	wp->w_autoaka = 0;
      line = cp;
      while (len && *cp != ' ')
	{
	  if (*cp++ == '/')
	    line = cp;
	  len--;
	}
      ChangeAKA(wp, line, cp - line);
    }
  else
    wp->w_autoaka = 0;
}

void
MakeBlankLine(p, n)
register char *p;
register int n;
{
  while (n--)
    *p++ = ' ';
}

void
SetCurr(wp)
struct win *wp;
{
  curr = wp;
  if (curr == 0)
    return;
  cols = curr->w_width;
  rows = curr->w_height;
  display = curr->w_active ? curr->w_display : 0;
}

static void
RestorePosAttrFont()
{
  GotoPos(curr->w_x, curr->w_y);
  SetAttr(curr->w_Attr);
  SetFont(curr->w_Font);
}

/* Send a terminal report as if it were typed. */ 
static void
Report(fmt, n1, n2)
char *fmt;
int n1, n2;
{
  register int len;
  char rbuf[40];

  sprintf(rbuf, fmt, n1, n2);
  len = strlen(rbuf);

  if ((unsigned)(curr->w_inlen + len) <= sizeof(curr->w_inbuf))
    {
      bcopy(rbuf, curr->w_inbuf + curr->w_inlen, len);
      curr->w_inlen += len;
    }
}

#ifdef COPY_PASTE
void
AddLineToHist(wp, pi, pa, pf)
struct win *wp;
char **pi, **pa, **pf;
{
  register char *q, *o;

  if (wp->w_histheight == 0)
    return;
  q = *pi; *pi = wp->w_ihist[wp->w_histidx]; wp->w_ihist[wp->w_histidx] = q;
  q = *pa; o = wp->w_ahist[wp->w_histidx]; wp->w_ahist[wp->w_histidx] = q;
  if (o != null)
    free(o);
 
  q = *pf; o = wp->w_fhist[wp->w_histidx]; wp->w_fhist[wp->w_histidx] = q;
  if (o != null)
    free(o);
  *pa = *pf = null;
  if (++wp->w_histidx >= wp->w_histheight)
    wp->w_histidx = 0;
}
#endif

