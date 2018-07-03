CC = gcc
CXX = g++

FLAGS = -g -pthread
LIBS = $(shell pkg-config python3 --libs)

MODULES = \
	main \
	sun_position

BDIR = build
SDIR = src

.PHONY: clean compilerflags

OBJS = $(addprefix ${BDIR}/,$(MODULES:=.o))

${BDIR}/solmap: ${OBJS}
	${CXX} -o ${BDIR}/solmap ${OBJS} ${FLAGS} ${LIBS}

-include $(OBJS:o=d)

${BDIR}/%.o: ${SDIR}/%.cpp | ${BDIR}
	${CXX} -c -MMD ${FLAGS} ${SDIR}/$*.cpp -o ${BDIR}/$*.o

${BDIR}/sun_position.o: ${BDIR}/sun_position.c
	${CC} -c `pkg-config python3 --cflags` -Isrc ${FLAGS} ${BDIR}/sun_position.c -o ${BDIR}/sun_position.o

${BDIR}/sun_position.c: plugin_build.py | ${BDIR}
	( \
		. venv/bin/activate; \
		python plugin_build.py \
	)

${BDIR}:
	mkdir ${BDIR}

clean: | ${BDIR}
	rm -r ${BDIR}
