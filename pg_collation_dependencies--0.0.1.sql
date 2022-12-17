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

CREATE FUNCTION pg_collation_matview_dependencies(
        IN matviewid regclass, OUT colloid oid
    )
    RETURNS SETOF oid
    LANGUAGE C STRICT VOLATILE COST 100
AS '$libdir/pg_collation_dependencies', 'pg_collation_matview_dependencies';

CREATE VIEW pg_collation_index_dependencies AS
    SELECT tc.oid AS tbl_oid, tc.oid::regclass::name AS table_name,
          ic.oid AS index_oid, ic.oid::regclass::name AS index_name,
          coll.oid AS coll_oid, coll.collname
    FROM pg_catalog.pg_class tc
    JOIN pg_catalog.pg_index i ON i.indrelid = tc.oid
    JOIN pg_catalog.pg_class ic ON ic.oid = i.indexrelid,
    LATERAL pg_collation_index_dependencies(ic.oid) d(colloid)
    JOIN pg_catalog.pg_collation coll ON coll.oid = d.colloid;

CREATE VIEW pg_collation_constraint_dependencies AS
    SELECT c.oid AS tbl_oid, c.oid::regclass::name AS table_name,
          con.oid AS constraint_oid, quote_ident(n.nspname) || '.' ||
            quote_ident(con.conname) AS constraint_name,
          coll.oid AS coll_oid, coll.collname
    FROM pg_catalog.pg_constraint con
    JOIN pg_catalog.pg_namespace n ON n.oid = con.connamespace
    JOIN pg_catalog.pg_class c ON c.oid = con.conrelid,
    LATERAL pg_collation_constraint_dependencies(con.oid) d(colloid)
    JOIN pg_catalog.pg_collation coll ON coll.oid = d.colloid;

CREATE VIEW pg_collation_matview_dependencies AS
    SELECT c.oid AS matview_oid, c.oid::regclass::name AS matview_name,
          coll.oid AS coll_oid, coll.collname
    FROM pg_catalog.pg_class c,
    LATERAL pg_collation_matview_dependencies(c.oid) d(colloid)
    JOIN pg_catalog.pg_collation coll ON coll.oid = d.colloid
    WHERE relkind = 'm';

CREATE VIEW pg_collation_broken_dependencies AS
    SELECT s.*,
        coll.collversion AS coll_recorded_version,
        pg_collation_actual_version(coll.oid) AS coll_actual_version
    FROM (
        SELECT 'index' AS dep_kind, tbl_oid, table_name,
            index_oid AS object_oid, index_name AS object_name,
            coll_oid, collname
        FROM pg_collation_index_dependencies
        UNION ALL
        SELECT 'constraint', * FROM pg_collation_constraint_dependencies
        UNION ALL
        SELECT 'materialized view', NULL, NULL, * FROM pg_collation_matview_dependencies
    ) s
    JOIN pg_catalog.pg_collation coll ON coll.oid = s.coll_oid
    WHERE coll.collversion IS DISTINCT FROM pg_collation_actual_version(coll.oid)
    AND NOT (
        coll.collnamespace = 'pg_catalog'::regnamespace
        AND collencoding = -1
        AND coll.collname IN ('C', 'POSIX')
    );
