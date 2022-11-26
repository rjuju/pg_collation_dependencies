ALTER TABLE coll ADD CONSTRAINT coll_check_constraint
    CHECK ( (val COLLATE "en_GB" > '') OR (enfres > ('', '')) );
ALTER TABLE coll ADD CONSTRAINT coll_unique_constraint
    UNIQUE (val, enfres);

SELECT con.conname, c.collname
FROM pg_constraint con,
LATERAL pg_collation_constraint_dependencies(con.oid) as d(o)
JOIN pg_collation c ON d.o = c.oid
WHERE con.conrelid = 'coll'::regclass
ORDER BY con.conname, c.collname;
