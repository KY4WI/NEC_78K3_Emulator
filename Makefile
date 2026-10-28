CC      = gcc
# Portable ISO C99 (-pedantic): the opcode tables used to need GNU binary
# literals (0b...), which forced -std=gnu99. They're hexadecimal now (see
# Opcodes.h), matching the rest of the toolchain, so this builds clean
# under strict C99 -- no GNU extensions left to require.
CFLAGS  = -std=c99 -pedantic -Wall -Wextra -Wno-missing-field-initializers \
          -Wno-unused-function -Wno-unused-parameter -O2

TARGET  = emu78k3
SRC     = emu78k3.c
HEADERS = Types.h Mnemonic.h Operand.h Opcode.h Opcodes.h Addressing.h

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)
