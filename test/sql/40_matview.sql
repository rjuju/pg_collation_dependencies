CREATE MATERIALIZED VIEW mv_coll AS
    SELECT id, val,
    val COLLATE "fr_FR" || '' as val1,
    (SELECT count(*)
        FROM coll cc
        WHERE cc.val COLLATE "en_US" > ''
    ) AS nb
    FROM coll
    ORDER BY val COLLATE "es_ES";

SELECT c.collname
FROM pg_collation_matview_dependencies('mv_coll') as d(o)
JOIN pg_collation c ON d.o = c.oid
ORDER BY c.collname::text COLLATE "C";
