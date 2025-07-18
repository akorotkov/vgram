/*-------------------------------------------------------------------------
 *
 * vgram.h
 *		Header for vgram module.
 *
 * Copyright (c) 2011-2017, Alexander Korotkov
 *
 * IDENTIFICATION
 *	  contrib/vgram/vgram.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _V_GRAM_H_
#define _V_GRAM_H_

#include "tsearch/ts_locale.h"
#include "utils/array.h"

/*
 * V-gram parameters
 */
#define isExtractable(c)			(isalpha((unsigned char) *(c)) || isdigit((unsigned char) *(c)))
/*#define VGRAM_LIMIT_RATIO			(0.005)
#define DEFAULT_CHARACTER_FREQUENCY	(0.001)*/
#define MAX_Q_LIMIT					(10)
#define EMPTY_CHARACTER				('$')

/* strategy numbers */
#define LikeStrategyNumber			3
#define ILikeStrategyNumber			4

typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int		minQ;
	int		maxQ;
	int		vgramsOffset;
	int		vgramsCount;
	int		vgramOffsets[FLEXIBLE_ARRAY_MEMBER];
} VGramOptions;

#define GET_VGRAM(opts, i) ((Pointer) (opts) + (opts)->vgramOffsets[(i)] + offsetof(VGramOptions, vgramsCount))

typedef void (*WordCallback) (const char *wordStart, const char *wordEnd, void *userData);
typedef void (*VGramCallBack) (char *vgram, void *userData);

typedef struct
{
	VGramCallBack	callback;
	VGramOptions   *options;
	void		   *userData;
} ExtractVGramsInfo;

extern Size vgrams_fill(ArrayType *arr, void *ptr);
extern int vgram_sort_cmp(const void *v1, const void *v2);
extern void extractMinimalVGramsWord(const char *wordStart, const char *wordEnd, void *userData);
extern void extractWords(const char *string, size_t len, WordCallback callback, void *userData);
extern void extractVGramsWord(const char *wordStart, const char *wordEnd, void *userData);
extern Datum *extractQueryLike(VGramOptions *options, int32 *nentries, text *pattern);

#endif /* _V_GRAM_H_ */
