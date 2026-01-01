CC      ?= cc
AR      ?= ar
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -g -Iinclude

SRC      = src/allocator.c
OBJ      = $(SRC:src/%.c=build/%.o)
LIB_NAME = liballocator.a

.PHONY: all demo test clean

all: demo

build:
	@mkdir -p build

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB_NAME): $(OBJ)
	$(AR) rcs $@ $^

demo: $(LIB_NAME) examples/demo.c
	$(CC) $(CFLAGS) -o $@ examples/demo.c $(LIB_NAME)

test: $(LIB_NAME) tests/basic_test.c
	$(CC) $(CFLAGS) -o tests/basic_test tests/basic_test.c $(LIB_NAME)
	./tests/basic_test

clean:
	rm -rf build $(LIB_NAME) demo tests/basic_test
