-- This program is open source, licensed under the PostgreSQL License.
-- For license terms, see the LICENSE file.
--
-- Copyright (C) 2022: Julien Rouhaud

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_collation_dependencies" to load this file. \quit

CREATE FUNCTION pg_collation_constraint_dependencies(
        IN conoid oid, OUT colloid oid
    )
    RETURNS SETOF oid
    LANGUAGE C STRICT VOLATILE COST 100
AS '$libdir/pg_collation_dependencies', 'pg_collation_constraint_dependencies';

CREATE FUNCTION pg_collation_index_dependencies(
        IN indexid regclass, OUT colloid oid
    )
    RETURNS SETOF oid
    LANGUAGE C STRICT VOLATILE COST 100
AS '$libdir/pg_collation_dependencies', 'pg_collation_index_dependencies';
