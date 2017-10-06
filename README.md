vgram - prototype of V-gram indexing for PostgreSQL
===================================================

Parameters
----------

Parameters of V-gram extraction are specified in vgram.h.

 * `minQ` – minimal length of V-gram
 * `maxQ` – maximal length of V-gram
 * `isExtractable(c)` – function which checks if given character could be
   extracted into V-gram (i.e. is part of word)
 * `VGRAM_LIMIT_RATIO` – maximal frequency of V-gram to be extracted.  If V-gram
   is more frequent, then longer V-grams are extracted instead.
 * `DEFAULT_CHARACTER_FREQUENCY` – default selectivity of character for
   selectivity estimation (???)
 * `EMPTY_CHARACTER` – character to be used to mark words boundaries

Collecting V-gram statistics
----------------------------

Aggregate function `qgram_stat(text)` collects V-gram frequency statistics and
stores it into `qgram_stat` table.  Statistics is small, but its collecting
is slow and resource consumption process.  This is why, it makes sense to
collect statistics using small random sample of your big dataset.

See following example to collect V-gram statistics using `dblp_titles.s` column.
If previously collected statistics exists, then `qgram_stat(text)` overrides
it.

```sql
SELECT qgram_stat(s) FROM dblp_titles;
```

Statistics is cached in local memory of backend memory.  Use
`qgram_stat_reset_cache()` to reset statistics.

You can check V-gram extraction using `get_vgrams(text)` function.  NOTICE
prints estimated frequencies of V-grams.

```sql
# SELECT get_vgrams('best string ever');
NOTICE:  $be - 0.017040
NOTICE:  bes - 0.002190
NOTICE:  est$ - 0.005348
NOTICE:  $str - 0.004012
NOTICE:  stri - 0.005604
NOTICE:  trin - 0.003417
NOTICE:  $ev - 0.000388
NOTICE:  eve - 0.006021
NOTICE:  ver$ - 0.007446
                 get_vgrams
--------------------------------------------
 {$be,bes,est$,$str,stri,trin,$ev,eve,ver$}
(1 row)
```

Usage
-----

Once you have statistics collected, you may build GIN index based on V-grams.

```sql
CREATE INDEX dblp_titles_s_idx ON dblp_titles USING gin (s vgram_gin_ops);
```

Then, this index could be used to accelerate like/ilike queries over indexed
column.

```sql
# SELECT * FROM dblp_titles WHERE s LIKE '%supernova%';
    id    |                                                             s
----------+---------------------------------------------------------------------------------------------------------------------------
  8555415 | Enabling lock-free concurrent fine-grain access to massive distributed data: Application to supernovae detection.
  8738163 | Seeking supernovae in the clouds: a performance study.
  9108437 | Workflow management for high volume supernova search.
 10861237 | On the performance of SPAI and ADI-like preconditioners for core collapse supernova simulations in one spatial dimension.
 10862468 | Multidimensional simulations of pair-instability supernovae.
(5 rows)

Time: 2,746 ms
```

Note, that once V-gram statistics is updated, all previously created indexes
are no longer valid!


Author
------

 * Alexander Korotkov <aekorotkov@gmail.com>, Postgres Professional, Moscow, Russia

Availability
------------

vgram is realized as an extension and not available in default PostgreSQL
installation. It is available from
[github](https://github.com/akorotkov/vgram)
under the same license as
[PostgreSQL](https://www.postgresql.org/about/licence/)
and supports PostgreSQL 9.4+.
