CREATE TYPE en_fr AS (en text COLLATE "en-x-icu", fr text COLLATE "fr-x-icu");
CREATE DOMAIN d_en_fr_es AS en_fr CHECK ((value).en COLLATE "es-x-icu" > '');
CREATE TYPE af_range AS RANGE (
    SUBTYPE = text,
    COLLATION = "af-x-icu"
);

CREATE EXTENSION pg_collation_dependencies;
