# Command-line and CI build of the STM32CubeIDE project in this folder
# (NUCLEO-F411RE), using arm-none-eabi-gcc and no IDE.
#
#   make -C stm32          build build/parcel-box.{elf,bin,hex} and print sizes
#   make -C stm32 flash    flash it over SWD (ST-LINK) and reset the board
#   make -C stm32 clean
#
# The IDE build (Debug/ and Release/, from .cproject) and this one compile the
# same sources. To keep them from drifting, both take whole folders rather
# than file lists: .cproject's source entries are Core, Drivers and BoxCore
# (with BoxCore/Tests excluded), and this Makefile globs the same folders
# below. A new .c file in Core/Src or BoxCore/Src is built by both with no
# edit to either. If you change the folders, the include paths or the
# defines, change .cproject (Debug and Release) and this file together.
#
# The tools default to the ones on PATH. To use the copies bundled with
# STM32CubeIDE instead, point these at them, e.g. on Windows:
#   make -C stm32 ARM_PREFIX=C:/ST/STM32CubeIDE_1.19.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.<version>/tools/bin/arm-none-eabi-
#   make -C stm32 flash PROGRAMMER=C:/ST/STM32CubeIDE_1.19.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.<version>/tools/bin/STM32_Programmer_CLI.exe
#
# The Box logic core in BoxCore/Src is compiled from where it is, the same
# source the PC tests use (`make test` at the repo root). BoxCore/Tests is
# never compiled into the firmware.

ARM_PREFIX ?= arm-none-eabi-
PROGRAMMER ?= STM32_Programmer_CLI

CC      := $(ARM_PREFIX)gcc
OBJCOPY := $(ARM_PREFIX)objcopy
SIZE    := $(ARM_PREFIX)size

TARGET  := parcel-box
BUILD   := build

# ST's files, used as shipped or as generated: warnings are shown but don't
# fail the build.
ST_SRCS := \
	Core/Src/system_stm32f4xx.c \
	Core/Src/syscalls.c \
	Core/Src/sysmem.c \
	$(wildcard Drivers/STM32F4xx_HAL_Driver/Src/*.c)

# Everything else in the source folders is ours (including the USER CODE in
# CubeMX's main.c, stm32f4xx_it.c and stm32f4xx_hal_msp.c): warnings are
# errors.
OWN_SRCS := $(filter-out $(ST_SRCS),$(wildcard Core/Src/*.c)) \
	$(wildcard BoxCore/Src/*.c)

ASM_SRCS := $(wildcard Core/Startup/*.s)
LDSCRIPT := STM32F411RETX_FLASH.ld

MCU      := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
DEFS     := -DSTM32F411xE -DUSE_HAL_DRIVER
# ST's headers are -isystem so their warnings never fail our build.
INCS     := -ICore/Inc -IBoxCore/Inc \
	-isystem Drivers/STM32F4xx_HAL_Driver/Inc \
	-isystem Drivers/STM32F4xx_HAL_Driver/Inc/Legacy \
	-isystem Drivers/CMSIS/Device/ST/STM32F4xx/Include \
	-isystem Drivers/CMSIS/Include
CFLAGS   := $(MCU) $(DEFS) $(INCS) -std=gnu11 -Os -g3 \
	-ffunction-sections -fdata-sections -MMD -MP
OWN_WARN := -Wall -Wextra -Werror
ST_WARN  := -Wall
LDFLAGS  := $(MCU) -T$(LDSCRIPT) --specs=nano.specs --specs=nosys.specs \
	-Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map -Wl,--print-memory-usage

OWN_OBJS := $(OWN_SRCS:%.c=$(BUILD)/obj/%.o)
ST_OBJS  := $(ST_SRCS:%.c=$(BUILD)/obj/%.o)
ASM_OBJS := $(ASM_SRCS:%.s=$(BUILD)/obj/%.o)
OBJS     := $(OWN_OBJS) $(ST_OBJS) $(ASM_OBJS)

ELF := $(BUILD)/$(TARGET).elf

# Everything rebuilds when this Makefile (flags, file lists) or the
# toolchain changes. The stamp is rewritten only when ARM_PREFIX differs
# from the last build, so normal builds stay incremental. Defined before any
# rule uses it, because make expands prerequisites as it reads each rule.
REBUILD_ON := Makefile $(BUILD)/toolchain.txt

.PHONY: all flash clean FORCE

all: $(ELF) $(BUILD)/$(TARGET).bin $(BUILD)/$(TARGET).hex
	$(SIZE) $(ELF)

$(ELF): $(OBJS) $(LDSCRIPT) $(REBUILD_ON)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/$(TARGET).bin: $(ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/$(TARGET).hex: $(ELF)
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/toolchain.txt: FORCE
	@mkdir -p $(BUILD)
	@echo '$(ARM_PREFIX)' > $@.new; \
	if cmp -s $@.new $@; then rm -f $@.new; else mv -f $@.new $@; fi

$(OWN_OBJS): $(BUILD)/obj/%.o: %.c $(REBUILD_ON)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OWN_WARN) -c $< -o $@

$(ST_OBJS): $(BUILD)/obj/%.o: %.c $(REBUILD_ON)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ST_WARN) -c $< -o $@

$(ASM_OBJS): $(BUILD)/obj/%.o: %.s $(REBUILD_ON)
	@mkdir -p $(dir $@)
	$(CC) $(MCU) -x assembler-with-cpp -c $< -o $@

flash: all
	"$(PROGRAMMER)" -c port=SWD -w $(ELF) -v -rst

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
