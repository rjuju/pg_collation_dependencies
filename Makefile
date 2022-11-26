EXTENSION    = pg_collation_dependencies
EXTVERSION   = 0.0.1
TESTS        = $(wildcard test/sql/*.sql)

# More tests can be added or overridden later, after including pgxs
REGRESS      = 00_setup \
	       10_index
REGRESS_OPTS = --inputdir=test

PG_CONFIG = pg_config

MODULE_big = pg_collation_dependencies
OBJS = pg_collation_dependencies.o

all:

release-zip: all
	git archive --format zip --prefix=pg_collation_dependencies-${EXTVERSION}/ --output ./pg_collation_dependencies-${EXTVERSION}.zip HEAD
	unzip ./pg_collation_dependencies-$(EXTVERSION).zip
	rm ./pg_collation_dependencies-$(EXTVERSION).zip
	rm ./pg_collation_dependencies-$(EXTVERSION)/.gitignore
	sed -i -e "s/__VERSION__/$(EXTVERSION)/g"  ./pg_collation_dependencies-$(EXTVERSION)/META.json
	zip -r ./pg_collation_dependencies-$(EXTVERSION).zip ./pg_collation_dependencies-$(EXTVERSION)/
	rm ./pg_collation_dependencies-$(EXTVERSION) -rf


DATA = $(wildcard *--*.sql)
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# pg11 doesn't support ICU, so use glibc collations only
ifeq ($(MAJORVERSION),11)
	REGRESS = 00_setup_pg11 \
		  10_index_pg11 \
		  20_constraint_pg11
else
ifneq ($(MAJORVERSION),12)
	REGRESS += 11_index_multirange
endif		# pg12+
	REGRESS += 20_constraint
endif		# pg11+

	REGRESS += 30_views
