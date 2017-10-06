CREATE EXTENSION vgram;

CREATE TABLE titles (s text);

\copy titles from 'data/titles.data'

SELECT qgram_stat(s) FROM titles;

SELECT qgram_stat_reset_cache();

SELECT get_vgrams('indexing');
SELECT get_vgrams('annotation');
SELECT get_vgrams('i like it');

CREATE INDEX titles_s_idx ON titles USING gin (s vgram_gin_ops);

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