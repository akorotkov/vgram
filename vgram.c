/*-------------------------------------------------------------------------
 *
 * vgram.c
 *		Routines for Q-gram statistics collection and dividing strings
 *		into V-grams.
 *
 * Copyright (c) 2011-2025, Alexander Korotkov
 *
 * IDENTIFICATION
 *	  contrib/vgram/vgram.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "utils/memutils.h"
#include <stddef.h>

#include "vgram.h"

PG_MODULE_MAGIC;

Datum		get_vgrams(PG_FUNCTION_ARGS);
Datum		qgram_stat_transfn(PG_FUNCTION_ARGS);
Datum		qgram_stat_finalfn(PG_FUNCTION_ARGS);
Datum		vgram_text_like(PG_FUNCTION_ARGS);
Datum		vgram_text_iclike(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(get_vgrams);
PG_FUNCTION_INFO_V1(qgram_stat_transfn);
PG_FUNCTION_INFO_V1(qgram_stat_finalfn);
PG_FUNCTION_INFO_V1(vgram_text_like);
PG_FUNCTION_INFO_V1(vgram_text_iclike);

static void addVGram(char *vgram, void *userData);

/**
 * Search q-grams stat table for given prefix. Initially lower and upper
 * bounds should cover all indexes of qgramTable. Resulting lower and upper
 * bounds can be reused for search with longer prefix.
 *
 * @param prefix Pointer to prefix
 * @param len Length of prefix *in bytes*
 * @param lower Pointer to current lower bound
 * @param upper Pointer to current upper bound
 * @return Index of found q-gram
 */
static int
prefixQGramSearch(VGramOptions *options, const char *prefix,
				  int len, int *lower, int *upper)
{
	int			mid,
				cmp;

	while (*lower <= *upper)
	{
		mid = (*lower + *upper) / 2;
		cmp = strncmp(GET_VGRAM(options, mid), prefix, len);
		if (cmp < 0)
		{
			*lower = mid + 1;
		}
		else if (cmp > 0)
		{
			*upper = mid - 1;
		}
		else
		{
			return mid;
		}
	}
	return -1;
}

/**
 * Adds given q-gram to hash
 *
 * @param qgram
 * @param qgramsHash
 */
static void
addQGramToHash(char *qgram, QGramStatState *state)
{
	QGramHashKey key;
	QGramHashValue *value;
	bool		found;

	key.qgram = qgram;
	value = (QGramHashValue *) hash_search(state->qgramsHash,
										   (const void *) &key,
										   HASH_ENTER,
										   &found);
	if (!found)
	{
		value->count = 1;
		value->delta = state->bCurrent - 1;
		state->incrementedQGrams = lappend(state->incrementedQGrams, qgram);
	}
	else
	{
		if (!value->incremented)
		{
			value->count++;
			state->incrementedQGrams = lappend(state->incrementedQGrams, value->key.qgram);
		}
		pfree(qgram);
	}
	value->incremented = true;
	state->qgramsCount++;
}

/**
 * Collect statistics from distinct word.
 *
 * @param wordStart Pointer to the first character of the word.
 * @param wordEnd Pointer below to the last character of the word.
 * @param userData Pointer to QGramStatState structure.
 */
void
collectStatsWord(const char *wordStart, const char *wordEnd,
				 void *userData)
{
	QGramStatState *state = (QGramStatState *) userData;
	const char *p,
			   *r;
	int			q;

	/* Collect q-grams stat */
	for (q = state->minQ; q <= state->maxQ; q++)
	{
		char	   *qgram;
		int			pos = 0,
					size;

		p = wordStart, r = wordStart;
		do
		{
			pos++;
			p += pg_mblen(p);

			if (pos >= q)
			{
				size = p - r;
				qgram = (char *) MemoryContextAlloc(state->context, size + 1);
				qgram[size] = 0;
				memcpy(qgram, r, size);
				r += pg_mblen(r);

				addQGramToHash(qgram, state);
			}
		}
		while (p < wordEnd);
	}
}

#ifdef NOT_USED
/*
 * Extract rare-enough vgrams from starting from each position of the word.
 */
void
extractVGramsWord(const char *wordStart, const char *wordEnd, void *userData)
{
	const char *p = wordStart,
			   *r = wordStart;
	int			len = 0;
	ExtractVGramsInfo *info = (ExtractVGramsInfo *) userData;
	int			minQ = info->options->minQ,
				maxQ = info->options->maxQ;

	while (p < wordEnd)
	{
		int			lower = 0,
					upper = info->options->vgramsCount - 1;
		bool		first_time = true;

		while (first_time || (len < maxQ && r < wordEnd))
		{
			if (!first_time || r <= p)
			{
				r += pg_mblen(r);
				len++;
			}
			first_time = false;
			if (len >= minQ && prefixQGramSearch(info->options, p, r - p, &lower, &upper) < 0)
			{
				char	   *qgram;

				qgram = (char *) palloc(r - p + 1);
				memcpy(qgram, p, r - p);
				qgram[r - p] = 0;
				info->callback(qgram, info->userData);
				break;
			}
		}
		p += pg_mblen(p);
		len--;
	}
}
#endif

/*
 * Extract rare-enough vgrams that don't contain other rare-enough vgrams.
 */
void
extractMinimalVGramsWord(const char *wordStart, const char *wordEnd, void *userData)
{
	const char *p = wordStart,
			   *r = wordStart,
			   *prevR = NULL,
			   *prevP = NULL;
	int			len = 0;
	ExtractVGramsInfo *info = (ExtractVGramsInfo *) userData;
	int			minQ = info->options->minQ,
				maxQ = info->options->maxQ;

	while (p < wordEnd)
	{
		int			lower = 0,
					upper = info->options->vgramsCount - 1;
		bool		first_time = true;

		while (first_time || (len < maxQ && r < wordEnd))
		{
			if (!first_time || r <= p)
			{
				r += pg_mblen(r);
				len++;
			}
			first_time = false;
			if (len >= minQ && prefixQGramSearch(info->options, p, r - p, &lower, &upper) < 0)
			{
				if (prevR && prevP && prevR < r)
				{
					char	   *qgram;

					qgram = (char *) palloc(prevR - prevP + 1);
					memcpy(qgram, prevP, prevR - prevP);
					qgram[prevR - prevP] = 0;
					info->callback(qgram, info->userData);
				}
				prevR = r;
				prevP = p;
				break;
			}
		}
		p += pg_mblen(p);
		len--;
	}
	if (prevR && prevP)
	{
		char	   *qgram;

		qgram = (char *) palloc(prevR - prevP + 1);
		memcpy(qgram, prevP, prevR - prevP);
		qgram[prevR - prevP] = 0;
		info->callback(qgram, info->userData);
	}
}

/**
 * Extract words from given string add call callback function for each of them.
 * Word is a continuous sequence of isExtractable characters. Surrounds each
 * word with EMPTY_CHARACTER.
 *
 * @param string Pointer to the source string
 * @param len Length of string *in bytes*
 * @param callback Callback to be called for each word
 * @param userData Some additional pointer to be passed to callback
 */
void
extractWords(const char *string, size_t len, WordCallback callback,
			 void *userData)
{
	const char *p,
			   *end = string + len,
			   *firstExtractable = NULL;
	char	   *lower,
			   *buf;
	size_t		lowerLen;

	buf = (char *) palloc(len + 2);
	buf[0] = EMPTY_CHARACTER;

	for (p = string; p < end; p += pg_mblen(p))
	{
		if (isExtractable(p))
		{
			if (!firstExtractable)
				firstExtractable = p;
		}
		else
		{
			if (firstExtractable)
			{
				lower = str_tolower(firstExtractable, p - firstExtractable, DEFAULT_COLLATION_OID);
				lowerLen = strlen(lower);
				memcpy(buf + 1, lower, lowerLen);
				pfree(lower);
				buf[lowerLen + 1] = EMPTY_CHARACTER;

				callback(buf, buf + lowerLen + 2, userData);

				firstExtractable = NULL;
			}
		}
	}
	if (firstExtractable)
	{
		lower = str_tolower(firstExtractable, p - firstExtractable, DEFAULT_COLLATION_OID);
		lowerLen = strlen(lower);
		memcpy(buf + 1, lower, lowerLen);
		pfree(lower);
		buf[lowerLen + 1] = EMPTY_CHARACTER;

		callback(buf, buf + lowerLen + 2, userData);
	}
	pfree(buf);
}

typedef struct
{
	Datum	   *vgrams;
	int			count;
} VGramsInfo;

static void
addVGram(char *vgram, void *userData)
{
	VGramsInfo *vgramsInfo = (VGramsInfo *) userData;

	vgramsInfo->vgrams[vgramsInfo->count++] = PointerGetDatum(cstring_to_text(vgram));
}

static VGramOptions *
makeOptions(int minQ, int maxQ, ArrayType *vgrams)
{
	Size		size;
	VGramOptions *options;

	size = vgrams_fill(vgrams, NULL);
	options = (VGramOptions *) palloc(offsetof(VGramOptions, vgramsCount) + size);
	options->minQ = minQ;
	options->maxQ = maxQ;
	options->vgramsOffset = offsetof(VGramOptions, vgramsCount);
	(void) vgrams_fill(vgrams, &options->vgramsCount);

	return options;
}

Datum
get_vgrams(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_PP(0);
	int			minQ = PG_GETARG_INT32(1);
	int			maxQ = PG_GETARG_INT32(2);
	ArrayType  *vgrams = PG_GETARG_ARRAYTYPE_P(3);
	ExtractVGramsInfo userData;
	VGramOptions *options = makeOptions(minQ, maxQ, vgrams);
	VGramsInfo	vgramsInfo;

	vgramsInfo.vgrams = (Datum *) palloc(sizeof(Datum) * VARSIZE_ANY_EXHDR(s) * (maxQ - minQ + 1));
	vgramsInfo.count = 0;

	userData.callback = addVGram;
	userData.options = options;
	userData.userData = &vgramsInfo;
	extractWords(VARDATA_ANY(s), VARSIZE_ANY_EXHDR(s), extractMinimalVGramsWord, &userData);

	PG_RETURN_ARRAYTYPE_P(construct_array(vgramsInfo.vgrams,
										  vgramsInfo.count,
										  TEXTOID,
										  -1,
										  false,
										  'i'
										  ));
}

uint32
qgram_key_hash(const void *key, Size keysize)
{
	const QGramHashKey *qgramKey = (const QGramHashKey *) key;
	Size		len = strlen(qgramKey->qgram);

	return DatumGetUInt32(hash_any((const unsigned char *) qgramKey->qgram,
								   (int) len));
}

int
qgram_key_match(const void *key1, const void *key2, Size keysize)
{
	const QGramHashKey *qgramKey1 = (const QGramHashKey *) key1;
	const QGramHashKey *qgramKey2 = (const QGramHashKey *) key2;

	return strcmp(qgramKey1->qgram, qgramKey2->qgram);
}

void
qgram_state_cleanup(QGramStatState *state)
{
	ListCell   *lc;

	foreach(lc, state->incrementedQGrams)
	{
		QGramHashKey key;
		QGramHashValue *value;
		bool		found;
		char	   *qgram = (char *) lfirst(lc);

		key.qgram = qgram;
		value = (QGramHashValue *) hash_search(state->qgramsHash,
											   (const void *) &key,
											   HASH_FIND,
											   &found);
		Assert(value && found);
		value->incremented = false;
	}

	list_free(state->incrementedQGrams);
	state->incrementedQGrams = NIL;
}

Datum
qgram_stat_transfn(PG_FUNCTION_ARGS)
{
	MemoryContext oldcontext;
	QGramStatState *state;
	HASHCTL		qgramsHashCtl;

	state = PG_ARGISNULL(0) ? NULL : (QGramStatState *) PG_GETARG_POINTER(0);

	if (state == NULL)
	{
		MemoryContext aggcontext;

		/* First time through --- initialize */
		if (!AggCheckCallContext(fcinfo, &aggcontext))
		{
			/* cannot be called directly because of internal-type argument */
			elog(ERROR, "array_agg_transfn called in non-aggregate context");
		}

		state = (QGramStatState *) MemoryContextAllocZero(aggcontext, sizeof(QGramStatState));
		state->minQ = PG_GETARG_INT32(2);
		state->maxQ = PG_GETARG_INT32(3);
		state->threshold = PG_GETARG_FLOAT8(4);
		state->context = aggcontext;
		state->incrementedQGrams = NIL;
		oldcontext = MemoryContextSwitchTo(state->context);

		qgramsHashCtl.keysize = sizeof(QGramHashKey);
		qgramsHashCtl.entrysize = sizeof(QGramHashValue);
		qgramsHashCtl.hcxt = state->context;
		qgramsHashCtl.hash = qgram_key_hash;
		qgramsHashCtl.match = qgram_key_match;
		state->qgramsHash = hash_create("qgrams hash",
										1024,
										&qgramsHashCtl,
										HASH_ELEM | HASH_CONTEXT
										| HASH_FUNCTION | HASH_COMPARE);

	}
	else
	{
		oldcontext = MemoryContextSwitchTo(state->context);
		state->totalCount++;
	}

	if (!PG_ARGISNULL(1))
	{
		text	   *s = PG_GETARG_TEXT_PP(1);

		extractWords(VARDATA_ANY(s), VARSIZE_ANY_EXHDR(s), collectStatsWord, state);
		qgram_state_cleanup(state);
	}


	MemoryContextSwitchTo(oldcontext);
	PG_RETURN_POINTER(state);
}

Datum
qgram_stat_finalfn(PG_FUNCTION_ARGS)
{
	QGramStatState *state;
	int			limitCount;
	HASH_SEQ_STATUS scanStatus;
	QGramHashValue *item;
	Datum	   *qgrams;
	int			qgramsCount = 0;
	int			i;

	state = PG_ARGISNULL(0) ? NULL : (QGramStatState *) PG_GETARG_POINTER(0);

	if (!state)
		PG_RETURN_NULL();

	limitCount = (int) (state->totalCount * state->threshold);

	hash_seq_init(&scanStatus, state->qgramsHash);
	while ((item = (QGramHashValue *) hash_seq_search(&scanStatus)) != NULL)
	{
		if (item->count >= limitCount)
			qgramsCount++;
	}

	qgrams = (Datum *) palloc(sizeof(Datum) * qgramsCount);

	hash_seq_init(&scanStatus, state->qgramsHash);
	i = 0;
	while ((item = (QGramHashValue *) hash_seq_search(&scanStatus)) != NULL)
	{
		if (item->count >= limitCount)
		{
			Assert(i < qgramsCount);
			qgrams[i] = PointerGetDatum(cstring_to_text(item->key.qgram));
			i++;
		}
	}
	Assert(i == qgramsCount);

	qsort(qgrams, qgramsCount, sizeof(Datum), vgram_sort_cmp);

	hash_destroy(state->qgramsHash);
	PG_RETURN_ARRAYTYPE_P(construct_array(qgrams, qgramsCount, TEXTOID, -1, false, TYPALIGN_INT));
}

Datum
vgram_text_like(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall2Coll(textlike,
											PG_GET_COLLATION(),
											PG_GETARG_DATUM(0),
											PG_GETARG_DATUM(1)));
}

Datum
vgram_text_iclike(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall2Coll(texticlike,
											PG_GET_COLLATION(),
											PG_GETARG_DATUM(0),
											PG_GETARG_DATUM(1)));
}
