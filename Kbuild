# Kbuild for the external module kernel_pc_demo.
# Object paths are relative to this directory (the project root).
obj-m += kernel_pc_demo.o

kernel_pc_demo-y := src/main.o \
		    src/params.o \
		    src/producer.o \
		    src/consumer.o

# Header search path, so src/*.c can #include "pc_demo.h".
ccflags-y := -I$(src)/src
