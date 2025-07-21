-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION vgram" to load this file. \quit

CREATE FUNCTION print_vgrams(s text, min_q int, max_q int)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION get_vgrams(s text, min_q int, max_q int, vgrams text[])
RETURNS text[]
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE OR REPLACE FUNCTION qgram_stat_transfn(state internal, s text, min_q int, max_q int, threshold float8)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION qgram_stat_finalfn(state internal)
RETURNS text[]
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE;

CREATE AGGREGATE qgram_stat(s text, min_q int, max_q int, threshold float8) (
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

CREATE FUNCTION vgram_gin_options(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

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
		FUNCTION		7		vgram_gin_options (internal),
		STORAGE			text;

CREATE FUNCTION vgram_text_in(cstring)
RETURNS vgram_text
AS 'textin'
LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION vgram_text_out(vgram_text)
RETURNS cstring
AS 'textout'
LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION vgram_text_recv(internal)
RETURNS vgram_text
AS 'textrecv'
LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION vgram_text_send(vgram_text)
RETURNS bytea
AS 'textsend'
LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION vgram_typanalyze(internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE vgram_text (
	INPUT = vgram_text_in,
	OUTPUT = vgram_text_out,
	RECEIVE = vgram_text_recv,
	SEND = vgram_text_send,
	ANALYZE = vgram_typanalyze,
	INTERNALLENGTH = -1,
	ALIGNMENT = int4,
	STORAGE = extended,
	CATEGORY = S,
	PREFERRED = false,
	COLLATABLE = true
);

CREATE CAST (vgram_text AS text) WITHOUT FUNCTION AS IMPLICIT;

CREATE FUNCTION vgram_text_like(vgram_text, text)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION vgram_text_iclike(vgram_text, text)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION vgram_likesel(internal, oid, internal, int4)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR ~~ (
	FUNCTION = vgram_text_like,
	LEFTARG = vgram_text,
	RIGHTARG = text,
	RESTRICT = vgram_likesel,
	JOIN = pg_catalog.likejoinsel
);

CREATE OPERATOR ~~* (
	FUNCTION = vgram_text_iclike,
	LEFTARG = vgram_text,
	RIGHTARG = text,
	RESTRICT = vgram_likesel,
	JOIN = pg_catalog.likejoinsel
);

CREATE FUNCTION vgram_cmp(vgram_text, vgram_text)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION vgram_gin_extract_value(vgram_text, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR CLASS vgram_gin_ops2
FOR TYPE vgram_text USING gin
AS
		OPERATOR		3		~~ (vgram_text, text),
		OPERATOR		4		~~* (vgram_text, text),
		FUNCTION		1		vgram_cmp (vgram_text, vgram_text),
		FUNCTION		2		vgram_gin_extract_value (vgram_text, internal),
		FUNCTION		3		vgram_gin_extract_query (text, internal, int2, internal, internal, internal, internal),
		FUNCTION		4		vgram_gin_consistent (internal, int2, text, int4, internal, internal, internal, internal),
		FUNCTION		6		vgram_gin_triconsistent (internal, int2, text, int4, internal, internal, internal),
		FUNCTION		7		vgram_gin_options (internal),
		STORAGE			text;
