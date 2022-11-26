ALTER TABLE coll ADD af_multi af_multirange;

CREATE INDEX coll_af_multi_idx ON coll (
    af_multi
);

SELECT c.collname
FROM pg_collation_index_dependencies('coll_af_multi_idx') as d(o)
JOIN pg_collation c ON d.o = c.oid
ORDER BY c.collname COLLATE "C";
