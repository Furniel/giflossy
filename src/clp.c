/* clp.c - Complete source code for CLP.
   Copyright (C) 1997 Eddie Kohler, eddietwo@lcs.mit.edu
   This file is part of CLP, the command line parser package.

   CLP is free software. It is distributed under the GNU Public License,
   version 2 or later; you can copy, distribute, or alter it at will, as
   long as this notice is kept intact and this source code is made available.
   There is no warranty, express or implied. */

#include "clp.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Option types for Clp_SetOptionChar */
#define Clp_DoubledLong		(Clp_LongImplicit * 2)

#define Clp_AnyArgument (Clp_Mandatory | Clp_Optional)


typedef struct {
  Clp_ArgParseFunc func;
  void *data;
} Clp_ArgType;
  

struct Clp_Internal {
  
  Clp_Option *opt;
  int nopt;
  
  Clp_ArgType *argtype;
  int nargtype;
  
  char **argv;
  int argc;
  
  unsigned char option_class[256];
  
  char option_chars[3];
  char *text;
  
  char *program_name;
  void (*error_hook)(void);
  
  int is_short;
  int whole_negated;
  int could_be_short;
  
  int option_processing;
  
  int ambiguous;
  
  Clp_Option *current_option;
  int current_short;
  int negated_by_no;
  
};


struct Clp_ParserState {
  
  char **argv;
  int argc;
  char option_chars[3];
  char *text;
  int is_short;
  int whole_negated;
  
};


typedef struct {
  char *name;
  int min_match;
  int value;
} Clp_StringListItem;

typedef struct {
  int allow_int;
  int nitems;
  Clp_StringListItem *items;
} Clp_StringList;


#define TEST(o, f)		(((o)->flags & (f)) != 0)


static int parse_string(Clp_Parser *, const char *, void *, int);
static int parse_int(Clp_Parser *, const char *, void *, int);
static int parse_bool(Clp_Parser *, const char *, void *, int);
static int parse_double(Clp_Parser *, const char *, void *, int);


/*******
 * Clp_NewParser, etc.
 **/

static int
min_different_chars(char *s, char *t)
     /* Returns the minimum number of characters required to distinguish
	s from t.
	If s is shorter than t, returns strlen(s). */
{
  char *sfirst = s;
  while (*s && *t && *s == *t)
    s++, t++;
  if (!*s)
    return s - sfirst;
  else
    return s - sfirst + 1;
}


Clp_Parser *
Clp_NewParser(int argc, char **argv, int nopt, Clp_Option *opt)
     /* Creates and returns a new Clp_Parser using the options in `opt',
	or 0 on memory allocation failure */
{
  int i, j;
  Clp_Parser *clp = (Clp_Parser *)malloc(sizeof(Clp_Parser));
  Clp_Internal *cli = (Clp_Internal *)malloc(sizeof(Clp_Internal));
  if (!clp || !cli) goto failed;
  
  clp->internal = cli;
  
  /* Massage the options to make them usable */
  for (i = 0; i < nopt; i++) {
    if (opt[i].long_min_match <= 0 || !opt[i].long_name)
      opt[i].flags &= ~Clp_LongMinMatch;
    
    if (opt[i].arg_type <= 0)
      opt[i].flags &= ~Clp_AnyArgument;
    if (opt[i].arg_type > 0 && !TEST(&opt[i], Clp_Optional))
      opt[i].flags |= Clp_Mandatory;
    
    /* Nonexistent short options have character 257. We know this won't
       equal any character in an argument, even if characters are signed */
    if (opt[i].short_name <= 0)
      opt[i].short_name = 257;
  }

  /* Calculate long options' minimum unambiguous length */
  for (i = 0; i < nopt; i++)
    if (opt[i].long_name && !TEST(&opt[i], Clp_LongMinMatch)) {
      int len = 1;
      
      for (j = 0; j < nopt; j++)
	if (opt[j].long_name
	    && i != j
	    && strncmp(opt[i].long_name, opt[j].long_name, len) == 0)
	  len = min_different_chars(opt[i].long_name, opt[j].long_name);
      
      opt[i].long_min_match = len;
    }

  /* Set up clp->internal */
  cli->opt = opt;
  cli->nopt = nopt;
  
  cli->argtype = (Clp_ArgType *)malloc(sizeof(Clp_ArgType) * 5);
  if (!cli->argtype) goto failed;
  cli->nargtype = 5;
  
  cli->argc = argc;
  cli->argv = argv;
  {
    char *slash = strrchr(argv[0], '/');
    cli->program_name = slash ? slash + 1 : argv[0];
  }
  cli->error_hook = 0;
  
  for (i = 0; i < 256; i++)
    cli->option_class[i] = 0;
  cli->option_class['-'] = Clp_Short;
  
  cli->is_short = 0;
  cli->whole_negated = 0;
  
  cli->option_processing = 1;
  cli->current_option = 0;
  
  /* Add default type parsers */
  Clp_AddType(clp, Clp_ArgString, parse_string, 0);
  Clp_AddType(clp, Clp_ArgInt, parse_int, 0);
  Clp_AddType(clp, Clp_ArgBool, parse_bool, 0);
  Clp_AddType(clp, Clp_ArgDouble, parse_double, 0);
  return clp;
  
 failed:
  if (cli && cli->argtype) free(cli->argtype);
  if (cli) free(cli);
  if (clp) free(clp);
  return 0;
}


void
Clp_SetOptionProcessing(Clp_Parser *clp, int option_processing)
     /* Sets whether command line arguments are parsed
	(looking for options) at all.
	Each parser starts out with OptionProcessing true. */
{
  Clp_Internal *cli = clp->internal;
  cli->option_processing = option_processing;
}


void
Clp_SetErrorHook(Clp_Parser *clp, void (*error_hook)(void))
     /* Sets a hook function to be called before Clp_OptionError
	prints anything. 0 means nothing will be called. */
{
  Clp_Internal *cli = clp->internal;
  cli->error_hook = error_hook;
}


void
Clp_SetOptionChar(Clp_Parser *clp, int c, int option_type)
     /* Determines how clp will deal with short options.
	option_type must be a sum of:

	0 == Clp_NotOption	`character' isn't an option.
	Clp_Short	`character' introduces a list of short options.
	Clp_Long	`character' introduces a long option.
	Clp_ShortNegated `character' introduces a negated list of
			short options.
	Clp_LongNegated `character' introduces a negated long option.
	Clp_LongImplicit `character' introduces a long option, and *is part*
			of the long option itself.

	Some values are not allowed (Clp_Long | Clp_LongNegated isn't allowed,
	for instance).
	c=0 means ALL characters are that type. */
{
  int i;
  Clp_Internal *cli = clp->internal;
  
  assert(option_type >= 0 && option_type < 2*Clp_LongImplicit);
  if (option_type & Clp_Short) assert(!(option_type & Clp_ShortNegated));
  if (option_type & Clp_Long) assert(!(option_type & Clp_LongNegated));
  
  if (c == 0)
    for (i = 1; i < 256; i++)
      cli->option_class[i] = option_type;
  else
    cli->option_class[c] = option_type;
}


/*****
 * Argument parsing
 **/

int
Clp_AddType(Clp_Parser *clp, int type_id, Clp_ArgParseFunc func, void *data)
     /* Add a argument parser for type_id to the Clp_Parser. When an argument
	arg for Clp_Option opt is being processed, the parser routines will
	call (*func)(clp, opt, arg, data, complain)
	where complain is 1 if the routine should report errors, or 0 if it
	should fail silently.
	Returns 1 on success, 0 if memory allocation fails or the arguments
	are bad. */
{
  int i;
  Clp_Internal *cli = clp->internal;
  int nargtype = cli->nargtype;
  assert(nargtype);
  
  if (type_id <= 0 || !func) return 0;
  
  while (nargtype <= type_id) {
    nargtype *= 2;
    cli->argtype = (Clp_ArgType *)
      realloc(cli->argtype, sizeof(Clp_ArgType) * nargtype);
    if (!cli->argtype) return 0;
  }
  
  for (i = cli->nargtype; i < nargtype; i++)
    cli->argtype[i].func = 0;
  cli->nargtype = nargtype;
  
  cli->argtype[type_id].func = func;
  cli->argtype[type_id].data = data;
  return 1;
}



/*******
 * Default argument parsers
 **/

static int
parse_string(Clp_Parser *clp, const char *arg, void *v, int complain)
{
  clp->val.s = (char *)arg;
  return 1;
}


static int
parse_int(Clp_Parser *clp, const char *arg, void *v, int complain)
{
  char *val;
  clp->val.i = strtol(arg, &val, 10);
  if (*arg != 0 && *val == 0)
    return 1;
  else if (complain)
    return Clp_OptionError(clp, "`%O's argument must be an integer");
  else
    return 0;
}


static int
parse_double(Clp_Parser *clp, const char *arg, void *v, int complain)
{
  char *val;
  clp->val.d = strtod(arg, &val);
  if (*arg != 0 && *val == 0)
    return 1;
  else if (complain)
    return Clp_OptionError(clp, "`%O's argument must be a real number");
  else
    return 0;
}


static int
parse_bool(Clp_Parser *clp, const char *arg, void *v, int complain)
{
  if (arg[0] == 'y' || arg[0] == 't' || strcmp(arg, "1") == 0) {
    clp->val.i = 1;
    return 1;
  } else if (arg[0] == 'n' || arg[0] == 'f' || strcmp(arg, "0") == 0) {
    clp->val.i = 0;
    return 1;
  } else if (complain)
    return Clp_OptionError(clp, "`%O's argument must be yes or no");
  else
    return 0;
}


/*****
 * the ever-glorious argcmp
 **/

static int
argcmp(const char *ref, const char *arg, int min_match)
     /* Returns 0 if ref and arg don't match.
	Returns -1 if ref and arg match, but fewer than min_match characters.
	Returns len if ref and arg match min_match or more characters;
	len is the number of charcters that matched.

	Examples:
	argcmp("x", "y", 1)	-->  0	/ just plain wrong
	argcmp("a", "ax", 1)	-->  0	/ ...even though min_match == 1
					and the 1st chars match
	argcmp("box", "bo", 3)	--> -1	/ ambiguous
	argcmp("cat", "c=3", 1)	-->  1	/ handles = arguments
	*/
{
  const char *refstart = ref;
  while (*ref && *arg && *arg != '=' && *ref == *arg)
    ref++, arg++;
  if (*arg && *arg != '=')
    return 0;
  else if (ref - refstart < min_match)
    return -1;
  else
    return ref - refstart;
}


/*****
 * Clp_AddStringListType
 **/

static int
parse_string_list(Clp_Parser *clp, const char *arg, void *v, int complain)
{
  Clp_StringList *sl = (Clp_StringList *)v;
  int i;
  int ambiguous = 0;
  
  for (i = 0; i < sl->nitems; i++) {
    int len = argcmp( sl->items[i].name, arg, sl->items[i].min_match );
    if (len > 0) {
      clp->val.i = sl->items[i].value;
      return 1;
    } else if (len < 0)
      ambiguous = 1;
  }
  
  if (sl->allow_int) {
    char *x;
    clp->val.i = strtol(arg, &x, 10);
    if (*arg != 0 && *x == 0)
      return 1;
  }
  
  if (complain && ambiguous)
    return Clp_OptionError
      (clp, "`%s' is ambiguous as an argument for `%O'", arg);
  else if (complain)
    return Clp_OptionError
      (clp, "unrecognized argument `%s' to `%O'", arg);
  else
    return 0;
}


int
finish_string_list(Clp_Parser *clp, int type_id, int flags,
		   Clp_StringListItem *items, int nitems)
{
  int i;
  Clp_StringList *clsl = (Clp_StringList *)malloc(sizeof(Clp_StringList));
  if (!clsl) return 0;
  
  clsl->allow_int = (flags & Clp_AllowNumbers) != 0;
  clsl->items = items;
  clsl->nitems = nitems;
  
  /* calculate min_match. If two strings have the same value,
     allow users to give their common prefix
     (instead of forcing them to distinguish unambiguously between the
     two essentially equivalent strings). */
  for (i = 0; i < nitems; i++) {
    int len = 1;
    int j;
    for (j = 0; j < nitems; j++)
      if (items[i].value != items[j].value
	  && strncmp(items[i].name, items[j].name, len) == 0)
	len = min_different_chars(items[i].name, items[j].name);
    items[i].min_match = len;
  }
  
  if (Clp_AddType(clp, type_id, parse_string_list, clsl))
    return 1;
  else {
    free(clsl);
    return 0;
  }
}


int
Clp_AddStringListType(Clp_Parser *clp, int type_id, int flags, ...)
     /* An easy way to add new types to clp.
	Call like this:
	Clp_AddStringListType
	  (clp, type_id, flags,
	   char *s_1, int value_1, ..., char *s_n, int value_n, 0);

        Defines type_id as a type in clp.
	This type accepts any of the strings s_1, ..., s_n
	(or unambiguous abbreviations thereof);
	if argument s_i is given, value_i is stored in clp->val.i.
	If Clp_AllowNumbers is set in flags,
	explicit integers are also allowed.

	Returns 1 on success, 0 on memory allocation errors. */
{
  int nitems = 0;
  int itemscap = 5;
  Clp_StringListItem *items = (Clp_StringListItem *)
    malloc(sizeof(Clp_StringListItem) * itemscap);
  
  va_list val;
  va_start(val, flags);
  
  if (!items) goto error;
  
  /* slurp up the arguments */
  while (1) {
    int value;
    char *name = va_arg(val, char *);
    if (!name) break;
    value = va_arg(val, int);
    
    if (nitems >= itemscap) {
      itemscap *= 2;
      items = (Clp_StringListItem *)
	realloc(items, sizeof(Clp_StringListItem) * itemscap);
      if (!items) goto error;
    }
    
    items[nitems].name = name;
    items[nitems].value = value;
    nitems++;
  }

  if (finish_string_list(clp, type_id, flags, items, nitems))
    return 1;
  
 error:
  va_end(val);
  if (items) free(items);
  return 0;
}


int
Clp_AddStringListTypeVec(Clp_Parser *clp, int type_id, int flags,
			 int nitems, char **strings, int *values)
     /* An alternate way to make a string list type. See Clp_AddStringListType
	for the basics; this coalesces the strings and values into two arrays,
	rather than spreading them out into a variable argument list. */
{
  int i;
  Clp_StringListItem *items = (Clp_StringListItem *)
    malloc(sizeof(Clp_StringListItem) * nitems);
  if (!items) return 0;
  
  /* copy over items */
  for (i = 0; i < nitems; i++) {
    items[i].name = strings[i];
    items[i].value = values[i];
  }
  
  if (finish_string_list(clp, type_id, flags, items, nitems))
    return 1;
  else {
    free(items);
    return 0;
  }
}


/*******
 * Returning information
 **/

const char *
Clp_ProgramName(Clp_Parser *clp)
{
  return clp->internal->program_name;
}


/******
 * Clp_ParserStates
 **/

Clp_ParserState *
Clp_NewParserState(void)
{
  return (Clp_ParserState *)malloc(sizeof(Clp_ParserState));
}

void
Clp_DeleteParserState(Clp_ParserState *save)
{
  free(save);
}


void
Clp_SaveParser(Clp_Parser *clp, Clp_ParserState *save)
     /* Saves parser position in save */
{
  Clp_Internal *cli = clp->internal;
  save->argv = cli->argv;
  save->argc = cli->argc;
  memcpy(save->option_chars, cli->option_chars, 3);
  save->text = cli->text;
  save->is_short = cli->is_short;
  save->whole_negated = cli->whole_negated;
}


void
Clp_RestoreParser(Clp_Parser *clp, Clp_ParserState *save)
     /* Restores parser position from save */
{
  Clp_Internal *cli = clp->internal;
  cli->argv = save->argv;
  cli->argc = save->argc;
  memcpy(cli->option_chars, save->option_chars, 3);
  cli->text = save->text;
  cli->is_short = save->is_short;
  cli->whole_negated = save->whole_negated;
}


/*******
 * Clp_Next and its helpers
 **/

static void
set_option_text(Clp_Internal *cli, char *text, int n_option_chars)
{
  char *option_chars = cli->option_chars;
  assert(n_option_chars < 3);
  
  while (n_option_chars-- > 0)
    *option_chars++ = *text++;
  *option_chars = 0;
  
  cli->text = text;
}


static int
next_argument(Clp_Parser *clp, int want_argument)
     /* Moves clp to the next argument.
	Returns 1 if it finds another option.
	Returns 0 if there aren't any more arguments.
	Returns 0, sets clp->hadarg = 1, and sets clp->arg to the argument
	if the next argument isn't an option.
	If want_argument > 0, it'll look for an argument.
	want_argument == 1: Accept arguments that start with Clp_NotOption
		or Clp_LongImplicit.
	want_argument == 2: Accept ALL arguments.
	
	Where is the option stored when this returns?
	Well, cli->argv[0] holds the whole of the next command line argument.
	cli->option_chars holds a string: what characters began the option?
	It is generally "-" or "--".
	cli->text holds the text of the option:
	for short options, cli->text[0] is the relevant character;
	for long options, cli->text holds the rest of the option. */
{
  Clp_Internal *cli = clp->internal;
  char *text;
  int option_class;

  /* clear could_be_short */
  cli->could_be_short = 0;
  
  /* if we're in a string of short options, move up one char in the string */
  if (cli->is_short) {
    ++cli->text;
    if (cli->text[0] == 0)
      cli->is_short = 0;
    else if (want_argument > 0) {
      /* handle -[option]argument case */
      clp->hadarg = 1;
      clp->arg = cli->text + 1;
      cli->is_short = 0;
      return 0;
    }
  }
  
  /* if in short options, we're all set */
  if (cli->is_short)
    return 1;
  
  /** if not in short options, move to the next argument **/
  cli->whole_negated = 0;
  
  if (cli->argc <= 1)
    return 0;
  
  cli->argc--;
  cli->argv++;
  text = cli->argv[0];
  
  if (want_argument > 1)
    goto not_option;
  
  option_class = cli->option_class[ (unsigned char)text[0] ];
  if (text[0] == '-' && text[1] == '-')
    option_class = Clp_DoubledLong;
  
  /* If this character could introduce either a short or a long option,
     try a long option first, but remember that short's a possibility for
     later. */
  if ((option_class & (Clp_Short | Clp_ShortNegated))
      && (option_class & (Clp_Long | Clp_LongNegated))) {
    option_class &= ~(Clp_Short | Clp_ShortNegated);
    if (text[1] != 0) cli->could_be_short = 1;
  }
  
  switch (option_class) {
    
   case Clp_Short:
    cli->is_short = 1;
    goto check_singleton;
    
   case Clp_ShortNegated:
    cli->is_short = 1;
    cli->whole_negated = 1;
    goto check_singleton;
    
   case Clp_Long:
    goto check_singleton;
    
   case Clp_LongNegated:
    cli->whole_negated = 1;
    goto check_singleton;
    
   check_singleton:
    /* For options introduced with one character, option-char,
       `[option-char]' alone is NOT an option. */
    if (text[1] == 0)
      goto not_option;
    set_option_text(cli, text, 1);
    break;
    
   case Clp_LongImplicit:
    /* LongImplict: option_chars == "" (since all chars are part of the
       option); restore head -> text of option */
    if (want_argument > 0)
      goto not_option;
    set_option_text(cli, text, 0);
    break;
    
   case Clp_DoubledLong:
    set_option_text(cli, text, 2);
    break;
    
   not_option: 
   case Clp_NotOption:
    cli->is_short = 0;
    clp->hadarg = 1;
    clp->arg = text;
    return 0;
    
   default:
    assert(0 && "misconfiguration");
    
  }
  
  return 1;
}


static void
switch_to_short_argument(Clp_Parser *clp)
{
  Clp_Internal *cli = clp->internal;
  char *text = cli->argv[0];
  int option_class = cli->option_class[ (unsigned char)text[0] ];
  cli->is_short = 1;
  cli->whole_negated = (option_class & Clp_ShortNegated ? 1 : 0);
  set_option_text(cli, cli->argv[0], 1);
  assert(cli->could_be_short);
}


static Clp_Option *
find_long(Clp_Parser *clp, char *arg)
     /* If arg corresponds to one of clp's options, finds that option
	& returns it.
	If any argument is given after an = sign in arg,
	sets clp->hadarg = 1 and clp->arg to that argument.
	Sets cli->ambiguous = 1 iff there was no match because the argument
	given was ambiguous. */
{
  Clp_Internal *cli = clp->internal;
  int i;
  int len;
  int ambiguous = 0;
  Clp_Option *opt = cli->opt;
  
  /* Look for a normal option. */
  for (i = 0; i < cli->nopt; i++)
    if (opt[i].long_name) {
      len = argcmp( opt[i].long_name, arg, opt[i].long_min_match );
      if (len > 0)
	goto worked;
      else if (len < 0)
	ambiguous = 1;
    }
  
 try_negation:
  /* If we can't find it, look for a negated option. */
  if (arg[0] == 'n' && arg[1] == 'o' && arg[2] == '-') {
    arg += 3;
    clp->negated = !clp->negated;
    
    for (i = 0; i < cli->nopt; i++)
      if (opt[i].long_name) {
	len = argcmp( opt[i].long_name, arg, opt[i].long_min_match );
	if (len > 0)
	  goto worked;
	else if (len < 0)
	  ambiguous = 1;
      }
    
    /* I know this is silly, but it makes me happy to accept
       --no-no-option as a double negative synonym for --option. :) */
    goto try_negation;
  }
  
  cli->ambiguous = ambiguous;
  return 0;
  
 worked:
  
  if (arg[ len ] == '=') {
    clp->hadarg = 1;
    clp->arg = arg + len + 1;
  }
  return &opt[i];
}


static Clp_Option *
find_short(Clp_Parser *clp, int short_name)
     /* If short_name corresponds to one of clp's options, returns it. */
{
  Clp_Internal *cli = clp->internal;
  Clp_Option *opt = cli->opt;
  int i;
  
  for (i = 0; i < cli->nopt; i++)
    if (opt[i].short_name == short_name)
      return &opt[i];
  
  cli->ambiguous = 0;
  return 0;
}


int
Clp_Next(Clp_Parser *clp)
     /* Gets and parses the next argument from the argument list.
	
        If there are no more arguments, returns Clp_Done.
	If the next argument isn't an option, returns Clp_NotOption;
	the argument is stored in clp->arg.
	If the next argument is an option, returns that option's option_id.
	
	If the next argument is an unrecognizable or ambiguous option,
	an error message is given and Clp_BadOption is returned.
	
	If an option has an argument, that argument is stored in clp->arg
	and clp->hadarg is set to 1.
	Furthermore, that argument's parsed value (according to its type)
	is stored in the clp->val union.
	
	If an option needs an argument but isn't given one;
	if it doesn't need an argument but IS given one;
	or if the argument is the wrong type,
	an error message is given and Clp_BadOption is returned. */
{
  Clp_Internal *cli = clp->internal;
  Clp_Option *opt;
  Clp_ParserState clpsave;
  int complain;
  
  /** Set up clp **/
  clp->hadarg = 0;
  clp->arg = 0;
  cli->current_option = 0;
  
  /** Get the next argument or option **/
  if (!next_argument(clp, cli->option_processing ? 0 : 2))
    return clp->hadarg ? Clp_NotOption : Clp_Done;
  
  clp->negated = cli->whole_negated;
  if (cli->is_short)
    opt = find_short(clp, cli->text[0]);
  else
    opt = find_long(clp, cli->text);
  
  /** If there's ambiguity between long & short options, and we couldn't find
      a long option, look for a short option **/
  if (!opt && cli->could_be_short) {
    switch_to_short_argument(clp);
    opt = find_short(clp, cli->text[0]);
  }
  
  /** If we didn't find an option... **/
  if (!opt || (clp->negated && !TEST(opt, Clp_Negate))) {
    
    /* default processing for the "--" option: turn off option processing
       and return the next argument */
    if (strcmp(cli->argv[0], "--") == 0) {
      Clp_SetOptionProcessing(clp, 0);
      return Clp_Next(clp);
    }
    
    /* otherwise, report some error or other */
    if (cli->ambiguous)
      return Clp_OptionError(clp, "`%s%s' is ambiguous",
			     cli->option_chars, cli->text);
    else if (cli->is_short)
      return Clp_OptionError(clp, "unrecognized option `%s%c'",
			     cli->option_chars, cli->text[0]);
    else
      return Clp_OptionError(clp, "unrecognized option `%s%s'",
			     cli->option_chars, cli->text);
  }
  
  /** Set the current option **/
  cli->current_option = opt;
  cli->current_short = cli->is_short;
  cli->negated_by_no = clp->negated && !cli->whole_negated;
  
  /** The no-argument (or should-have-no-argument) case **/
  if (clp->negated || !TEST(opt, Clp_AnyArgument)) {
    if (clp->hadarg)
      return Clp_OptionError(clp, "`%O' can't take an argument");
    else
      return opt->option_id;
  }
  
  /** Get an argument if we need one, or if it's optional **/
  /* complain == 1 only if the argument was explicitly given,
     or it is mandatory. */
  complain = (clp->hadarg != 0) || TEST(opt, Clp_Mandatory);
  Clp_SaveParser(clp, &clpsave);
  
  if (cli->is_short && cli->text[1] != 0) {
    /* The -[option]argument case:
       Assume that the rest of the current string is the argument. */
    clp->hadarg = 1;
    clp->arg = cli->text + 1;
    cli->is_short = 0;
    
  } else if (TEST(opt, Clp_Mandatory) && !clp->hadarg) {
    /* Mandatory argument case */
    /* Allow arguments to options to start with a dash, but only if the user
       gave the Clp_AllowDash flag */
    next_argument(clp, TEST(opt, Clp_AllowDash) ? 2 : 1);
    if (!clp->hadarg) {
      Clp_RestoreParser(clp, &clpsave);
      return Clp_OptionError(clp, "`%O' requires an argument");
    }
  }
  
  /** Parse the argument **/
  if (clp->hadarg) {
    Clp_ArgType *atr;
    int at = opt->arg_type;
    
    if (at <= 0 || at >= cli->nargtype || cli->argtype[at].func == 0)
      return Clp_Error;
    
    atr = &cli->argtype[at];
    if (atr->func(clp, clp->arg, atr->data, complain) <= 0) {
      /* parser failed */
      clp->hadarg = 0;
      if (TEST(opt, Clp_Mandatory))
	return Clp_BadOption;
      else
	Clp_RestoreParser(clp, &clpsave);
    }
  }
  
  return opt->option_id;
}


char *
Clp_GetNextArgument(Clp_Parser *clp)
     /* Returns the next argument from the argument list without parsing it.
        If there are no more arguments, returns 0. */
{
  clp->hadarg = 0;
  clp->arg = 0;
  next_argument(clp, 2);
  return clp->arg;
}


/*******
 * Clp_OptionError
 **/

int
Clp_OptionError(Clp_Parser *clp, const char *fmt, ...)
     /* Reports an error for parser clp. Allowable % format characters are:

	s	Print a string from the argument list.
	c	Print an int from the argument list as a character.
	d	Print an int from the argument list.
	O	Print the name of the current option;
		take nothing from the argument list.
		
	No field specifications or flags are allowed.
	Always returns Clp_BadOption. */
{
  Clp_Internal *cli = clp->internal;
  const char *percent;
  va_list val;
  
  if (cli->error_hook) (*cli->error_hook)();
  fprintf(stderr, "%s: ", cli->program_name);
  
  va_start(val, fmt);
  
  for (percent = strchr(fmt, '%'); percent; percent = strchr(fmt, '%')) {
    fwrite(fmt, 1, percent - fmt, stderr);
    switch (*++percent) {
      
     case 's': {
       char *s = va_arg(val, char *);
       if (s) fputs(s, stderr);
       else fputs("(null)", stderr);
       break;
     }
     
     case 'c': {
       int c = va_arg(val, int);
       fputc(c, stderr);
       break;
     }
     
     case 'd': {
       int d = va_arg(val, int);
       fprintf(stderr, "%d", d);
       break;
     }
     
     case 'O': {
       Clp_Option *opt = cli->current_option;
       if (!opt)
	 fputs("(no current option!)", stderr);
       if (cli->current_short)
	 fprintf(stderr, "%s%c", cli->option_chars, opt->short_name);
       else if (cli->negated_by_no)
	 fprintf(stderr, "%sno-%s", cli->option_chars, opt->long_name);
       else
	 fprintf(stderr, "%s%s", cli->option_chars, opt->long_name);
       break;
     }
     
     case '%':
      fputc('%', stderr);
      break;
      
     default:
      assert(0);
      break;
      
    }
    fmt = ++percent;
  }
  
  fputs(fmt, stderr);
  fputc('\n', stderr);
  
  va_end(val);
  return Clp_BadOption;
}


#ifdef __cplusplus
}
#endif