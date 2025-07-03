/*-------------------------------------------------------------------------
 *
 * vgram_like.c
 *		Routines for using index over V-grams to accelerate like/ilike
 *		queries.
 *
 * Copyright (c) 2011-2017, Alexander Korotkov
 *
 * IDENTIFICATION
 *	  contrib/vgram/vgram_like.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_collation_d.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "varatt.h"

#include "vgram.h"

#define ISESCAPECHAR(x) (*(x) == '\\')	/* Wildcard escape character */
#define ISWILDCARDCHAR(x) (*(x) == '_' || *(x) == '%')	/* Wildcard
														 * meta-character */

/*
 * Extract the next non-wildcard part of a search string, ie, a word bounded
 * by '_' or '%' meta-characters, non-word characters or string end.
 *
 * str: source string, of length lenstr bytes (need not be null-terminated)
 * buf: where to return the substring (must be long enough)
 * *bytelen: receives byte length of the found substring
 * *charlen: receives character length of the found substring
 *
 * Returns pointer to end+1 of the found substring in the source string.
 * Returns NULL if no word found (in which case buf, bytelen, charlen not set)
 *
 * If the found word is bounded by non-word characters or string boundaries
 * then this function will include corresponding padding spaces into buf.
 */
static const char *
get_wildcard_part(const char *str, int lenstr,
				  char *buf, int *bytelen, int *charlen)
{
	const char *beginword = str;
	const char *endword;
	char	   *s = buf;
	bool		in_wildcard_meta = false;
	bool		in_escape = false;
	int			clen;

	/*
	 * Find the first word character remembering whether last character was
	 * wildcard meta-character.
	 */
	while (beginword - str < lenstr)
	{
		if (in_escape)
		{
			in_escape = false;
			in_wildcard_meta = false;
			if (isExtractable(beginword))
				break;
		}
		else
		{
			if (ISESCAPECHAR(beginword))
				in_escape = true;
			else if (ISWILDCARDCHAR(beginword))
				in_wildcard_meta = true;
			else if (isExtractable(beginword))
				break;
			else
				in_wildcard_meta = false;
		}
		beginword += pg_mblen(beginword);
	}

	/*
	 * Handle string end.
	 */
	if (beginword - str >= lenstr)
		return NULL;

	/*
	 * Add left padding spaces if last character wasn't wildcard
	 * meta-character.
	 */
	*charlen = 0;
	if (!in_wildcard_meta)
	{
		*s++ = EMPTY_CHARACTER;
		(*charlen)++;
	}

	/*
	 * Copy data into buf until wildcard meta-character, non-word character or
	 * string boundary.  Strip escapes during copy.
	 */
	endword = beginword;
	in_wildcard_meta = false;
	in_escape = false;
	while (endword - str < lenstr)
	{
		clen = pg_mblen(endword);
		if (in_escape)
		{
			in_escape = false;
			in_wildcard_meta = false;
			if (isExtractable(endword))
			{
				memcpy(s, endword, clen);
				(*charlen)++;
				s += clen;
			}
			else
				break;
		}
		else
		{
			if (ISESCAPECHAR(endword))
				in_escape = true;
			else if (ISWILDCARDCHAR(endword))
			{
				in_wildcard_meta = true;
				break;
			}
			else if (isExtractable(endword))
			{
				memcpy(s, endword, clen);
				(*charlen)++;
				s += clen;
			}
			else
			{
				in_wildcard_meta = false;
				break;
			}
		}
		endword += clen;
	}

	/*
	 * Add right padding spaces if last character wasn't wildcard
	 * meta-character.
	 */
	if (!in_wildcard_meta)
	{
		*s++ = EMPTY_CHARACTER;
		(*charlen)++;
	}

	*bytelen = s - buf;
	return endword;
}

#define OPTIMAL_VGRAM_COUNT 5

typedef struct
{
	char	  **data;
	int			count;
	int			allocated;
}	VGramInfo;

static void
addVGram(char *vgram, void *userData)
{
	VGramInfo  *vgrams = (VGramInfo *) userData;

	if (vgrams->count >= vgrams->allocated)
	{
		vgrams->allocated *= 2;
		vgrams->data = (char **) repalloc(vgrams->data, sizeof(char *) * vgrams->allocated);
	}

	vgrams->data[vgrams->count] = vgram;
	vgrams->count++;
}


Datum *
extractQueryLike(VGramOptions *options, int32 *nentries, text *pattern)
{
	char	   *buf,
			   *buf2;
	const char *eword,
			   *str;
	int			len,
				bytelen,
				charlen,
				i;
	VGramInfo	vgrams;
	ExtractVGramsInfo userData;
	Datum	   *entries;

	userData.callback = addVGram;
	userData.options = options;
	userData.userData = (void *) &vgrams;

	vgrams.count = 0;
	vgrams.allocated = 16;
	vgrams.data = (char **) palloc(sizeof(char *) * vgrams.allocated);

	str = (char *) VARDATA_ANY(pattern);
	len = VARSIZE_ANY_EXHDR(pattern);

	buf = (char *) palloc(len + 3);
	eword = str;
	while ((eword = get_wildcard_part(eword, len - (eword - str),
									  buf, &bytelen, &charlen)) != NULL)
	{
		buf2 = str_tolower(buf, bytelen, DEFAULT_COLLATION_OID);
		bytelen = strlen(buf2);

		extractMinimalVGramsWord(buf2, buf2 + bytelen, &userData);

		pfree(buf2);
	}
	pfree(buf);

	*nentries = vgrams.count;

	entries = (Datum *) palloc(sizeof(Datum) * vgrams.count);
	for (i = 0; i < vgrams.count; i++)
	{
		entries[i] = PointerGetDatum(cstring_to_text(vgrams.data[i]));
	}
	return entries;
}
