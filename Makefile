ROOT_DIR := $(CURDIR)
BUILD_DIR := $(ROOT_DIR)/build
TARGET := $(BUILD_DIR)/dppd

CC ?= gcc
MODE ?= auto

SRC := \
	app/main.c \
	app/ctrl.c \
	app/port_init.c \
	app/worker.c \
	app/stats.c \
	lib/common/log.c \
	lib/common/csum.c \
	lib/common/rss_conf.c \
	lib/pkt/parser.c \
	lib/pkt/pkt_rewrite.c \
	lib/pkt/arp_table.c \
	lib/pkt/tx_offload.c \
	lib/flow/pipeline_fwd.c \
	lib/flow/route_lpm.c \
	lib/flow/acl_rule.c \
	lib/flow/nat_session.c \
	lib/offload/core/backend.c \
	lib/offload/soft/offload_soft.c \
	lib/offload/rte_flow/offload_rte_flow.c \
	lib/offload/rte_flow/rte_flow_map.c \
	lib/offload/hw/offload_hw.c \
	lib/representor/representor.c \
	lib/switch/transfer_pipeline.c

INCLUDES := -Iinclude -Iapp -Ilib/common -Ilib/pkt -Ilib/flow -Ilib/offload/core -Ilib/representor -Ilib/switch
BASE_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O2 $(INCLUDES)
BASE_LDFLAGS :=

PKG_DPDPK := $(shell pkg-config --exists libdpdk 2>/dev/null && echo yes || echo no)

ifeq ($(MODE),dpdk)
  ifneq ($(PKG_DPDPK),yes)
    $(error MODE=dpdk was requested but libdpdk was not found via pkg-config)
  endif
  CFLAGS := $(BASE_CFLAGS) -DDPPD_HAS_DPDK=1 $(shell pkg-config --cflags libdpdk)
  LDFLAGS := $(BASE_LDFLAGS) $(shell pkg-config --libs libdpdk)
else ifeq ($(MODE),mock)
  CFLAGS := $(BASE_CFLAGS) -DDPPD_HAS_DPDK=0
  LDFLAGS := $(BASE_LDFLAGS)
else
  ifeq ($(PKG_DPDPK),yes)
    CFLAGS := $(BASE_CFLAGS) -DDPPD_HAS_DPDK=1 $(shell pkg-config --cflags libdpdk)
    LDFLAGS := $(BASE_LDFLAGS) $(shell pkg-config --libs libdpdk)
  else
    CFLAGS := $(BASE_CFLAGS) -DDPPD_HAS_DPDK=0
    LDFLAGS := $(BASE_LDFLAGS)
  endif
endif

.PHONY: all clean info

all: $(TARGET)

$(TARGET): $(SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)

info:
	@echo "MODE=$(MODE)"
	@echo "PKG_DPDPK=$(PKG_DPDPK)"
	@echo "CFLAGS=$(CFLAGS)"
	@echo "LDFLAGS=$(LDFLAGS)"

clean:
	rm -rf $(BUILD_DIR)
