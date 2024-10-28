sbin_PROGRAMS = $(RUSTDIR)/zcreate

__RUSTDIR__zcreate_SOURCES = $(shell $(abs_top_builddir)/cmd/zrust/parse_deps.sh $(RUSTDIR)/zcreate.d)

RUSTFLAGS = \
	\"-L\", \"$(abs_top_builddir)/lib/.libs\"

GITREV = $(shell cat $(abs_top_builddir)/include/zfs_gitrev.h | awk '{print $$3}')

$(RUSTDIR)/zcreate$(EXEEXT): $(__RUSTDIR__zcreate_SOURCES)
	touch .zcreate.build_needed

all-local: $(__RUSTDIR__zcreate_SOURCES)
	@if ls .*.build_needed >/dev/null 2>&1; then \
		CARGO_ZOA_GITREV="$(GITREV)" cargo build --config \
		    "target.x86_64-unknown-linux-gnu.rustflags = [$(RUSTFLAGS)]" \
		    $(RUSTTARGET) $(shell ls .*.build_needed 2>/dev/null | \
		    sed 's/\.\(.*\).build_needed/--package \1/g'); \
		success=$$?; \
		rm .*.build_needed; \
		exit $$success; \
	fi
