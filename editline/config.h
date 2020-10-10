#ifndef CHATRA_EDITLINE_CONFIG_H
#define CHATRA_EDITLINE_CONFIG_H

#define __RCSID(a)

/* Define to 1 if you have the `getline' function. */
#define HAVE_GETLINE 1

/* Define to 1 if you have the `strlcat' function. */
#define HAVE_STRLCAT 1

/* Define to 1 if you have the `strlcpy' function. */
#define HAVE_STRLCPY 1

/* Define to 1 if you have the `issetugid' function. */
#define HAVE_ISSETUGID 1

/* Define to 1 if you have the `wcsdup' function. */
#define HAVE_WCSDUP 1

/* Define to 1 if the system has the type `u_int32_t'. */
#define HAVE_U_INT32_T 1

/* Define to 1 if you have the <termcap.h> header file. */
#define HAVE_TERMCAP_H 1

/* Define to 1 if you have the <curses.h> header file. */
#define HAVE_CURSES_H 1

/* Define to 1 if you have the <ncurses.h> header file. */
#define HAVE_NCURSES_H 1

/* Define to 1 if you have the <term.h> header file. */
#define HAVE_TERM_H 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the `vis' function. */
#define HAVE_VIS 1

#ifndef __OSX_AVAILABLE_STARTING
	#define __OSX_AVAILABLE_STARTING(_osx, _ios)
	#define __OSX_AVAILABLE_BUT_DEPRECATED(_osxIntro, _osxDep, _iosIntro, _iosDep)
	#define __OSX_AVAILABLE_BUT_DEPRECATED_MSG(_osxIntro, _osxDep, _iosIntro, _iosDep, _msg)
#endif

#endif // CHATRA_EDITLINE_CONFIG_H
