CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra
SRC = src/block.cpp src/main.cpp
TEST_SRC = src/block.cpp tests/test_kvblocks.cpp

kvblocks: $(SRC) src/block.h
	$(CXX) $(CXXFLAGS) -o kvblocks $(SRC)

test_kvblocks: $(TEST_SRC) src/block.h
	$(CXX) $(CXXFLAGS) -o test_kvblocks $(TEST_SRC)

test: test_kvblocks
	./test_kvblocks

clean:
	rm -f kvblocks test_kvblocks

.PHONY: test clean
