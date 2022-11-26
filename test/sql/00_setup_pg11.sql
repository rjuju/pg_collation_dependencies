CREATE TYPE en_fr AS (en text COLLATE "en_US", fr text COLLATE "fr_FR");
CREATE DOMAIN d_en_fr_es AS en_fr CHECK ((value).en COLLATE "es_ES" > '');

CREATE EXTENSION pg_collation_dependencies;
