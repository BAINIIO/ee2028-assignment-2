.DEFAULT_GOAL := all

ARM_TOOL := sh tools/arm-tool.sh
CC := $(ARM_TOOL) gcc
BUILD := build/firmware
TARGET := $(BUILD)/zombie_game
CPU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
CPPFLAGS := -DUSE_HAL_DRIVER -DSTM32L4S5xx \
 -ICore/Inc -IDrivers/STM32L4xx_HAL_Driver/Inc \
 -IDrivers/STM32L4xx_HAL_Driver/Inc/Legacy \
 -IDrivers/CMSIS/Include -IDrivers/CMSIS/Device/ST/STM32L4xx/Include \
 -IDrivers/BSP/B-L4S5I-IOT01 -IDrivers/BSP/Components/Common
CPPFLAGS += $(EXTRA_DEFINES)
CFLAGS := $(CPU) -std=c11 -Og -g3 -ffunction-sections -fdata-sections \
 -Wall -Wextra -Wformat=2 -Wshadow -Wno-unused-parameter -MMD -MP
LDFLAGS := $(CPU) -TSTM32L4S5VITX_FLASH.ld --specs=nano.specs --specs=nosys.specs \
 -Wl,--gc-sections -Wl,-Map=$(TARGET).map -Wl,--print-memory-usage -u _printf_float

HAL_NAMES := hal hal_cortex hal_rcc hal_rcc_ex hal_pwr hal_pwr_ex hal_flash \
 hal_flash_ex hal_gpio hal_dma hal_exti hal_i2c hal_i2c_ex hal_uart \
 hal_uart_ex hal_tim hal_tim_ex hal_spi hal_spi_ex
HAL_SOURCES := $(addprefix Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_,$(addsuffix .c,$(HAL_NAMES)))
BSP_NAMES := stm32l4s5i_iot01 stm32l4s5i_iot01_accelero stm32l4s5i_iot01_gyro \
 stm32l4s5i_iot01_magneto stm32l4s5i_iot01_tsensor stm32l4s5i_iot01_psensor stm32l4s5i_iot01_hsensor
BSP_SOURCES := $(addprefix Drivers/BSP/B-L4S5I-IOT01/,$(addsuffix .c,$(BSP_NAMES)))
COMPONENT_SOURCES := Drivers/BSP/Components/lsm6dsl/lsm6dsl.c \
 Drivers/BSP/Components/lis3mdl/lis3mdl.c Drivers/BSP/Components/lps22hb/lps22hb.c \
 Drivers/BSP/Components/hts221/hts221.c
APP_SOURCES := $(wildcard Core/Src/*.c)
SOURCES := $(APP_SOURCES) $(HAL_SOURCES) $(BSP_SOURCES) $(COMPONENT_SOURCES)
STARTUP := Core/Startup/startup_stm32l4s5vitx.s
OBJECTS := $(addprefix $(BUILD)/,$(SOURCES:.c=.o)) $(BUILD)/$(STARTUP:.s=.o)
# Optional private object generated from stdin; credentials never enter source.
LOCAL_CONFIG_OBJECT := $(wildcard $(BUILD)/network_credentials.o)
HOST_FLAGS := -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror \
 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -ICore/Inc
HOST_TESTS := $(patsubst tests/%.c,build/host/%,$(wildcard tests/test_*.c))
PORTABLE_SOURCES := Core/Src/game.c Core/Src/button.c Core/Src/telemetry.c Core/Src/game_display.c Core/Src/nfc_ndef.c

.PHONY: all host-test size clean help FORCE
all: $(TARGET).elf $(TARGET).bin $(TARGET).hex size

$(BUILD)/.configuration: FORCE
	@python3 tools/build_config.py "$@" "$(CPPFLAGS)" "$(CFLAGS)" "$(LDFLAGS)"

$(OBJECTS): $(BUILD)/.configuration

$(BUILD)/%.o: %.c Makefile Core/Inc/app_config.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(BUILD)/%.o: %.s Makefile
	@mkdir -p "$(@D)"
	$(CC) $(CPU) -g3 -x assembler-with-cpp -c "$<" -o "$@"

$(TARGET).elf: $(OBJECTS) $(LOCAL_CONFIG_OBJECT) STM32L4S5VITX_FLASH.ld Makefile
	$(CC) $(OBJECTS) $(LOCAL_CONFIG_OBJECT) $(LDFLAGS) -Wl,--start-group -lc -lm -Wl,--end-group -o "$@"

$(TARGET).bin: $(TARGET).elf
	$(ARM_TOOL) objcopy -O binary "$<" "$@"

$(TARGET).hex: $(TARGET).elf
	$(ARM_TOOL) objcopy -O ihex "$<" "$@"

size: $(TARGET).elf
	$(ARM_TOOL) size "$<"

build/host/%: tests/%.c $(PORTABLE_SOURCES) $(wildcard Core/Inc/*.h) Makefile
	@mkdir -p "$(@D)"
	clang $(HOST_FLAGS) "$<" $(PORTABLE_SOURCES) -lm -o "$@"

build/host/test_app: tests/test_app.c tests/fakes/main.h Core/Src/main.c $(PORTABLE_SOURCES) $(wildcard Core/Inc/*.h) Makefile
	@mkdir -p "$(@D)"
	clang -Itests/fakes $(HOST_FLAGS) "$<" $(PORTABLE_SOURCES) -lm -o "$@"

build/host/test_board_uart: tests/test_board_uart.c Core/Src/board_io.c $(wildcard Core/Inc/*.h) Makefile
	@mkdir -p "$(@D)"
	clang $(HOST_FLAGS) "$<" -o "$@"

build/host/test_network: tests/test_network.c Core/Src/network_io.c $(wildcard Core/Inc/*.h) Makefile
	@mkdir -p "$(@D)"
	clang $(HOST_FLAGS) "$<" -o "$@"

build/host/test_nfc_io: tests/test_nfc_io.c tests/fakes/nfc/stm32l4xx_hal.h Core/Src/nfc_io.c Core/Src/nfc_ndef.c $(wildcard Core/Inc/*.h) Makefile
	@mkdir -p "$(@D)"
	clang -Itests/fakes/nfc $(HOST_FLAGS) "$<" Core/Src/nfc_ndef.c -lm -o "$@"

build/host/test_command_telemetry: Core/Src/command_telemetry.c

build/host/test_external_oled: tests/test_external_oled.c tests/fakes/stm32l4xx_hal.h Core/Src/external_io.c $(PORTABLE_SOURCES) $(wildcard Core/Inc/*.h) Makefile
	@mkdir -p "$(@D)"
	clang -Itests/fakes $(HOST_FLAGS) "$<" $(PORTABLE_SOURCES) -lm -o "$@"

build/host/test_laptop_display: tests/test_laptop_display.c tests/fakes/stm32l4xx_hal.h Core/Src/external_io.c $(PORTABLE_SOURCES) $(wildcard Core/Inc/*.h) Makefile
	@mkdir -p "$(@D)"
	clang -Itests/fakes $(HOST_FLAGS) "$<" $(PORTABLE_SOURCES) -lm -o "$@"

host-test: $(HOST_TESTS)
	@set -e; for test in $(HOST_TESTS); do "./$$test"; done

clean:
	rm -rf build

help:
	@printf '%s\n' 'make: build STM32 ELF/BIN/HEX (does not flash)' \
	 'make host-test: run portable state-machine/threshold tests with sanitizers' \
	 'make EXTRA_DEFINES="-DAPP_ENABLE_OLED=0": disable optional peripherals' \
	 'python3 tools/board.py detect: list probes without changing firmware' \
	 'python3 tools/board.py flash: explicitly program the connected board' \
	 'python3 tools/board.py debug: start OpenOCD for GDB (halts/resets board)' \
	 'python3 tools/monitor.py: live UART monitor'

-include $(OBJECTS:.o=.d)
