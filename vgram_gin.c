/*-------------------------------------------------------------------------
 *
 * vgram_gin.c
 *		Routines for GIN indexing of V-grams extracted from strings.
 *
 * Copyright (c) 2011-2017, Alexander Korotkov
 *
 * IDENTIFICATION
 *	  contrib/vgram/vgram_gin.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "access/gin.h"
#include "access/reloptions.h"
#include "access/skey.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "varatt.h"
#include "vgram.h"

Datum		vgram_cmp(PG_FUNCTION_ARGS);
Datum		vgram_gin_extract_value(PG_FUNCTION_ARGS);
Datum		vgram_gin_consistent(PG_FUNCTION_ARGS);
Datum		vgram_gin_triconsistent(PG_FUNCTION_ARGS);
Datum		vgram_gin_extract_query(PG_FUNCTION_ARGS);
Datum		vgram_gin_options(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(vgram_cmp);
PG_FUNCTION_INFO_V1(vgram_gin_extract_value);
PG_FUNCTION_INFO_V1(vgram_gin_consistent);
PG_FUNCTION_INFO_V1(vgram_gin_triconsistent);
PG_FUNCTION_INFO_V1(vgram_gin_extract_query);
PG_FUNCTION_INFO_V1(vgram_gin_options);

static int
vgram_cmp_internal(Datum d1, Datum d2)
{
	text	   *vgram1 = DatumGetTextPP(d1);
	text	   *vgram2 = DatumGetTextPP(d2);
	int			len1,
				len2,
				cmp;

	len1 = VARSIZE_ANY_EXHDR(vgram1);
	len2 = VARSIZE_ANY_EXHDR(vgram2);

	cmp = memcmp(VARDATA_ANY(vgram1), VARDATA_ANY(vgram2), Min(len1, len2));
	if (cmp != 0)
		return cmp;
	if (len1 < len2)
		return -1;
	else if (len1 == len2)
		return 0;
	else
		return 1;
}

int
vgram_sort_cmp(const void *v1, const void *v2)
{
	return vgram_cmp_internal(*((Datum *) v1),
							  *((Datum *) v2));
}

static void
entries_unique(Datum *entries, int32 *nentries)
{
	int32		n = *nentries,
				i,
				j = 0;

	if (n == 0)
		return;

	qsort(entries, n, sizeof(Datum *), vgram_sort_cmp);

	for (i = 1; i < n; i++)
	{
		if (vgram_cmp_internal(entries[i], entries[j]))
		{
			j++;
			entries[j] = entries[i];
		}
	}
	*nentries = j + 1;
}

Datum
vgram_cmp(PG_FUNCTION_ARGS)
{
	Datum		d1 = PG_GETARG_DATUM(0);
	Datum		d2 = PG_GETARG_DATUM(1);

	PG_RETURN_INT32(vgram_cmp_internal(d1, d2));
}

typedef struct
{
	Datum	   *entries;
	int32		nentries;
	int32		allocatedEntries;
}	ExtractValueInfo;

static void
extractVGram(char *vgram, void *userData)
{
	ExtractValueInfo *info = (ExtractValueInfo *) userData;

	info->nentries++;
	if (info->nentries > info->allocatedEntries)
	{
		info->allocatedEntries *= 2;
		info->entries = (Datum *) repalloc(info->entries, sizeof(Datum) * info->allocatedEntries);
	}
	info->entries[info->nentries - 1] = PointerGetDatum(cstring_to_text(vgram));
	pfree(vgram);
}

Datum
vgram_gin_extract_value(PG_FUNCTION_ARGS)
{
	text	   *s = (text *) PG_GETARG_TEXT_PP(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	VGramOptions *options = (VGramOptions *) PG_GET_OPCLASS_OPTIONS();

	ExtractValueInfo info;
	ExtractVGramsInfo userData;

	info.nentries = 0;
	info.allocatedEntries = 4;
	info.entries = (Datum *) palloc(sizeof(Datum) * info.allocatedEntries);

	userData.callback = extractVGram;
	userData.options = options;
	userData.userData = &info;

	extractWords(VARDATA_ANY(s), VARSIZE_ANY_EXHDR(s), extractMinimalVGramsWord, &userData);

	PG_FREE_IF_COPY(s, 0);

	entries_unique(info.entries, &info.nentries);

	*nentries = info.nentries;
	PG_RETURN_POINTER(info.entries);
}

Datum
vgram_gin_consistent(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* text    *query = PG_GETARG_TEXT_P(2); */
	int32		nkeys = PG_GETARG_INT32(3);

	/* Pointer	  *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		res;
	int32		i;

	/* All cases served by this function are inexact */
	*recheck = true;

	switch (strategy)
	{
		case ILikeStrategyNumber:
		case LikeStrategyNumber:
			/* Check if all extracted trigrams are presented. */
			res = true;
			for (i = 0; i < nkeys; i++)
			{
				if (!check[i])
				{
					res = false;
					break;
				}
			}
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			res = false;		/* keep compiler quiet */
			break;
	}

	PG_RETURN_BOOL(res);
}

Datum
vgram_gin_triconsistent(PG_FUNCTION_ARGS)
{
	GinTernaryValue *check = (GinTernaryValue *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* text    *query = PG_GETARG_TEXT_P(2); */
	int32		nkeys = PG_GETARG_INT32(3);

	/* Pointer	  *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	GinTernaryValue res = GIN_MAYBE;
	int32		i;

	switch (strategy)
	{
		case ILikeStrategyNumber:
		case LikeStrategyNumber:
			/* Check if all extracted trigrams are presented. */
			for (i = 0; i < nkeys; i++)
			{
				if (check[i] == GIN_FALSE)
				{
					res = GIN_FALSE;
					break;
				}
			}
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			res = false;		/* keep compiler quiet */
			break;
	}

	/* All cases served by this function are inexact */
	Assert(res != GIN_TRUE);
	PG_RETURN_GIN_TERNARY_VALUE(res);
}

Datum
vgram_gin_extract_query(PG_FUNCTION_ARGS)
{
	text	   *val = (text *) PG_GETARG_TEXT_P(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);

	/* bool   **pmatch = (bool **) PG_GETARG_POINTER(3); */
	/* Pointer	  *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	/* bool   **nullFlags = (bool **) PG_GETARG_POINTER(5); */
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	Datum	   *entries = NULL;
	VGramOptions *options = (VGramOptions *) PG_GET_OPCLASS_OPTIONS();


	switch (strategy)
	{
		case ILikeStrategyNumber:
		case LikeStrategyNumber:

			entries = extractQueryLike(options, nentries, val);
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			break;
	}

	entries_unique(entries, nentries);

	/*
	 * If no trigram was extracted then we have to scan all the index.
	 */
	if (*nentries == 0)
		*searchMode = GIN_SEARCH_MODE_ALL;

	PG_RETURN_POINTER(entries);
}

static void
vgrams_validator(const char *value)
{
	ArrayType *arr;
	int		nVgrams;
	Datum  *elems;

	arr = DatumGetArrayTypeP(DirectFunctionCall3(array_in,
							 CStringGetDatum(value),
							 ObjectIdGetDatum(TEXTOID),
							 Int32GetDatum(-1)));

	deconstruct_array(arr, TEXTOID, -1, false, 'i', &elems, NULL, &nVgrams);
}

Size
vgrams_fill(ArrayType *arr, void *ptr)
{
	int		nVgrams;
	Datum  *elems;
	int		i;
	Size	size = sizeof(int);
	Pointer p = (Pointer) ptr;

	deconstruct_array(arr, TEXTOID, -1, false, 'i', &elems, NULL, &nVgrams);

	qsort(elems, nVgrams, sizeof(Datum), vgram_sort_cmp);

	size += nVgrams * sizeof(int);
	if (p)
		*(int *) p = nVgrams;

	for (i = 0; i < nVgrams; i++)
	{
		char   *vgram = VARDATA_ANY(elems[i]);
		int		vgramSize = VARSIZE_ANY_EXHDR(elems[i]);

		if (p)
		{
			*(int *) (p + (i + 1) * sizeof(int)) = (int) size;
			memcpy(p + size, vgram, vgramSize);
			*(p + size + vgramSize + 1) = '\0';
		}
		size += vgramSize + 1;
	}

	return size;
}

static Size
vgrams_fill_string(const char *value, void *ptr)
{
	ArrayType *arr;

	arr = DatumGetArrayTypeP(DirectFunctionCall3(array_in,
							 CStringGetDatum(value),
							 ObjectIdGetDatum(TEXTOID),
							 Int32GetDatum(-1)));

	return vgrams_fill(arr, ptr);
}

Datum
vgram_gin_options(PG_FUNCTION_ARGS)
{
	local_relopts *relopts = (local_relopts *) PG_GETARG_POINTER(0);

	init_local_reloptions(relopts, offsetof(VGramOptions, vgramsCount));
	add_local_int_reloption(relopts, "minQ",
							"minimal vgram size",
							2,
							1,
							10,
							offsetof(VGramOptions, minQ));
	add_local_int_reloption(relopts, "maxQ",
							"maximal vgram size",
							2,
							1,
							10,
							offsetof(VGramOptions, maxQ));
	add_local_string_reloption(relopts, "vgrams",
							   "an array of frequent vgrams",
							   NULL,
							   vgrams_validator,
							   vgrams_fill_string,
							   offsetof(VGramOptions, vgramsOffset));

	PG_RETURN_VOID();
}
