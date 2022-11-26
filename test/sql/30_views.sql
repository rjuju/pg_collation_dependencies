SELECT table_name, object_name, collname
FROM pg_collation_index_dependencies
WHERE table_name = 'coll'
AND collname = 'en_GB';

SELECT table_name, object_name, collname
FROM pg_collation_constraint_dependencies
WHERE table_name = 'coll'
AND collname = 'en_GB';

BEGIN;

SELECT table_name, object_name, collname, coll_recorded_version
FROM pg_collation_broken_dependencies;

UPDATE pg_collation SET collversion = 'not_a_version'
WHERE collname IN ('en_GB', 'C', 'POSIX');

SELECT dep_kind, table_name, object_name, collname, coll_recorded_version
FROM pg_collation_broken_dependencies
ORDER BY dep_kind COLLATE "C";

ROLLBACK;
