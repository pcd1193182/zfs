sbin_PROGRAMS = target/@RUSTDIR@/zfs_object_agent

target_@RUSTDIR@_zfs_object_agent_SOURCES = \
	src/client.rs \
	src/main.rs \
	src/object_access.rs \
	src/object_based_log.rs \
	src/pool.rs \
	src/server.rs

.PHONY: target/@RUSTDIR@/zfs_object_agent$(EXEEXT)

target/@RUSTDIR@/zfs_object_agent$(EXEEXT):
	cargo build $(RUSTFLAGS)

all: build

build:
	cargo build $(RUSTFLAGS)
