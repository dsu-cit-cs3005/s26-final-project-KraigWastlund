CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -pedantic

all: inClassDemo

inClassDemo: main.cpp Arena.cpp Arena.h
	$(CXX) $(CXXFLAGS) main.cpp Arena.cpp -o inClassDemo

clean:
	rm -f inClassDemo
