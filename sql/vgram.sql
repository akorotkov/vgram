CREATE EXTENSION vgram;

CREATE TABLE titles (s text);

\copy titles from 'data/titles.data'

SELECT qgram_stat(s, 2, 4, 0.05) AS vgrams FROM titles;
\gset

SELECT get_vgrams('indexing', 2, 4, :'vgrams');
SELECT get_vgrams('annotation', 2, 4, :'vgrams');
SELECT get_vgrams('i like it', 2, 4, :'vgrams');

CREATE INDEX titles_s_idx ON titles USING gin (s vgram_gin_ops (minq=-1, maxq=2));
CREATE INDEX titles_s_idx ON titles USING gin (s vgram_gin_ops (minq=2, maxq=11));
CREATE INDEX titles_s_idx ON titles USING gin (s vgram_gin_ops (minq=4, maxq=2));
CREATE INDEX titles_s_idx ON titles USING gin (s vgram_gin_ops (minq=2, maxq=4, vgrams=:'vgrams'));

SET enable_seqscan = ON;
SET enable_bitmapscan = OFF;

SELECT * FROM titles WHERE s ilike '%indexing%';
SELECT * FROM titles WHERE s ilike '%annotation%';

SET enable_seqscan = OFF;
SET enable_bitmapscan = ON;

EXPLAIN (COSTS OFF) SELECT * FROM titles WHERE s ilike '%indexing%';
EXPLAIN (COSTS OFF) SELECT * FROM titles WHERE s ilike '%annotation%';
SELECT * FROM titles WHERE s ilike '%indexing%';
SELECT * FROM titles WHERE s ilike '%annotation%';

DROP INDEX titles_s_idx;
CREATE INDEX titles_s_idx ON titles USING gin (s vgram_gin_ops (minq=3, maxq=3));

EXPLAIN (COSTS OFF) SELECT * FROM titles WHERE s ilike '%indexing%';
EXPLAIN (COSTS OFF) SELECT * FROM titles WHERE s ilike '%annotation%';
SELECT * FROM titles WHERE s ilike '%indexing%';
SELECT * FROM titles WHERE s ilike '%annotation%';

DROP INDEX titles_s_idx;
ALTER TABLE titles ALTER COLUMN s TYPE vgram_text USING s::vgram_text;
ANALYZE titles;
CREATE INDEX titles_s_idx ON titles USING gin (s vgram_gin_ops2 (minq=2, maxq=4, vgrams=:'vgrams'));

EXPLAIN (COSTS OFF) SELECT * FROM titles WHERE s ilike '%indexing%';
EXPLAIN (COSTS OFF) SELECT * FROM titles WHERE s ilike '%annotation%';
SELECT * FROM titles WHERE s ilike '%indexing%';
SELECT * FROM titles WHERE s ilike '%annotation%';
