all : main.o routines.o
	gcc main.o routines.o -O2 -lpthread -lm -o conquest

main.o : main.c routines.h
	gcc -O2 -c main.c

routines.o : routines.c routines.h
	gcc -O2 -c routines.c

.PHONY : clean
clean :
	rm conquest main.o routines.o