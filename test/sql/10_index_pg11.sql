CREATE TABLE coll (
        id integer,
        val text,
        enfres d_en_fr_es
);

CREATE INDEX coll_idx ON coll (
    id, val, enfres,
    (val COLLATE "zh_TW"),
    (val COLLATE "en_GB" > '')
) WHERE ((val COLLATE "de_DE" > '') OR (val COLLATE "it_IT" > ''));

SELECT c.collname
FROM pg_collation_index_dependencies('coll_idx') as d(o)
JOIN pg_collation c ON d.o = c.oid
ORDER BY c.collname;
