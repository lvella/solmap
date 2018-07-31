CC = gcc
CXX = g++ -std=c++17
GLSLC = glslc

OPTFLAGS = -g

MODULES = \
	buffer \
	main \
	mesh_tools \
	shadow_processor \
	sun_position \
	vk_manager

SHADERS = \
	depth-map.vert \
	incidence-calc.comp

SDIR = src-host
DDIR = src-device

BDIR = build
BINCDIR = build/include

LIBS = $(shell pkg-config python3 --libs) -lvulkan -lassimp
FLAGS = ${OPTFLAGS} -pthread -I${BINCDIR}

INC_SHADERS = $(addprefix ${BINCDIR}/,$(SHADERS:=.inc))
OBJS = $(addprefix ${BDIR}/,$(MODULES:=.o))

${BDIR}/solmap: ${INC_SHADERS} ${OBJS}
	${CXX} -o ${BDIR}/solmap ${OBJS} ${FLAGS} ${LIBS}

-include $(OBJS:o=d)

${BDIR}/%.o: ${SDIR}/%.cpp | ${BDIR}
	${CXX} -c -MMD ${FLAGS} ${SDIR}/$*.cpp -o ${BDIR}/$*.o

${BDIR}/sun_position.o: ${BDIR}/sun_position.c
	${CC} -c `pkg-config python3 --cflags` -I${SDIR} ${FLAGS} ${BDIR}/sun_position.c -o ${BDIR}/sun_position.o

${BDIR}/sun_position.c: plugin_build.py | ${BDIR}
	( \
		. venv/bin/activate; \
		python plugin_build.py \
	)

# Compile the shaders to includable SPIR-V
${BINCDIR}/%.inc: ${DDIR}/% | ${BINCDIR}
	${GLSLC} -mfmt=c -O ${DDIR}/$* -o ${BINCDIR}/$*.inc

${BDIR}:
	mkdir ${BDIR}

${BINCDIR}: | ${BDIR}
	mkdir ${BINCDIR}

.PHONY: clean compilerflags

compilerflags:
	@echo ${FLAGS}

clean: | ${BDIR}
	rm -r ${BDIR}
