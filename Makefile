
# use these for profiling
#CXXFLAGS += -Wall -pg
#LDFLAGS += -lcurl -lm -lssl -pg

CXXFLAGS += -Wall -O3
LDFLAGS += -lcurl -lm -lssl
LD = g++
CC = g++
CXX = g++

all: testclient testmd5 extractbytes

testclient: testclient.o options.o

testdns: testdns.o

testmd5: testmd5.o

extractbytes: extractbytes.o

clean:
	rm -f *~ gmon.out *.o testclient testdns testmd5 extractbytes