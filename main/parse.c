/* **********************************************************
 * Copyright 2000 VMware, Inc.  All rights reserved. -- VMware Confidential
 * **********************************************************/

/*
 * parse.c --
 *
 *      Simple parsing utility routines.
 */

#include "vm_types.h"
#include "vm_libc.h"
#include "return_status.h"
#include "libc.h"
#include "vmkernel.h"

#define LOGLEVEL_MODULE Parse
#include "log.h"

#include "parse.h"

/*
 *----------------------------------------------------------------------
 *
 * Parse_Args --
 *
 *      Parse "buf" as a vector of up to "argc" arguments delimited
 *	by whitespace.  Updates "buf" in-place, replacing whitespace
 *	with NULs, and sets elements of "argv" to the start of each
 *	parsed argument.
 *
 * Results:
 *	Modifies "buf", updates "argv" to point to parsed arguments.
 *	Returns the number of parsed arguments.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
Parse_Args(char *buf,		// IN/OUT: source string
           char *argv[],	// IN/OUT: arg vector
           int argc)		// IN:     vector size
{
   Bool space;
   char *s;
   int n;

   // initialize
   space = TRUE;
   n = 0;

   // modify buf in-place to construct argv
   for (s = buf; *s != '\0'; s++) {
      if ((*s == ' ') || (*s == '\t') || (*s == '\n')) {
         // convert whitespace into NULs
         *s = '\0';
         space = TRUE;
      } else {
         // parse next arg
         if (space && (n < argc)) {
            argv[n++] = s;
         }
         space = FALSE;
      }
   }
   
   return(n);
}

/*
 *----------------------------------------------------------------------
 *
 * Parse_Consolidate_String --
 *
 *      Consolidates (removes spaces from) a string in-place.
 *
 * Results:
 *
 *      Modifies and consolidates "str".  For example:
 *      "bad beer  rots 89 " becomes "badbeerrots89"
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
Parse_ConsolidateString(char *str)
{
   int spaces = 0;

   /* This algorithim skips spaces as they occur and 
    * copies data ahead of the space into the space.
    * Keeps track of the number of spaces to know how far
    * to look ahead.
    */
   while (*(str+spaces) != '\0') {
      if (*(str + spaces) == ' ') {
         spaces++;
      } else {
         *str = *(str + spaces);
         str++;
      }
   }
   *str = '\0';
}

/*
 *----------------------------------------------------------------------
 *
 * Parse_RangeList --
 * 
 *    Determine if "val" is in the range list "str".
 *    A range list is a comma seperated list of '-' delimited ranges.
 *    Single values are also valid.
 *    Searching stops when/if a ";" is reached.
 *    An example of a valid range:
 *    str = "1-3,26-35,1023,5-18,69,41-43;"
 *
 * Results:

 *    Returns true if "val" is in the range "str", false otherwise.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */
Bool
Parse_RangeList(const char *str0, unsigned long val)
{
   unsigned long start, end;
   char *endPtr0, *endPtr1, *str1;
   const char *ptr;
   /* Valid characters for a range list */
   char *valid = "0123456789,-;";

   for(ptr = str0; (*ptr != '\0') && (*ptr != ';'); ptr++) {
      if (strchr(valid, *ptr) == NULL) {
         return(0);
      }
   }
   
   while(1) {
      start = simple_strtoul(str0, &endPtr0, 10);

      if (*endPtr0 == '-') {
         end = simple_strtoul(++endPtr0, &endPtr1, 10);
         if ((val >= start) && (val <= end)) {
            return(1);
         }
      }

      if (start == val) {
         return(1);
      } else if(((str1 = strchr(str0, ',')) == NULL) || (str1 > strchr(str0, ';'))) {
         return(0);
      } else {
         str0 = str1 + sizeof(char);
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * ParseInteger --
 *
 *      Parses the first "len" characters of "buf" as an unsigned
 *	integer number in specified "base", followed by optional
 *	whitespace.  Sets "value" to the parsed number.
 *
 * Results:
 *	Returns VMK_OK if successful, VMK_BAD_PARAM otherwise.
 *	Sets "value" to parsed number.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static VMK_ReturnStatus
ParseInteger(const char *buf, int len, int base, uint32 *value)
{
   char *endp;

   // parse decimal number, possibly followed by whitespace
   *value = simple_strtoul(buf, &endp, base);
   while (endp < (buf + len)) {
      // check for trailing non-whitespace characters
      if ((*endp != '\n') && (*endp != ' ') && (*endp != '\t')) {
         return(VMK_BAD_PARAM);
      }
      endp++;
   }

   // successful
   return(VMK_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * Parse_Int --
 *
 *      Parses the first "len" characters of "buf" as an unsigned
 *	decimal integer number, followed by optional whitespace.
 *	Sets "value" to the parsed number.
 *
 * Results:
 *	Returns VMK_OK if successful, VMK_BAD_PARAM otherwise.
 *	Sets "value" to parsed number.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Parse_Int(const char *buf, int len, uint32 *value)
{
   return(ParseInteger(buf, len, 10, value));
}

/*
 *----------------------------------------------------------------------
 *
 * Parse_Hex --
 *
 *      Parses the first "len" characters of "buf" as an unsigned
 *	hex integer number, followed by optional whitespace.
 *	Sets "value" to the parsed number.
 *
 * Results:
 *	Returns VMK_OK if successful, VMK_BAD_PARAM otherwise.
 *	Sets "value" to parsed number.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Parse_Hex(const char *buf, int len, uint32 *value)
{
   return(ParseInteger(buf, len, 16, value));
}

/*
 *----------------------------------------------------------------------
 *
 * Parse_IntMask --
 *
 *      Parse "buf" as a set of small unsigned integers, separated
 *	by commas or whitespace.  Each parsed number must be less
 *	than MIN("max", 32).  Sets "value" to the bitmask containing
 *	all parsed numbers.   Updates "buf" in-place, replacing 
 *	whitespace with NULs.  If unsuccessful, sets "badToken" to 
 *	the offending string that caused the parse to fail.
 *
 * Results:
 *	Returns VMK_OK if successful, otherwise error code.
 *	Sets "value" to the parsed bitmask.
 *	Sets "badToken" to offending string if parse unsuccessful.
 *	Modifies "buf".
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
VMK_ReturnStatus
Parse_IntMask(char *buf, uint32 max, uint32 *value, char **badToken)
{
   char *argv[32];
   int argc, i;
   uint32 mask;

   // initialize
   *badToken = NULL;

   // validate "max"
   if (max > 32) {
      return(VMK_BAD_PARAM);
   }

   // accept commas as delimiters, convert into spaces
   for (i = 0; buf[i] != '\0'; i++) {
      if (buf[i] == ',') {
         buf[i] = ' ';
      }
   }

   // parse buffer as argument vector, fail if empty
   argc = Parse_Args(buf, argv, 32);
   if (argc < 1) {
      return(VMK_BAD_PARAM);
   }
   
   // update mask for each specified number
   mask = 0;
   for (i = 0; i < argc; i++) {
      uint32 n;

      // parse number, fail if unable
      if (Parse_Int(argv[i], strlen(argv[i]), &n) != VMK_OK) {
         *badToken = argv[i];
         return(VMK_BAD_PARAM);
      }

      // fail if invalid number
      if (n >= max) {
         *badToken = argv[i];
         return(VMK_BAD_PARAM);
      }

      // valid number
      mask |= (1 << n);
   }
   
   // everything OK
   *value = mask;
   return(VMK_OK);
}

#define isdigit(c)      ((c) >= '0' && (c) <= '9')

/*
 *-----------------------------------------------------------------------------
 *
 * Parse_Int64 --
 *
 *     Interprets the first "len" characters of "buf" as a 64-bit signed
 *     integer and stores the resulting int64 in "result"
 *     Note that there can be no whitespace in the string to be parsed.
 *
 * Results:
 *     Returns VMK_OK on success, VMK_BAD_PARAM otherwise.
 *
 * Side effects:
 *     None.
 *
 *-----------------------------------------------------------------------------
 */
VMK_ReturnStatus
Parse_Int64(char *buf, int len, int64 *result)
{
   int i;
   int multiplier = 1;
   int64 res = 0;

   for (i=1; i <= len; i++) {
      char curChar = buf[len - i];

      if (i == len && curChar == '-') {
         res = -res;
         break;
      } else if (!isdigit(curChar)) {
         LOG(1, "char %c not digit\n", curChar);
         return VMK_BAD_PARAM;
      }

      res += (curChar - '0') * multiplier;
      multiplier *= 10;
   }

   *result = res;
   
   return VMK_OK;
}
