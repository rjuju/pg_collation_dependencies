pg_collation_dependencies
=========================

PostgreSQL extension to reliably find collation dependencies that may be
corrupted in case of an underlying collation library upgrade.

Two functions are provided to list the full collation dependencies for indexes
and constraints:

* pg_collation_index_dependencies(oid index_oid)
* pg_collation_constraint_dependencies(oid constraint_oid)

And some views to get a the full list of collation dependencies for all
indexes/constraints on the database:

* pg_collation_index_dependencies
* pg_collation_constraint_dependencies

And finally a view listing all objects depending on a collation for which the
version appears to be outdated, thus is likely to be corrupted:

* pg_collation_broken_dependencies

Here's a quick example based on the regression tests:

```
CREATE TYPE en_fr AS (en TEXT COLLATE "en-x-icu", fr TEXT COLLATE "fr-x-icu");
CREATE DOMAIN d_en_fr_es AS en_fr CHECK ((VALUE).en COLLATE "es-x-icu" > '');
CREATE TYPE af_range AS RANGE (
    SUBTYPE = TEXT,
    COLLATION = "af-x-icu"
);
CREATE TYPE af_range AS RANGE (
    SUBTYPE = TEXT,
    COLLATION = "af-x-icu"
);

CREATE TABLE coll (
        id INTEGER,
        val TEXT,
        enfres d_en_fr_es,
        af af_range
);

CREATE INDEX coll_idx ON coll (
    id, val, af, enfres,
    (val COLLATE "ar-x-icu"),
    (val COLLATE "en_GB" > '')
) WHERE ((val COLLATE "de-x-icu" > '') OR (val COLLATE "it_IT" > ''));

ALTER TABLE coll ADD CONSTRAINT coll_check_constraint
    CHECK ( (val COLLATE "en_GB" > '') OR (enfres > ('', '')) );
ALTER TABLE coll ADD CONSTRAINT coll_unique_constraint
    UNIQUE (val, enfres);

CREATE EXTENSION pg_collation_dependencies;

SELECT * FROM pg_collation_index_dependencies WHERE table_name = 'coll';
 tbl_oid | table_name | index_oid |      object_name       | coll_oid | collname
---------+------------+-----------+------------------------+----------+----------
   18633 | coll       |     18639 | coll_af_multi_idx      |      100 | default
   18633 | coll       |     18639 | coll_af_multi_idx      |    12607 | af-x-icu
   18633 | coll       |     18641 | coll_unique_constraint |      100 | default
   18633 | coll       |     18641 | coll_unique_constraint |    12739 | en-x-icu
   18633 | coll       |     18641 | coll_unique_constraint |    12848 | es-x-icu
   18633 | coll       |     18641 | coll_unique_constraint |    12920 | fr-x-icu
   18633 | coll       |     18638 | coll_idx               |      100 | default
   18633 | coll       |     18638 | coll_idx               |    12404 | it_IT
   18633 | coll       |     18638 | coll_idx               |    12456 | en_GB
   18633 | coll       |     18638 | coll_idx               |    12607 | af-x-icu
   18633 | coll       |     18638 | coll_idx               |    12616 | ar-x-icu
   18633 | coll       |     18638 | coll_idx               |    12711 | de-x-icu
   18633 | coll       |     18638 | coll_idx               |    12739 | en-x-icu
   18633 | coll       |     18638 | coll_idx               |    12848 | es-x-icu
   18633 | coll       |     18638 | coll_idx               |    12920 | fr-x-icu
(15 rows)

SELECT * FROM pg_collation_constraint_dependencies WHERE table_name = 'coll';
 tbl_oid | table_name | constraint_oid |          object_name          | coll_oid | collname
---------+------------+----------------+-------------------------------+----------+----------
   18633 | coll       |          18642 | public.coll_unique_constraint |      100 | default
   18633 | coll       |          18642 | public.coll_unique_constraint |    12739 | en-x-icu
   18633 | coll       |          18642 | public.coll_unique_constraint |    12848 | es-x-icu
   18633 | coll       |          18642 | public.coll_unique_constraint |    12920 | fr-x-icu
   18633 | coll       |          18640 | public.coll_check_constraint  |      100 | default
   18633 | coll       |          18640 | public.coll_check_constraint  |    12456 | en_GB
   18633 | coll       |          18640 | public.coll_check_constraint  |    12739 | en-x-icu
   18633 | coll       |          18640 | public.coll_check_constraint  |    12848 | es-x-icu
   18633 | coll       |          18640 | public.coll_check_constraint  |    12920 | fr-x-icu
(9 rows)

BEGIN;
UPDATE pg_collation SET collversion = 'not_a_version' WHERE collname = 'en_GB';

SELECT dep_kind, table_name, object_name, collname,
    coll_recorded_version, coll_actual_version
FROM pg_collation_broken_dependencies
ORDER BY dep_kind;
  dep_kind  | table_name |         object_name          | collname | coll_recorded_version | coll_actual_version
------------+------------+------------------------------+----------+-----------------------+---------------------
 constraint | coll       | public.coll_check_constraint | en_GB    | not_a_version         | 2.36
 index      | coll       | coll_idx                     | en_GB    | not_a_version         | 2.36
(2 rows)
```
