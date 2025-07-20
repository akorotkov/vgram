/*-------------------------------------------------------------------------
 *
 * vgram_selfunc.c
 *	  Selectivity estimation for LIKE/ILIKE operator over vgram_text
 *	  columns.
 *
 * Portions Copyright (c) 2025, Alexander Korotkov
 *
 * IDENTIFICATION
 *	  contrib/vgram/vgram_selfunc.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type_d.h"
#include "common/hashfn.h"
#include "optimizer/optimizer.h"
#include "utils/formatting.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"

#include "varatt.h"
#include "vgram.h"

#define DEFAULT_LIKE_SEL 0.05
#define MAX_STAT_Q	(3)

typedef struct
{
	const char *qgram;
	int			qgramLen;
} QGramKey;

typedef struct
{
	QGramKey	key;
	float4		frequency;
} QGramFreq;

typedef struct
{
	QGramFreq  *lookup;
	int			lookupLen;
	float4		minfreq;
} StatData;

Datum		vgram_likesel(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(vgram_likesel);

static float4 estimate_like_sel(StatData *statData, const char *s,
								int len, Oid collation);
static float4 estimate_like_fragment_sel(StatData *statData,
										 const char *s, int len);
static float4 lookup_qgram(StatData *statData, const char *qgram,
						   int len, bool *found);
static int qgram_key_cmp_internal(const void *e1, const void *e2);

Datum
vgram_likesel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			operator = PG_GETARG_OID(1);
#endif
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Selectivity selec;
	Form_pg_statistic stats;
	AttStatsSlot sslot;
	int			i;
	StatData	statData;
	text	   *pattern;

	/*
	 * If expression is not variable = something or something = variable, then
	 * punt and return a default estimate.
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(DEFAULT_LIKE_SEL);

	/*
	 * Can't do anything useful if the something is not a constant, either.
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_LIKE_SEL);
	}

	/*
	 * The "~~" and "~~*"" operators are strict, so we can cope with NULL
	 * right away.
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}

	if (((Const *) other)->consttype != TEXTOID ||
		!HeapTupleIsValid(vardata.statsTuple))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_LIKE_SEL);
	}

	pattern = DatumGetTextPP(((Const *) other)->constvalue);
	stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);

	/* MCELEM will be an array of TEXT elements for a tsvector column */
	if (!get_attstatsslot(&sslot, vardata.statsTuple,
						  STATISTIC_KIND_MCELEM, InvalidOid,
						  ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS) ||
		sslot.nnumbers != sslot.nvalues + 2)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_LIKE_SEL);
	}

	statData.lookup = (QGramFreq *) palloc(sizeof(QGramFreq) * sslot.nvalues);
	statData.lookupLen = sslot.nvalues;
	for (i = 0; i < sslot.nvalues; i++)
	{
		text   *value = DatumGetTextPP(sslot.values[i]);

		Assert(!VARATT_IS_COMPRESSED(value) && !VARATT_IS_EXTERNAL(value));
		statData.lookup[i].key.qgram = VARDATA_ANY(value);
		statData.lookup[i].key.qgramLen = VARSIZE_ANY_EXHDR(value);
		statData.lookup[i].frequency = sslot.numbers[i];
	}

	/* Grab the lowest frequency. */
	statData.minfreq = sslot.numbers[sslot.nnumbers - 2];

	selec = estimate_like_sel(&statData,
							  VARDATA_ANY(pattern),
							  VARSIZE_ANY_EXHDR(pattern),
							  PG_GET_COLLATION());

	selec *= (1.0 - stats->stanullfrac);

	free_attstatsslot(&sslot);
	ReleaseVariableStats(vardata);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}

static float4
estimate_like_sel(StatData *statData, const char *s, int len, Oid collation)
{
	char	   *buf,
			   *buf2;
	const char *eword;
	float4		result = 1.0f;
	int			bytelen,
				charlen;

	buf = (char *) palloc(len + 3);
	eword = s;
	while ((eword = get_wildcard_part(eword, len - (eword - s),
									  buf, &bytelen, &charlen)) != NULL)
	{
		buf2 = str_tolower(buf, bytelen, collation);
		bytelen = strlen(buf2);

		result *= estimate_like_fragment_sel(statData, buf, bytelen);

		pfree(buf2);
	}
	pfree(buf);

	return result;
}

static float4
estimate_like_fragment_sel(StatData *statData, const char *s, int len)
{
	float4		result;
	int			charLen = pg_mbstrlen_with_len(s, len);
	int			i;
	const char *p, *q;

	if (charLen <= MAX_STAT_Q)
	{
		result = lookup_qgram(statData, s, len, NULL);
		elog(DEBUG3, "estimate_like_fragment_sel(): %.*s, %f",
			 (int) len, s, result);
		return result;
	}

	q = p = s;
	for (i = 0; i < MAX_STAT_Q; i++)
		q += pg_mblen(q);
	result = lookup_qgram(statData, p, q - p, NULL);
	elog(DEBUG3, "estimate_like_fragment_sel(): %.*s, %f",
		 (int) (q - p), p, result);

	while (q < s + len)
	{
		bool		found;
		float4		numerator;
		float4		denominator;
		const char *pp;

		p += pg_mblen(p);

		pp = p;
		denominator = lookup_qgram(statData, pp, q - pp, &found);
		while (!found)
		{
			pp += pg_mblen(pp);
			if (pp >= q)
			{
				Assert(pp == q);
				denominator = 1.0f;
				break;
			}
			denominator = lookup_qgram(statData, pp, q - pp, &found);
		}
		elog(DEBUG3, "estimate_like_fragment_sel(): denominator %.*s, %f",
			 (int) (q - pp), pp, denominator);

		q += pg_mblen(q);
		numerator = lookup_qgram(statData, pp, q - pp, NULL);
		elog(DEBUG3, "estimate_like_fragment_sel(): numerator %.*s, %f",
			 (int) (q - pp), pp, numerator);
		result *= numerator / denominator;
	}

	return result;
}

static float4
lookup_qgram(StatData *statData, const char *qgram, int len, bool *found)
{
	QGramKey	key;
	QGramFreq  *searchres;

	key.qgram = qgram;
	key.qgramLen = len;

	searchres = (QGramFreq *) bsearch(&key,
									  statData->lookup, statData->lookupLen,
									  sizeof(QGramFreq),
									  qgram_key_cmp_internal);

	if (found)
		*found = (searchres != NULL);

	if (searchres)
		return searchres->frequency;
	else
		return statData->minfreq * 0.5f;

}

static int
qgram_key_cmp_internal(const void *e1, const void *e2)
{
	QGramKey   *key1 = (QGramKey *) e1;
	QGramKey   *key2 = (QGramKey *) e2;
	int			cmp;

	cmp = memcmp(key1->qgram,
				 key2->qgram,
				 Min(key1->qgramLen, key2->qgramLen));

	if (cmp != 0)
		return cmp;
	if (key1->qgramLen < key2->qgramLen)
		return -1;
	else if (key1->qgramLen == key2->qgramLen)
		return 0;
	else
		return 1;
}
