#include "postgres.h"
#include "access/gin.h"
#include "access/skey.h"
#include "fmgr.h"
#include "utils/builtins.h"

#include "vgram.h"

Datum vgram_cmp(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(vgram_cmp);

Datum vgram_gin_extract_value(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(vgram_gin_extract_value);

Datum vgram_gin_consitent(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(vgram_gin_consitent);

Datum vgram_gin_extract_query(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(vgram_gin_extract_query);

static int
vgram_cmp_internal(Datum d1, Datum d2)
{
	text *vgram1 = DatumGetTextPP(d1);
	text *vgram2 = DatumGetTextPP(d2);
	int len1, len2, cmp;
	
	len1 = VARSIZE_ANY_EXHDR(vgram1);
	len2 = VARSIZE_ANY_EXHDR(vgram2);
	
	cmp = strncmp(VARDATA_ANY(vgram1), VARDATA_ANY(vgram2), Min(len1, len2));
	if (cmp != 0)
		return cmp;
	if (len1 < len2)
		return -1;
	else if (len1 == len2)
		return 0;
	else
		return 1;
}

static int
vgram_sort_cmp(const void *v1, const void *v2)
{
	return vgram_cmp_internal(
		*((Datum *)v1),
		*((Datum *)v2)
	);
}

static void
entries_unique(Datum *entries, int32 *nentries)
{
	int32 n = *nentries, i, j = 0;
	
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
	Datum d1 = PG_GETARG_DATUM(0);
	Datum d2 = PG_GETARG_DATUM(1);
	PG_RETURN_INT32(vgram_cmp_internal(d1, d2));
}

typedef struct
{
	Datum	*entries;
	int32	 nentries;
	int32	 allocatedEntries;
} ExtractValueInfo;

static void
extractVGram(char *vgram, void *userData)
{
	ExtractValueInfo *info = (ExtractValueInfo *)userData;
	
	info->nentries++;
	if (info->nentries > info->allocatedEntries)
	{
		info->allocatedEntries *= 2;
		info->entries = (Datum *)repalloc(info->entries, sizeof(Datum) * info->allocatedEntries);
	}
	info->entries[info->nentries - 1] = PointerGetDatum(cstring_to_text(vgram));
	pfree(vgram);
}

Datum
vgram_gin_extract_value(PG_FUNCTION_ARGS)
{
	text	   *s = (text *) PG_GETARG_TEXT_PP(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	ExtractValueInfo info;
	ExtractVGramsInfo userData;
	
	loadStats();
	
	info.nentries = 0;
	info.allocatedEntries = 4;
	info.entries = (Datum *)palloc(sizeof(Datum) * info.allocatedEntries);
	
	userData.callback = extractVGram;
	userData.userData = &info;
	
	extractWords(VARDATA_ANY(s), VARSIZE_ANY_EXHDR(s), extractMinimalVGramsWord, &userData);
	
	PG_FREE_IF_COPY(s, 0);
	
	entries_unique(info.entries, &info.nentries);
	
	*nentries = info.nentries;
	PG_RETURN_POINTER(info.entries);	
}

Datum
vgram_gin_consitent(PG_FUNCTION_ARGS)
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

	loadStats();
	
	switch (strategy)
	{
		case ILikeStrategyNumber:
		case LikeStrategyNumber:

			entries = extractQueryLike(nentries, val);
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
