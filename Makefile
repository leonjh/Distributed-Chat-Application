TARGETS = chatserver chatclient

all: $(TARGETS)

%.o: %.cc
	g++ $^ -c -o $@

chatserver: chatserver.o tokenizer.cc
	g++ $^ -o $@

chatclient: chatclient.o tokenizer.cc
	g++ $^ -o $@

pack:
	rm -f submit-hw3.zip
	zip -r submit-hw3.zip README Makefile *.c* *.h*

clean::
	rm -fv $(TARGETS) *~ *.o submit-hw3.zip

realclean:: clean
	rm -fv submit-hw3.zip
