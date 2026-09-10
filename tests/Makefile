# Host unit tests (no Arduino / ESP toolchain).
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -I../include
SRC       = test_session.cpp ../src/session.cpp
BIN       = test_session

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRC) ../include/session.h ../include/sw3518_scale.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

test: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
