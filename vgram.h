#include "tsearch/ts_locale.h"

/*
 * V-gram parameters
 */
#define minQ 2
#define maxQ 5
#define isExtractable(c)	(t_isalpha(c) || t_isdigit(c))
#define VGRAM_LIMIT_RATIO 0.03
#define DEFAULT_CHARACTER_FREQUENCY 0.001
#define EMPTY_CHARACTER '$'

/* strategy numbers */
#define LikeStrategyNumber			3
#define ILikeStrategyNumber			4


typedef void (*WordCallback) (const char *wordStart, const char *wordEnd, void *userData);
typedef void (*VGramCallBack) (char *vgram, void *userData);

typedef struct
{
	VGramCallBack callback;
	void *userData;
} ExtractVGramsInfo;

extern void loadStats(void);
extern float4 estimateVGramSelectivilty(const char *vgram);
extern void extractMinimalVGramsWord(const char *wordStart, const char *wordEnd, void *userData);
extern void extractWords(const char *string, size_t len, WordCallback callback, void *userData);
extern void extractVGramsWord(const char *wordStart, const char *wordEnd, void *userData);
extern Datum *extractQueryLike(int32 *nentries, text *pattern);

