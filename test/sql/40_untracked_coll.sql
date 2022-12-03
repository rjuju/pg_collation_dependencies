-- make sure we don't track collations when the index doesn't depend on it
ALTER TABLE coll ADD val_en_us text COLLATE "en_US";
CREATE INDEX coll_collate_1 ON coll (val COLLATE "en_GB");
CREATE INDEX coll_collate_2 ON coll (val_en_us COLLATE "en_GB");

SELECT i.relname, c.collname
FROM pg_catalog.pg_class i,
LATERAL pg_collation_index_dependencies(i.oid) as d(o)
JOIN pg_collation c ON d.o = c.oid
WHERE i.relname LIKE 'coll_collate_%' AND i.relkind = 'i'
ORDER BY i.relname, c.collname;
