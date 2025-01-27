# Generated by configure from .in at Sun Jan 23 19:29:20 CET 2005

CC	= gcc
LD	= gcc

CFLAGS	= -g -Wall 
CFLAGS	+= -I. 
LDFLAGS	= libinterrupt.a
HEADERS = mythread.h queue.h



OBJS	= RRFN.o queue.o 

LIBS	= -lm -lrt

SRCS	= $(patsubst %.o,%.c,$(OBJS))

PRGS	= main

all: libinterrupt.a $(PRGS)

libinterrupt.a: interrupt.o
	ar -rv libinterrupt.a interrupt.o

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $*.c $(INCLUDE) -o $@ $(LIBS)

$(PRGS): $(OBJS)
$(PRGS): $(LIBS)
$(PRGS): % : %.o
	$(CC) $(CFLAGS) -o $@ $< $(OBJS) $(LDFLAGS) $(LIBS)

clean:
	-rm -f *.o *.a *~ $(PRGS)

