/*
 * Copyright (c) 2000-2001 Stephen Williams (steve@icarus.com)
 *
 *    This source code is free software; you can redistribute it
 *    and/or modify it in source code form under the terms of the GNU
 *    General Public License as published by the Free Software
 *    Foundation; either version 2 of the License, or (at your option)
 *    any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */
#ident "$Id: main.c,v 1.36 2002/04/24 02:02:31 steve Exp $"

# include "config.h"


const char HELP[] =
"Usage: iverilog [-ESvV] [-B base] [-C path] [-c cmdfile]\n"
"                [-D macro[=defn]] [-I includedir] [-M depfile] [-m module]\n"
"                [-N file] [-o filename] [-p flag=value]\n"
"                [-s topmodule] [-t target] [-T min|typ|max]\n"
"                [-W class] [-y dir] [-Y suf] source_file(s)\n"
"See man page for details.";

#define MAXSIZE 4096

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <sys/types.h>
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#ifdef __MINGW32__
#include <windows.h>
#endif

#if HAVE_GETOPT_H
#include <getopt.h>
#endif

#if defined(__MINGW32__) && !defined(HAVE_GETOPT_H)
extern int getopt(int argc, char*argv[], const char*fmt);
extern int optind;
extern const char*optarg;
#endif

#if !defined(WIFEXITED)
# define WIFEXITED(rc) ((rc&0x7f) == 0)
#endif

#if !defined(WEXITSTATUS)
# define WEXITSTATUS(rc) (rc>>8)
#endif

#ifndef IVL_ROOT
# define IVL_ROOT "."
#endif

#ifndef RDYNAMIC
# define RDYNAMIC "-rdynamic"
#endif

# include  "globals.h"

#ifdef __MINGW32__
const char sep = '\\';
#else
const char sep = '/';
#endif

extern void cfreset(FILE*fd, const char*path);

const char*base = 0;
const char*mtm  = 0;
const char*opath = "a.out" EXEEXT;
const char*npath = 0;
const char*targ  = "vvp";
const char*depfile = 0;

char warning_flags[16] = "";
char *library_flags = 0;
char *library_flags2 = 0;

char*inc_list = 0;
char*def_list = 0;
char*mod_list = 0;
char*command_filename = 0;
char*start = 0;

char*f_list = 0;

/* These are used to collect the list of file names that will be
   passed to ivlpp. Keep the list in a file because it can be a long
   list. */
char*source_path = 0;
FILE*source_file = 0;
unsigned source_count = 0;

int synth_flag = 0;
int verbose_flag = 0;
int command_file = 0;

FILE *fp;

char line[MAXSIZE];
char tmp[MAXSIZE];

static char ivl_root[MAXSIZE];

/*
 * This is the default target type. It looks up the bits that are
 * needed to run the command from the configuration file (which is
 * already parsed for us) so we can handle must of the generic cases.
 */
static int t_default(char*cmd, unsigned ncmd)
{
      int rc;
      const char*pattern;

      pattern = lookup_pattern("<ivl>");
      if (pattern == 0) {
	    fprintf(stderr, "No such target: %s\n", targ);
	    return -1;
      }

      tmp[0] = ' ';
      tmp[1] = '|';
      tmp[2] = ' ';
      rc = build_string(tmp+3, sizeof tmp - 3, pattern);
      cmd = realloc(cmd, ncmd+3+rc+1);
#ifdef __MINGW32__
      {
	char *t;
	for (t = tmp; *t; t++)
	  {
	    if (*t == '/') *t = '\\';
	  }
      }
#endif

      strcpy(cmd+ncmd, tmp);


      if (verbose_flag)
	    printf("translate: %s\n", cmd);


      rc = system(cmd);
      remove(source_path);
      if (rc != 0) {
	    if (rc == 127) {
		  fprintf(stderr, "Failed to execute: %s\n", cmd);
		  return 1;
	    }

	    if (WIFEXITED(rc))
		  return WEXITSTATUS(rc);

	    fprintf(stderr, "Command signaled: %s\n", cmd);
	    return -1;
      }

      return 0;
}

/*
 * This function handles the vvm target. After preprocessing, run the
 * ivl translator to get C++, then run g++ to make an executable
 * program out of that.
 */
static int t_vvm(char*cmd, unsigned ncmd)
{
      int rc;

      const char*pattern = lookup_pattern("<ivl>");
      if (pattern == 0) {
	    fprintf(stderr, "No such target: %s\n", targ);
	    return -1;
      }

      tmp[0] = ' ';
      tmp[1] = '|';
      tmp[2] = ' ';
      rc = build_string(tmp+3, sizeof tmp - 3, pattern);
      cmd = realloc(cmd, ncmd+3+rc+1);

#ifdef __MINGW32__
      {
	char *t;
	for (t = tmp; *t; t++)
	  {
	    if (*t == '/') *t = '\\';
	  }
      }
#endif


      strcpy(cmd+ncmd, tmp);


      if (verbose_flag)
	    printf("translate: %s\n", cmd);

      rc = system(cmd);
      remove(source_path);
      if (rc != 0) {
	    if (WIFEXITED(rc)) {
		  fprintf(stderr, "errors translating Verilog program.\n");
		  return WEXITSTATUS(rc);
	    } else {
		  fprintf(stderr, "Command signaled: %s\n", cmd);
		  return -1;
	    }
      }

#if defined(__MINGW32__)
	/* In the Windows world, the path to headers and libraries
	   for vvm is relative the ivl_root, which is in something like:

	       d:\iverilog\lib\ivl    <-- ivl_root
	       d:\iverilog\include    <-- vvm headers
	       d:\iverilog\lib        <-- vvm libraries

	   So we make up all the ugly references that are the g++
	   command line to compile a .cc file generated by vvm. */
      sprintf(tmp, "%s " RDYNAMIC " -s -fno-exceptions -o %s "
	      "-I%s\\..\\..\\include -L%s\\.. %s.cc -lvvm -lvpip %s",
	      CXX, opath, ivl_root, ivl_root, opath, DLLIB);
#else
	/* In the UNIX universe, life is easy. The configure script
	   has figured everything out and wrote it into the Makefile,
	   which passes it on as the IVL_INC and IVL_LIB defines. */
      sprintf(tmp, "%s " RDYNAMIC " -s -fno-exceptions -o %s -I%s "
	      "-L%s %s.cc -lvvm -lvpip %s", CXX, opath, IVL_INC, 
	      IVL_LIB, opath, DLLIB);
#endif
      if (verbose_flag)
	    printf("compile: %s\n", tmp);

      rc = system(tmp);
      if (rc != 0) {
	    if (WIFEXITED(rc)) {
		  fprintf(stderr, "errors compiling translated program.\n");
		  return WEXITSTATUS(rc);
	    } else {
		  fprintf(stderr, "Command signaled: %s\n", tmp);
		  return -1;
	    }
      }

      sprintf(tmp, "%s.cc", opath);
      unlink(tmp);

      return 0;
}


static void process_warning_switch(const char*name)
{
      if (warning_flags[0] == 0)
	    strcpy(warning_flags, "-W");

      if (strcmp(name,"all") == 0) {
	    strcat(warning_flags, "it");

      } else if (strcmp(name,"implicit") == 0) {
	    if (! strchr(warning_flags+2, 'i'))
		  strcat(warning_flags, "i");
      } else if (strcmp(name,"timescale") == 0) {
	    if (! strchr(warning_flags+2, 't'))
		  strcat(warning_flags, "t");
      } else if (strcmp(name,"no-implicit") == 0) {
	    char*cp = strchr(warning_flags+2, 'i');
	    if (cp) while (*cp) {
		  cp[0] = cp[1];
		  cp += 1;
	    }
      } else if (strcmp(name,"no-timescale") == 0) {
	    char*cp = strchr(warning_flags+2, 't');
	    if (cp) while (*cp) {
		  cp[0] = cp[1];
		  cp += 1;
	    }
      }
}

void process_library_switch(const char *name)
{
      if (library_flags) {
	    library_flags = realloc(library_flags, 
				    strlen(library_flags) + strlen(name) + 5);
	    strcat(library_flags, " -y ");
      } else {
	    library_flags = malloc(strlen(name) + 4);
	    strcpy(library_flags, "-y ");
      }
      strcat(library_flags, name);
}

void process_library2_switch(const char *name)
{
      if (library_flags2) {
	    library_flags2 = realloc(library_flags2, 
				    strlen(library_flags2) + strlen(name) + 5);
	    strcat(library_flags2, " -Y ");
      } else {
	    library_flags2 = malloc(strlen(name) + 4);
	    strcpy(library_flags2, "-Y ");
      }
      strcat(library_flags2, name);
}

void process_include_dir(const char *name)
{
      if (inc_list == 0) {
	    inc_list = malloc(strlen(" -I")+strlen(name)+1);
	    strcpy(inc_list, " -I");
	    strcat(inc_list, name);
      } else {
	    inc_list = realloc(inc_list, strlen(inc_list)
			       + strlen(" -I")
			       + strlen(name) + 1);
	    strcat(inc_list, " -I");
	    strcat(inc_list, name);
      }
}

void process_define(const char*name)
{
      if (def_list == 0) {
	    def_list = malloc(strlen(" -D")+strlen(name)+1);
	    strcpy(def_list, " -D");
	    strcat(def_list, name);
      } else {
	    def_list = realloc(def_list, strlen(def_list)
			       + strlen(" -D")
			       + strlen(name) + 1);
	    strcat(def_list, " -D");
	    strcat(def_list, name);
      }
}

void process_file_name(const char*name)
{
      fprintf(source_file, "%s\n", name);
      source_count += 1;
}

int main(int argc, char **argv)
{
      const char*config_path = 0;
      char*cmd;
      unsigned ncmd;
      int e_flag = 0;
      int version_flag = 0;
      int opt, idx;
      char*cp;

#ifdef __MINGW32__
      { char * s;
	char basepath[1024];
	GetModuleFileName(NULL,basepath,1024);

	  /* Calculate the ivl_root from the path to the command. This
	     is necessary because of the installation process in
	     Windows. Mostly, it is those darn drive letters, but oh
	     well. We know the command path is formed like this:

	         D:\iverilog\bin\iverilog.exe

	     The IVL_ROOT in a Windows installation is the path:

	         D:\iverilog\lib\ivl

	     so we chop the file name and the last directory by
	     turning the last two \ characters to null. Then we append
	     the lib\ivl to finish. */

        strncpy(ivl_root, basepath, MAXSIZE);
	s = strrchr(ivl_root, sep);
	if (s) *s = 0;
	s = strrchr(ivl_root, sep);
	if (s) *s = 0;
	strcat(ivl_root, "\\lib\\ivl");

	base = ivl_root;
      }
#else
        /* In a UNIX environment, the IVL_ROOT from the Makefile is
	   dependable. It points to the $prefix/lib/ivl directory,
	   where the sub-parts are installed. */
      strcpy(ivl_root, IVL_ROOT);
      base = ivl_root;
#endif

      source_path = strdup(tempnam(NULL, "ivrlg"));
      assert(source_path);
      source_file = fopen(source_path, "w");
      if (NULL == source_file) {
	    fprintf(stderr, "%s: Error opening temporary file %s\n",
		    argv[0], source_path);
	    fprintf(stderr, "%s: Please check TMP or TMPDIR.\n", argv[0]);
	    return 1;
      }

      while ((opt = getopt(argc, argv, "B:C:c:D:Ef:hI:M:m:N::o:p:Ss:T:t:vVW:y:Y:")) != EOF) {

	    switch (opt) {
		case 'B':
		  base = optarg;
		  break;
		case 'C':
		  config_path = optarg;
		  break;
 		case 'c':
		  command_filename = malloc(strlen(optarg)+1);
 		  strcpy(command_filename, optarg);
 		  break;
		case 'D':
		  process_define(optarg);
		  break;
		case 'E':
		  e_flag = 1;
		  break;
		case 'f':
		  fprintf(stderr, "warning: The -f flag is moved to -p\n");
		case 'p':
		  if (f_list == 0) {
			f_list = malloc(strlen(" -p")+strlen(optarg)+1);
			strcpy(f_list, " -p");
			strcat(f_list, optarg);
		  } else {
			f_list = realloc(f_list, strlen(f_list) +
					 strlen(" -p") +
					 strlen(optarg) + 1);
			strcat(f_list, " -p");
			strcat(f_list, optarg);
		  }
		  break;

 		case 'h':
 		  fprintf(stderr, "%s\n", HELP);
 		  return 1;

		case 'I':
		  process_include_dir(optarg);
		  break;

		case 'M':
		  depfile = optarg;
		  break;

		case 'm':
		  if (mod_list == 0) {
			mod_list = malloc(strlen(" -m")+strlen(optarg)+1);
			strcpy(mod_list, " -m");
			strcat(mod_list, optarg);
		  } else {
			mod_list = realloc(mod_list, strlen(mod_list)
					   + strlen(" -m")
					   + strlen(optarg) + 1);
			strcat(mod_list, " -m");
			strcat(mod_list, optarg);
		  }
		  break;

		case 'N':
		  npath = optarg;
		  break;

		case 'o':
		  opath = optarg;
		  break;

		case 'S':
		  synth_flag = 1;
		  break;
		case 's':
	          if (start) {
			static const char *s = " -s ";
			size_t l = strlen(start);
			start = realloc(start, l + strlen(optarg) + strlen(s) + 1);
			strcpy(start + l, s);
			strcpy(start + l + strlen(s), optarg);
		  } else {
			static const char *s = "-s ";
			start = malloc(strlen(optarg) + strlen(s) + 1);
			strcpy(start, s);
			strcpy(start + strlen(s), optarg);
		  }
		  break;
		case 'T':
		  if (strcmp(optarg,"min") == 0) {
			mtm = "min";
		  } else if (strcmp(optarg,"typ") == 0) {
			mtm = "typ";
		  } else if (strcmp(optarg,"max") == 0) {
			mtm = "max";
		  } else {
			fprintf(stderr, "%s: invalid -T%s argument\n",
				argv[0], optarg);
			return 1;
		  }
		  break;
		case 't':
		  targ = optarg;
		  break;
		case 'v':
		  verbose_flag = 1;
		  break;
		case 'V':
		  version_flag = 1;
		  break;
		case 'W':
		  process_warning_switch(optarg);
		  break;
		case 'y':
		  process_library_switch(optarg);
		  break;
		case 'Y':
		  process_library2_switch(optarg);
		  break;
		case '?':
		default:
		  return 1;
	    }
      }

      if (version_flag || verbose_flag) {
	    printf("Icarus Verilog version " VERSION "\n");
	    printf("Copyright 1998-2002 Stephen Williams\n");
	    printf("$Name:  $\n");

	    if (version_flag)
		  return 0;
      }

      if (command_filename) {
	    int rc;

	    if (( fp = fopen(command_filename, "r")) == NULL ) {
		  fprintf(stderr, "%s: Can't open %s\n",
			  argv[0], command_filename);
		  return 1;
	    }

	    cfreset(fp, command_filename);
	    rc = cfparse();
	    if (rc != 0) {
		  fprintf(stderr, "%s: error reading command file\n",
			  command_filename);
		  return 1;
	    }
      }

	/* Finally, process all the remaining words on the command
	   line as file names. */
      for (idx = optind ;  idx < argc ;  idx += 1)
	    process_file_name(argv[idx]);


      fclose(source_file);
      source_file = 0;

      if (source_count == 0) {
	    fprintf(stderr, "%s: No input files.\n", argv[0]);
 	    fprintf(stderr, "%s\n", HELP);
	    return 1;
      }

	/* Load the iverilog.conf file to get our substitution
	   strings. */

      { char path[1024];
        FILE*fd;
	if (config_path) {
	      strcpy(path, config_path);
	} else {
	      sprintf(path, "%s%civerilog.conf", base,sep);
	}
	fd = fopen(path, "r");
	if (fd == 0) {
	      fprintf(stderr, "Config file \"%s\" not found\n",path);
	      return 1;
	}
	reset_lexor(fd);
	yyparse();
      }

	/* Start building the preprocess command line. */

      sprintf(tmp, "%s%civlpp %s%s -f%s ", base,sep,
	      verbose_flag?" -v":"",
	      e_flag?"":" -L", source_path);

      ncmd = strlen(tmp);
      cmd = malloc(ncmd + 1);
      strcpy(cmd, tmp);

      if (inc_list) {
	    cmd = realloc(cmd, ncmd + strlen(inc_list) + 1);
	    strcat(cmd, inc_list);
	    ncmd += strlen(inc_list);
      }

      if (def_list) {
	    cmd = realloc(cmd, ncmd + strlen(def_list) + 1);
	    strcat(cmd, def_list);
	    ncmd += strlen(def_list);
      }

      if (depfile) {
	    cmd = realloc(cmd, ncmd + strlen(depfile) + 5);
	    strcat(cmd, " -M ");
	    strcat(cmd, depfile);
	    ncmd += strlen(depfile) + 4;
      }

	/* If the -E flag was given on the command line, then all we
	   do is run the preprocessor and put the output where the
	   user wants it. */
      if (e_flag) {
	    int rc;
	    if (strcmp(opath,"-") != 0) {
		  sprintf(tmp, " > %s", opath);
		  cmd = realloc(cmd, ncmd+strlen(tmp)+1);
		  strcpy(cmd+ncmd, tmp);
		  ncmd += strlen(tmp);
	    }

	    if (verbose_flag)
		  printf("preprocess: %s\n", cmd);

	    rc = system(cmd);
	    remove(source_path);
	    if (rc != 0) {
		  if (WIFEXITED(rc)) {
			fprintf(stderr, "errors preprocessing Verilog program.\n");
			return WEXITSTATUS(rc);
		  }

		  fprintf(stderr, "Command signaled: %s\n", cmd);
		  return -1;
	    }

	    return 0;
      }

      if (strcmp(targ,"vvm") == 0)
	    return t_vvm(cmd, ncmd);
      else {
	    return t_default(cmd, ncmd);
      }

      return 0;
}

/*
 * $Log: main.c,v $
 * Revision 1.36  2002/04/24 02:02:31  steve
 *  add -Wno- arguments to the driver.
 *
 * Revision 1.35  2002/04/15 00:04:23  steve
 *  Timescale warnings.
 *
 * Revision 1.34  2002/04/04 05:26:13  steve
 *  Add dependency generation.
 *
 * Revision 1.33  2002/03/15 23:27:42  steve
 *  Patch to allow user to set place for temporary files.
 *
 * Revision 1.32  2002/02/03 07:05:37  steve
 *  Support print of version number.
 *
 * Revision 1.31  2001/11/21 02:20:34  steve
 *  Pass list of file to ivlpp via temporary file.
 *
 * Revision 1.30  2001/11/16 05:07:19  steve
 *  Add support for +libext+ in command files.
 *
 * Revision 1.29  2001/11/13 03:30:26  steve
 *  The +incdir+ plusarg can take multiple directores,
 *  and add initial support for +define+ in the command file.
 *
 * Revision 1.28  2001/11/12 18:47:32  steve
 *  Support +incdir in command files, and ignore other
 *  +args flags. Also ignore -a and -v flags.
 *
 * Revision 1.27  2001/11/12 01:26:36  steve
 *  More sophisticated command file parser.
 *
 * Revision 1.26  2001/11/11 00:10:05  steve
 *  Remov XNF dead wood.
 *
 * Revision 1.25  2001/10/23 00:37:30  steve
 *  The -s flag can now be repeated on the iverilog command.
 *
 * Revision 1.24  2001/10/20 23:02:40  steve
 *  Add automatic module libraries.
 *
 * Revision 1.23  2001/10/19 23:10:08  steve
 *  Fix memory fault with -c flag.
 *
 * Revision 1.22  2001/10/11 00:12:49  steve
 *  Detect execv failures.
 *
 * Revision 1.21  2001/07/25 03:10:50  steve
 *  Create a config.h.in file to hold all the config
 *  junk, and support gcc 3.0. (Stephan Boettcher)
 *
 * Revision 1.20  2001/06/30 21:53:42  steve
 *  Make the vvp target the default.
 *
 * Revision 1.19  2001/06/30 04:23:02  steve
 *  Get include and lib paths right for mingw and vvm.
 *
 * Revision 1.18  2001/06/30 00:59:24  steve
 *  Redo the ivl_root calculator for mingw.
 *
 * Revision 1.17  2001/06/27 02:22:26  steve
 *  Get include and lib paths from Makefile.
 *
 * Revision 1.16  2001/06/20 02:25:40  steve
 *  Edit ivl_install_dir only on mingw
 *
 * Revision 1.15  2001/06/15 05:14:21  steve
 *  Fix library path calculation on non Windows systems
 *  to include the install directories. (Brendan Simon)
 *
 * Revision 1.14  2001/06/12 03:53:10  steve
 *  Change the VPI call process so that loaded .vpi modules
 *  use a function table instead of implicit binding.
 *
 * Revision 1.13  2001/05/20 18:22:02  steve
 *  Fix WIFEXITED macro.
 *
 * Revision 1.12  2001/05/20 18:06:57  steve
 *  local declares if the header is missing.
 *
 * Revision 1.11  2001/05/20 15:09:40  steve
 *  Mingw32 support (Venkat Iyer)
 *
 * Revision 1.10  2001/05/17 03:14:26  steve
 *  Update help message.
 *
 * Revision 1.9  2001/04/26 16:04:39  steve
 *  Handle missing or uninstalled .conf files.
 *
 * Revision 1.8  2001/02/01 17:12:22  steve
 *  Forgot to actually allow the -p flag.
 *
 * Revision 1.7  2001/01/20 19:02:05  steve
 *  Switch hte -f flag to the -p flag.
 */

