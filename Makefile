CXX      = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread

LIBS = -lsfml-graphics -lsfml-window -lsfml-audio -lsfml-network -lsfml-system -lX11 -lm

TARGETS = arbiter_bin hip_bin asp_bin

all: clean $(TARGETS)
	@echo Build complete.

arbiter_bin: arbiter/arbiter.cpp
	$(CXX) $(CXXFLAGS) arbiter/*.cpp -o arbiter_bin $(LIBS)

hip_bin: hip/hip.cpp
	$(CXX) $(CXXFLAGS) hip/*.cpp -o hip_bin $(LIBS)

asp_bin: asp/asp.cpp
	$(CXX) $(CXXFLAGS) asp/*.cpp -o asp_bin $(LIBS)

clean:
	rm -f arbiter_bin hip_bin asp_bin

.PHONY: all clean
