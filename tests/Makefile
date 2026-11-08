# Host-side unit tests. These build the portable EE sources with the host C
# compiler; no PS2SDK is required.
#
#   make -C tests          build and run everything
#   make -C tests SAN=1    same, with ASan/UBSan

CC ?= cc
SRC := ../src
CFLAGS ?= -O2
CFLAGS += -std=gnu99 -Wall -Wextra -Werror -Wno-unused-function \
          -Iinclude -I$(SRC) -I.
ifeq ($(SAN),1)
CFLAGS += -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
          -fno-sanitize-recover=all
LDFLAGS += -fsanitize=address,undefined
endif

BUILD := build
TESTS := test_card_math test_mc_fs_format test_sha256 test_save_title

test_card_math_SRCS := test_card_math.c $(SRC)/card_math.c
test_mc_fs_format_SRCS := test_mc_fs_format.c $(SRC)/mc_fs_format.c
test_sha256_SRCS := test_sha256.c $(SRC)/sha256.c
test_save_title_SRCS := test_save_title.c $(SRC)/save_title.c

.PHONY: all run clean
all: run

$(BUILD):
	mkdir -p $@

define TEST_RULE
$(BUILD)/$(1): $$($(1)_SRCS) test_util.h include/tamtypes.h | $(BUILD)
	$$(CC) $$(CFLAGS) -o $$@ $$($(1)_SRCS) $$(LDFLAGS)
endef
$(foreach t,$(TESTS),$(eval $(call TEST_RULE,$(t))))

run: $(addprefix $(BUILD)/,$(TESTS))
	@set -e; for t in $^; do ./$$t; done

clean:
	rm -rf $(BUILD)
