.PHONY:all clean directories

MKDIR_P = mkdir -p

PREFIX ?= /usr/bin
VERSION ?=0.1
INSTALL_PATH = ${PREFIX}/birdee${VERSION}/

##
PWD_DIR=$(shell pwd)
COMPILER_DIR=$(PWD_DIR)/Birdee
RUNTIME_DIR=$(PWD_DIR)/BirdeeRuntime
BLIB_DIR=$(PWD_DIR)/BirdeeHome/src
INC_DIR=$(PWD_DIR)/Birdee/include
INC_DIR2=$(PWD_DIR)/dependency/include
BIN_DIR=$(PWD_DIR)/BirdeeHome/bin
LIB_DIR=$(PWD_DIR)/BirdeeHome/lib
PLAYGROUND_DIR=$(PWD_DIR)/BirdeePlayground

PYLIBS = $(shell python3-config --libs)
CXX ?= g++
CPPFLAGS ?= -g -DBIRDEE_USE_DYN_LIB -std=c++14 -g -I$(INC_DIR) -I$(INC_DIR2) $(shell python3 -m pybind11 --includes)
LIBS ?= -pthread

##
export PWD_DIR CXX CPPFLAGS LIBS COMPILER_DIR INC_DIR BIN_DIR PYLIBS LIB_DIR BLIB_DIR

##
all: directories compiler runtime libraries playground

directories: ${BIN_DIR} ${LIB_DIR}

${BIN_DIR}:
	${MKDIR_P} ${BIN_DIR}

${LIB_DIR}:
	${MKDIR_P} ${LIB_DIR}

compiler:
	$(MAKE)  -C $(COMPILER_DIR)
	
runtime:
	$(MAKE)  -C $(RUNTIME_DIR)

libraries: compiler runtime
	$(MAKE) -C $(BLIB_DIR)

playground: libraries
	$(MAKE)  -C $(PLAYGROUND_DIR)
	
install:
	cp -rf $(PWD_DIR)/BirdeeHome $(INSTALL_PATH)
	ln $(INSTALL_PATH)/bin/birdeec $(PREFIX)/birdeec
	ln $(INSTALL_PATH)/bin/birdeeplay $(PREFIX)/birdeeplay

uninstall:
	rm -r $(INSTALL_PATH)
	rm  $(PREFIX)/birdeec
	rm  $(PREFIX)/birdeeplay
##
clean:
	$(MAKE)  -C $(COMPILER_DIR) clean
	$(MAKE)  -C $(RUNTIME_DIR) clean
	$(MAKE)  -C $(BLIB_DIR) clean
	rm -rf ${BIN_DIR}
	rm -rf ${LIB_DIR}

