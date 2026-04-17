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
YAP_C_BACKEND_FLAGS := $(YAP_CFLAGS) -I./include $(CFLAGS) -ltcc
YAP_C_BACKEND_LIB := ./libyap_c.so

TINYCC_DIR := ./tinycc

.PHONY: default all ready_tcc clean

default: all

all: ready_tcc
	@printf "$(PURPLE)Building yap-c backend$(RESET)\n"
	@printf "$(CYAN)log: $(log)$(RESET)\n"
	@printf "$(CYAN)Building objects$(RESET)\n"
	$(CC) -fPIC $(YAP_C_BACKEND_FLAGS) src/*.c -c
	@printf "$(CYAN)Building shared library$(RESET)\n"
	$(CC) -shared -o $(YAP_C_BACKEND_LIB) ./*.o
	$(RM) ./*.o
	@printf "$(GREEN)Done!$(RESET)\n"

ready_tcc:
	@if [ -e "$(TINYCC_DIR)/.git" ]; then \
		printf "$(CYAN)TinyCC submodule already present$(RESET)\n"; \
	else \
		printf "$(PURPLE)Fetching TinyCC submodule$(RESET)\n"; \
		git submodule update --init --recursive --remote $(TINYCC_DIR); \
		printf "$(GREEN)TinyCC ready$(RESET)\n"; \
	fi

clean:
	@printf "$(YELLOW)Cleaning artifacts$(RESET)\n"
	$(RM) $(YAP_C_BACKEND_LIB) ./*.o
