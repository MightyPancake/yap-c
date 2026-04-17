CC := gcc
# CC := clang
CFLAGS := -Wall -Wextra -O1
CYAN := \033[96m
PURPLE := \033[94m
GREEN := \033[92m
YELLOW := \033[93m
RESET := \033[0m

RM := rm -fr

debug ?= false
ifeq ($(debug),true)
	CFLAGS += -g -fno-omit-frame-pointer
endif

log := $(debug)
ifeq ($(log),true)
	CFLAGS += -DYAP_LOG
endif

YAP_CFLAGS := $(shell yap --cflags)
TINYCC_DIR := ./tinycc
YAP_C_BACKEND_FLAGS := $(YAP_CFLAGS) -I./include -I$(TINYCC_DIR) $(CFLAGS)
YAP_C_BACKEND_LINK_FLAGS := $(TINYCC_DIR)/libtcc.a -ldl -lm -lpthread
YAP_C_BACKEND_LIB := ./libyap_c.so

.PHONY: default all ready_tcc clean

default: all

all: ready_tcc
	@printf "$(PURPLE)Building yap-c backend$(RESET)\n"
	@printf "$(CYAN)log: $(log)$(RESET)\n"
	@printf "$(CYAN)Building objects$(RESET)\n"
	$(CC) -fPIC $(YAP_C_BACKEND_FLAGS) src/*.c -c
	@printf "$(CYAN)Building shared library$(RESET)\n"
	$(CC) -shared -o $(YAP_C_BACKEND_LIB) ./*.o $(YAP_C_BACKEND_LINK_FLAGS)
	$(RM) ./*.o
	@printf "$(GREEN)Done!$(RESET)\n"

ready_tcc:
	@if [ ! -d "$(TINYCC_DIR)" ]; then \
		printf "$(PURPLE)Fetching TinyCC submodule$(RESET)\n"; \
		git submodule update --init --recursive $(TINYCC_DIR); \
	fi
	@if [ -f "$(TINYCC_DIR)/libtcc.a" ]; then \
		printf "$(CYAN)Using existing local TinyCC build$(RESET)\n"; \
	else \
		printf "$(CYAN)Building local TinyCC (PIC)$(RESET)\n"; \
		if [ ! -f "$(TINYCC_DIR)/config.mak" ]; then \
			printf "$(CYAN)Configuring TinyCC$(RESET)\n"; \
			cd $(TINYCC_DIR) && ./configure; \
		fi; \
		$(MAKE) -C $(TINYCC_DIR) libtcc.a; \
	fi
	@printf "$(GREEN)TinyCC ready$(RESET)\n"

clean:
	@printf "$(YELLOW)Cleaning artifacts$(RESET)\n"
	$(RM) $(YAP_C_BACKEND_LIB) ./*.o
