-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION vgram" to load this file. \quit

CREATE TABLE qgram_stat
(
	qgram text,
	frequency float4
);

CREATE FUNCTION print_qgrams(text)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION get_vgrams(text)
RETURNS text[]
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION print_qgram_stat()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION qgram_stat_reset_cache()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE OR REPLACE FUNCTION qgram_stat_transfn(internal, text)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION qgram_stat_finalfn(internal)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE;

CREATE AGGREGATE qgram_stat(text) (
	SFUNC = qgram_stat_transfn,
	STYPE = internal,
	FINALFUNC = qgram_stat_finalfn
);

-- support functions for gin
CREATE FUNCTION vgram_cmp(text, text)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION vgram_gin_extract_value(text, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION vgram_gin_extract_query(text, internal, int2, internal, internal, internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION vgram_gin_consistent(internal, int2, text, int4, internal, internal, internal, internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION vgram_gin_triconsistent(internal, int2, text, int4, internal, internal, internal)
RETURNS "char"
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR CLASS vgram_gin_ops
FOR TYPE text USING gin
AS
		OPERATOR		3		pg_catalog.~~ (text, text),
		OPERATOR		4		pg_catalog.~~* (text, text),
		FUNCTION		1		vgram_cmp (text, text),
		FUNCTION		2		vgram_gin_extract_value (text, internal),
		FUNCTION		3		vgram_gin_extract_query (text, internal, int2, internal, internal, internal, internal),
		FUNCTION		4		vgram_gin_consistent (internal, int2, text, int4, internal, internal, internal, internal),
		FUNCTION		6		vgram_gin_triconsistent (internal, int2, text, int4, internal, internal, internal),
		STORAGE			text;

