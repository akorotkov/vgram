/*-------------------------------------------------------------------------
 *
 * vgram.c
 *		Routines for Q-gram statistics collection and dividing strings
 *		into V-grams.
 *
 * Copyright (c) 2011-2017, Alexander Korotkov
 *
 * IDENTIFICATION
 *	  contrib/vgram/vgram.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "access/hash.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/hsearch.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"

#include "vgram.h"

PG_MODULE_MAGIC;

Datum		print_qgrams(PG_FUNCTION_ARGS);
Datum		get_vgrams(PG_FUNCTION_ARGS);
Datum		qgram_stat_transfn(PG_FUNCTION_ARGS);
Datum		qgram_stat_finalfn(PG_FUNCTION_ARGS);
Datum		print_qgram_stat(PG_FUNCTION_ARGS);
Datum		qgram_stat_reset_cache(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(get_vgrams);
PG_FUNCTION_INFO_V1(print_qgrams);
PG_FUNCTION_INFO_V1(qgram_stat_transfn);
PG_FUNCTION_INFO_V1(qgram_stat_finalfn);
PG_FUNCTION_INFO_V1(qgram_stat_reset_cache);

static int	qgramTableElementCmp(const void *a1, const void *a2);
static int	qgram_key_match(const void *key1, const void *key2, Size keysize);
static uint32 qgram_key_hash(const void *key, Size keysize);
static void addVGram(char *vgram, void *userData);

typedef struct
{
	char	   *qgram;
	float		frequency;
} QGramTableElement;

/*
 * State of q-grams statistics collection.
 */
typedef struct
{
	MemoryContext	context,
					tmpContext;
	HTAB		   *qgramsHash,
				   *stringQGramsHash,
				   *charactersHash;
	int64			totalCount,
					totalLength;
} QGramStatState;

typedef struct
{
	char		   *qgram;
} QGramHashKey;

typedef struct
{
	QGramHashKey	key;
	int64			count;
} QGramHashValue;

bool				qgramTableLoaded = false;
int					qgramTableSize = 0,
					characterTableSize = 0;
QGramTableElement  *qgramTable = NULL,
				   *characterTable = NULL;
float4				avgCharactersCount = 0.0f;

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
prefixQGramSearch(const char *prefix, int len, int *lower, int *upper)
{
	int			mid,
				cmp;

	while (*lower <= *upper)
	{
		mid = (*lower + *upper) / 2;
		cmp = strncmp(qgramTable[mid].qgram, prefix, len);
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
addQGramToHash(char *qgram, HTAB *qgramsHash)
{
	QGramHashKey key;
	QGramHashValue *value;
	bool		found;

	key.qgram = qgram;
	value = (QGramHashValue *) hash_search(qgramsHash,
										   (const void *) &key,
										   HASH_ENTER,
										   &found);
	if (!found)
		value->count = 1;
	else
		value->count++;
}

/**
 * Adds given character to hash
 *
 * @param qgram
 * @param qgramsHash
 */
static void
addCharacterToHash(char *character, HTAB *charactersHash)
{
	QGramHashKey key;
	QGramHashValue *value;
	bool		found;

	key.qgram = character;
	value = (QGramHashValue *) hash_search(charactersHash,
										   (const void *) &key,
										   HASH_ENTER,
										   &found);
	if (!found)
	{
		value->count = 1;
	}
	else
	{
		value->count++;
		pfree(character);
	}
}

/**
 * Collect statistics from distinct word.
 *
 * @param wordStart Pointer to the first character of the word.
 * @param wordEnd Pointer below to the last character of the word.
 * @param userData Pointer to QGramStatState structure.
 */
static void
collectStatsWord(const char *wordStart, const char *wordEnd,
				 void *userData)
{
	QGramStatState *state = (QGramStatState *) userData;
	const char	   *p,
				   *r;
	int				q;

	/* Collect q-grams stat */
	for (q = minQ; q <= maxQ; q++)
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
				qgram = (char *) palloc(size + 1);
				qgram[size] = 0;
				memcpy(qgram, r, size);
				r += pg_mblen(r);

				addQGramToHash(qgram, state->stringQGramsHash);
			}
		}
		while (p < wordEnd);
	}

	/* Collect characters stat */
	p = wordStart + pg_mblen(wordStart);
	while (p < wordEnd)
	{
		int			len = pg_mblen(p);
		char	   *character = (char *) MemoryContextAlloc(state->context, len + 1);

		memcpy(character, p, len);
		character[len] = 0;

		addCharacterToHash(character, state->charactersHash);
		state->totalLength++;

		p += len;
	}
}

static float4
getCharacterFrequency(const char *c, int len)
{
	int			mid,
				cmp,
				lower = 0,
				upper = characterTableSize - 1;

	while (lower <= upper)
	{
		mid = (lower + upper) / 2;
		cmp = strncmp(characterTable[mid].qgram, c, len);
		if (cmp < 0)
		{
			lower = mid + 1;
		}
		else if (cmp > 0)
		{
			upper = mid - 1;
		}
		else
		{
			return characterTable[mid].frequency;
		}
	}
	return DEFAULT_CHARACTER_FREQUENCY;
}

float4
estimateVGramSelectivilty(const char *vgram)
{
	const char *p,
			   *prev = NULL;
	int			len = 0;

	p = vgram;
	while (*p)
	{
		prev = p;
		len++;
		p += pg_mblen(p);
	}

	if (len < minQ)
		elog(ERROR, "Short vgram %s", vgram);
	else if (len == minQ)
	{
		float4		result = 1.0f;

		p = vgram;
		while (*p)
		{
			int		char_len;

			char_len = pg_mblen(p);
			result *= getCharacterFrequency(p, char_len);
			p += char_len;
		}

		return result;
	}
	else
	{
		int			lower = 0,
					upper = qgramTableSize - 1,
					i;

		i = prefixQGramSearch(vgram, prev - vgram, &lower, &upper);
		if (i < 0)
			elog(ERROR, "Corrupted vgram %s", vgram);

		return
			qgramTable[i].frequency * getCharacterFrequency(prev, p - prev);
	}
}

void
extractVGramsWord(const char *wordStart, const char *wordEnd, void *userData)
{
	const char *p = wordStart;
	ExtractVGramsInfo *info = (ExtractVGramsInfo *) userData;

	while (p < wordEnd)
	{
		const char *r = p;
		int			lower = 0,
					upper = qgramTableSize - 1,
					len = 0;

		while (len < maxQ && r < wordEnd)
		{
			r += pg_mblen(r);
			len++;
			if (len >= minQ && prefixQGramSearch(p, r - p, &lower, &upper) < 0)
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
	}
}

void
extractMinimalVGramsWord(const char *wordStart, const char *wordEnd, void *userData)
{
	const char *p = wordStart,
			   *prevR = NULL,
			   *prevP = NULL;
	ExtractVGramsInfo *info = (ExtractVGramsInfo *) userData;

	while (p < wordEnd)
	{
		const char *r = p;
		int			lower = 0,
					upper = qgramTableSize - 1,
					len = 0;

		while (len < maxQ && r < wordEnd)
		{
			r += pg_mblen(r);
			len++;
			if (len >= minQ && prefixQGramSearch(p, r - p, &lower, &upper) < 0)
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
				lower = lowerstr_with_len(firstExtractable, p - firstExtractable);
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
		lower = lowerstr_with_len(firstExtractable, p - firstExtractable);
		lowerLen = strlen(lower);
		memcpy(buf + 1, lower, lowerLen);
		pfree(lower);
		buf[lowerLen + 1] = EMPTY_CHARACTER;

		callback(buf, buf + lowerLen + 2, userData);
	}
	pfree(buf);
}

Datum
print_qgrams(PG_FUNCTION_ARGS)
{
	QGramStatState state;
	text	   *s = PG_GETARG_TEXT_PP(0);
	HASH_SEQ_STATUS scanStatus;
	HASHCTL		qgramsHashCtl;
	QGramHashValue *item;

	state.totalLength = 0;
	qgramsHashCtl.keysize = sizeof(QGramHashKey);
	qgramsHashCtl.entrysize = sizeof(QGramHashValue);
	qgramsHashCtl.hash = qgram_key_hash;
	qgramsHashCtl.match = qgram_key_match;
	state.stringQGramsHash = hash_create("string qgrams hash",
										 1024,
										 &qgramsHashCtl,
								   HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);
	state.charactersHash = hash_create("characters qgrams hash",
									   1024,
									   &qgramsHashCtl,
								   HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);
	state.context = CurrentMemoryContext;

	extractWords(VARDATA_ANY(s), VARSIZE_ANY_EXHDR(s), collectStatsWord, &state);

	hash_seq_init(&scanStatus, state.stringQGramsHash);
	while ((item = (QGramHashValue *) hash_seq_search(&scanStatus)) != NULL)
	{
		elog(NOTICE, "qgram: %s %ld", item->key.qgram, item->count);
	}

	hash_seq_init(&scanStatus, state.charactersHash);
	while ((item = (QGramHashValue *) hash_seq_search(&scanStatus)) != NULL)
	{
		elog(NOTICE, "character: %s %ld", item->key.qgram, item->count);
	}
	elog(NOTICE, "Total length: %ld", state.totalLength);

	PG_RETURN_VOID();
}

typedef struct
{
	Datum	   *vgrams;
	int			count;
}	VGramsInfo;

static void
addVGram(char *vgram, void *userData)
{
	VGramsInfo *vgramsInfo = (VGramsInfo *) userData;

	elog(NOTICE, "%s - %f", vgram, estimateVGramSelectivilty(vgram));
	vgramsInfo->vgrams[vgramsInfo->count++] = PointerGetDatum(cstring_to_text(vgram));
}

Datum
get_vgrams(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_PP(0);
	ExtractVGramsInfo userData;
	VGramsInfo	vgramsInfo;

	vgramsInfo.vgrams = (Datum *) palloc(sizeof(Datum) * VARSIZE_ANY_EXHDR(s) *(maxQ - minQ + 1));
	vgramsInfo.count = 0;

	loadStats();
	userData.callback = addVGram;
	userData.userData = &vgramsInfo;
	extractWords(VARDATA_ANY(s), VARSIZE_ANY_EXHDR(s), extractMinimalVGramsWord, &userData);

	PG_RETURN_ARRAYTYPE_P(
						  construct_array(
										  vgramsInfo.vgrams,
										  vgramsInfo.count,
										  TEXTOID,
										  -1,
										  false,
										  'i'
										  )
		);
}

static uint32
qgram_key_hash(const void *key, Size keysize)
{
	const QGramHashKey *qgramKey = (const QGramHashKey *) key;
	Size		len = strlen(qgramKey->qgram);

	return DatumGetUInt32(hash_any((const unsigned char *) qgramKey->qgram,
								   (int) len));
}

static int
qgram_key_match(const void *key1, const void *key2, Size keysize)
{
	const QGramHashKey *qgramKey1 = (const QGramHashKey *) key1;
	const QGramHashKey *qgramKey2 = (const QGramHashKey *) key2;

	return strcmp(qgramKey1->qgram, qgramKey2->qgram);
}

Datum
qgram_stat_transfn(PG_FUNCTION_ARGS)
{
	MemoryContext	oldcontext;
	QGramStatState *state;
	HASHCTL			qgramsHashCtl;
	HASH_SEQ_STATUS	scanStatus;
	QGramHashValue *item;

	state = PG_ARGISNULL(0) ? NULL : (QGramStatState *) PG_GETARG_POINTER(0);

	if (state == NULL)
	{
		MemoryContext context,
					aggcontext;

		/* First time through --- initialize */
		if (!AggCheckCallContext(fcinfo, &aggcontext))
		{
			/* cannot be called directly because of internal-type argument */
			elog(ERROR, "array_agg_transfn called in non-aggregate context");
		}

		/* Make a temporary context to hold all the junk */
		context = AllocSetContextCreate(aggcontext,
										"qgram_stat result",
										ALLOCSET_DEFAULT_MINSIZE,
										ALLOCSET_DEFAULT_INITSIZE,
										ALLOCSET_DEFAULT_MAXSIZE);
		state = (QGramStatState *) MemoryContextAlloc(context, sizeof(QGramStatState));
		state->tmpContext = AllocSetContextCreate(aggcontext,
												  "qgram_stat result",
												  ALLOCSET_DEFAULT_MINSIZE,
												  ALLOCSET_DEFAULT_INITSIZE,
												  ALLOCSET_DEFAULT_MAXSIZE);
		state->context = context;
		oldcontext = MemoryContextSwitchTo(state->tmpContext);
		state->totalCount = 0;

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
		state->charactersHash = hash_create("letters hash",
											1024,
											&qgramsHashCtl,
											HASH_ELEM | HASH_CONTEXT
											| HASH_FUNCTION | HASH_COMPARE);

	}
	else
	{
		oldcontext = MemoryContextSwitchTo(state->tmpContext);
		state->totalCount++;
	}

	if (!PG_ARGISNULL(1))
	{
		text	   *s = PG_GETARG_TEXT_PP(1);

		qgramsHashCtl.keysize = sizeof(QGramHashKey);
		qgramsHashCtl.entrysize = sizeof(QGramHashValue);
		qgramsHashCtl.hcxt = state->tmpContext;
		qgramsHashCtl.hash = qgram_key_hash;
		qgramsHashCtl.match = qgram_key_match;
		state->stringQGramsHash = hash_create("string qgrams hash",
											  1024,
											  &qgramsHashCtl,
											  HASH_ELEM | HASH_CONTEXT
											  | HASH_FUNCTION | HASH_COMPARE);

		extractWords(VARDATA_ANY(s), VARSIZE_ANY_EXHDR(s), collectStatsWord, state);
		hash_seq_init(&scanStatus, state->stringQGramsHash);
		while ((item = (QGramHashValue *) hash_seq_search(&scanStatus)) != NULL)
		{
			bool		found;
			QGramHashValue *value;

			value = (QGramHashValue *) hash_search(state->qgramsHash,
												   (const void *) &item->key,
												   HASH_ENTER,
												   &found);
			if (!found)
			{
				value->key.qgram = MemoryContextStrdup(state->context, value->key.qgram);
				value->count = 1;
			}
			else
				value->count++;
		}
		hash_destroy(state->stringQGramsHash);
		MemoryContextReset(state->tmpContext);
	}


	MemoryContextSwitchTo(oldcontext);
	PG_RETURN_POINTER(state);
}

static void
freeStats()
{
	int			i;

	if (qgramTable)
	{
		for (i = 0; i < qgramTableSize; i++)
			pfree(qgramTable[i].qgram);
		pfree(qgramTable);
	}
	if (characterTable)
	{
		for (i = 0; i < characterTableSize; i++)
			pfree(characterTable[i].qgram);
		pfree(characterTable);
	}
	qgramTableLoaded = false;
	qgramTable = NULL;
	qgramTableSize = 0;
	characterTable = NULL;
	characterTableSize = 0;
}

Datum
qgram_stat_reset_cache(PG_FUNCTION_ARGS)
{
	freeStats();
	PG_RETURN_VOID();
}

static void
loadTable(char *query, QGramTableElement ** table, int *size)
{
	int			result,
				i;

	result = SPI_execute(query, true, 0);

	if (result != SPI_OK_SELECT)
		elog(ERROR, "Can't read table qgram_stat;");
	if (SPI_tuptable->tupdesc->natts != 2)
		elog(ERROR, "qgram_stat table must have 2 columns.");
	if (SPI_gettypeid(SPI_tuptable->tupdesc, 1) != TEXTOID)
		elog(ERROR, "1st column of qgram_stat table must be text.");
	if (SPI_gettypeid(SPI_tuptable->tupdesc, 2) != FLOAT4OID)
		elog(ERROR, "2nd column of qgram_stat table must be float4.");

	if (SPI_processed == 0)
	{
		*table = NULL;
		*size = 0;
		return;
	}

	*table = MemoryContextAlloc(TopMemoryContext, sizeof(QGramTableElement) * SPI_processed);
	for (i = 0; i < SPI_processed; i++)
	{
		bool		isnullfreq,
					isnullqgram;
		text	   *qgram;
		MemoryContext oldContext;

		qgram = DatumGetTextP(SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnullqgram));
		if (isnullqgram)
			elog(ERROR, "qgram value must be not null.");

		oldContext = MemoryContextSwitchTo(TopMemoryContext);
		(*table)[i].qgram = text_to_cstring(qgram);
		MemoryContextSwitchTo(oldContext);
		(*table)[i].frequency = DatumGetFloat4(SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2, &isnullfreq));
		if (isnullfreq)
			elog(ERROR, "qgram value must be not null.");
	}
	*size = SPI_processed;

	qsort(*table, *size, sizeof(QGramTableElement), qgramTableElementCmp);
}

void
loadStats(void)
{
	int			result;
	bool		isnull;

	if (qgramTableLoaded)
		return;

	SPI_connect();

	loadTable("SELECT * FROM qgram_stat WHERE length(qgram) > 1;",
			  &qgramTable, &qgramTableSize);
	loadTable("SELECT * FROM qgram_stat WHERE length(qgram) = 1;",
			  &characterTable, &characterTableSize);

	result = SPI_execute("SELECT frequency FROM qgram_stat WHERE qgram IS NULL", true, 0);

	if (result != SPI_OK_SELECT)
		elog(ERROR, "Can't read table qgram_stat;");
	if (SPI_tuptable->tupdesc->natts != 1 ||
		SPI_gettypeid(SPI_tuptable->tupdesc, 1) != FLOAT4OID)
		elog(ERROR, "frequency column of qgram_stat table must be float4.");

	if (SPI_processed == 0)
		avgCharactersCount = 25.0f;

	avgCharactersCount = DatumGetFloat4(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));

	SPI_finish();

	qgramTableLoaded = true;
}

static int
qgramTableElementCmp(const void *a1, const void *a2)
{
	const QGramTableElement *e1 = (const QGramTableElement *) a1;
	const QGramTableElement *e2 = (const QGramTableElement *) a2;

	return strcmp(e1->qgram, e2->qgram);
}

Datum
qgram_stat_finalfn(PG_FUNCTION_ARGS)
{
	QGramStatState *state;
	int				limitCount,
					spiResult;
	HASH_SEQ_STATUS scanStatus;
	QGramHashValue *item;
	MemoryContext	oldcontext;
	SPIPlanPtr		plan;
	Oid				argTypes[2] = {TEXTOID, FLOAT4OID};
	Datum			values[2];

	state = PG_ARGISNULL(0) ? NULL : (QGramStatState *) PG_GETARG_POINTER(0);

	if (!state)
		PG_RETURN_NULL();

	oldcontext = MemoryContextSwitchTo(state->context);
	limitCount = (int) (state->totalCount * VGRAM_LIMIT_RATIO);

	SPI_connect();
	spiResult = SPI_execute("TRUNCATE qgram_stat;", false, 0);
	if (spiResult != SPI_OK_UTILITY)
		elog(ERROR, "Error truncating table qgram_stat.");
	plan = SPI_prepare("INSERT INTO qgram_stat (qgram, frequency) VALUES ($1, $2);", 2, argTypes);

	hash_seq_init(&scanStatus, state->qgramsHash);
	while ((item = (QGramHashValue *) hash_seq_search(&scanStatus)) != NULL)
	{
		if (item->count >= limitCount)
		{
			values[0] = PointerGetDatum(cstring_to_text(item->key.qgram));
			values[1] = Float4GetDatum((float) item->count / (float) state->totalCount);
			spiResult = SPI_execute_plan(plan, values, NULL, false, 0);
			if (spiResult != SPI_OK_INSERT)
				elog(ERROR, "Error inserting record into table qgram_stat.");
		}
	}

	hash_seq_init(&scanStatus, state->charactersHash);
	while ((item = (QGramHashValue *) hash_seq_search(&scanStatus)) != NULL)
	{
		if (item->count >= limitCount)
		{
			values[0] = PointerGetDatum(cstring_to_text(item->key.qgram));
			values[1] = Float4GetDatum((float) item->count / (float) state->totalLength);
			spiResult = SPI_execute_plan(plan, values, NULL, false, 0);
			if (spiResult != SPI_OK_INSERT)
				elog(ERROR, "Error inserting record into table qgram_stat.");
		}
	}

	values[1] = Float4GetDatum((float) state->totalLength / (float) state->totalCount);
	spiResult = SPI_execute_plan(plan, values, "n ", false, 0);
	if (spiResult != SPI_OK_INSERT)
		elog(ERROR, "Error inserting record into table qgram_stat.");

	SPI_finish();

	hash_destroy(state->qgramsHash);
	hash_destroy(state->charactersHash);
	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(state->tmpContext);
	MemoryContextDelete(state->context);
	freeStats();
	PG_RETURN_NULL();
}

Datum
print_qgram_stat(PG_FUNCTION_ARGS)
{
	int			i;

	loadStats();

	for (i = 0; i < qgramTableSize; i++)
		elog(NOTICE, "qgram %s, %f", qgramTable[i].qgram, qgramTable[i].frequency);
	for (i = 0; i < characterTableSize; i++)
		elog(NOTICE, "character %s, %f", characterTable[i].qgram, characterTable[i].frequency);
	elog(NOTICE, "average characters %f", avgCharactersCount);
	PG_RETURN_VOID();
}
