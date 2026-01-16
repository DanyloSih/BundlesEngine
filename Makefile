# Project name
PROJECT_NAME = BundlesEngine

# Build configuration (Debug or Release)
BUILD_TYPE ?= Debug

# Platform detection
ifeq ($(OS),Windows_NT)
    PLATFORM = Windows
    EXE_EXT = .exe
    LIB_EXT = .a
    LIB_PREFIX = lib
    RM = del /Q /F
    RMDIR = rmdir /S /Q
    MKDIR = mkdir
    PATH_SEP = \\
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
        PLATFORM = Linux
    else ifeq ($(UNAME_S),Darwin)
        PLATFORM = macOS
    else
        PLATFORM = Unix
    endif
    EXE_EXT =
    LIB_EXT = .a
    LIB_PREFIX = lib
    RM = rm -f
    RMDIR = rm -rf
    MKDIR = mkdir -p
    PATH_SEP = /
endif

# Paths
ROOT_DIR = .
LIBS_DIR = $(ROOT_DIR)/libs
INCLUDE_DIR = $(ROOT_DIR)/include
SRC_DIR = $(ROOT_DIR)/src
BUILD_BASE = $(ROOT_DIR)/builds/$(PLATFORM)/$(BUILD_TYPE)
BUILD_LIBS_DIR = $(BUILD_BASE)/libs
BUILD_TEMP_DIR = $(BUILD_BASE)/temp

# Compiler and flags
CC = gcc
AR = ar

ifeq ($(BUILD_TYPE),Debug)
    CFLAGS = -g -O0 -Wall -Wextra
else
    CFLAGS = -O2 -Wall
endif

CFLAGS += -I$(INCLUDE_DIR)

# Find all libraries in libs/
LIB_DIRS = $(wildcard $(LIBS_DIR)/*)
LIB_NAMES = $(notdir $(LIB_DIRS))

# Project files
SRC_FILES = $(wildcard $(SRC_DIR)/*.c)
OBJ_FILES = $(patsubst $(SRC_DIR)/%.c,$(BUILD_TEMP_DIR)/%.o,$(SRC_FILES))

# Executable file
EXECUTABLE = $(BUILD_BASE)/$(PROJECT_NAME)$(EXE_EXT)

# Targets
.PHONY: all clean rebuild-% libs

all: init libs $(EXECUTABLE)

# Initialize directories
init:
	@echo "=== Initializing build structure ==="
	@echo "Platform: $(PLATFORM)"
	@echo "Configuration: $(BUILD_TYPE)"
ifeq ($(OS),Windows_NT)
	@if not exist "$(BUILD_BASE)" $(MKDIR) "$(BUILD_BASE)"
	@if not exist "$(BUILD_LIBS_DIR)" $(MKDIR) "$(BUILD_LIBS_DIR)"
	@if not exist "$(BUILD_TEMP_DIR)" $(MKDIR) "$(BUILD_TEMP_DIR)"
else
	@$(MKDIR) $(BUILD_BASE)
	@$(MKDIR) $(BUILD_LIBS_DIR)
	@$(MKDIR) $(BUILD_TEMP_DIR)
endif

# Build all libraries
libs: init $(LIB_NAMES)

# Rule for each library
$(LIB_NAMES): %:
	@echo "=== Processing library: $@ ==="
ifeq ($(OS),Windows_NT)
	@if not exist "$(BUILD_LIBS_DIR)\\$@" $(MKDIR) "$(BUILD_LIBS_DIR)\\$@"
	@if not exist "$(BUILD_LIBS_DIR)\\$@\\temp" $(MKDIR) "$(BUILD_LIBS_DIR)\\$@\\temp"
else
	@$(MKDIR) $(BUILD_LIBS_DIR)/$@
	@$(MKDIR) $(BUILD_LIBS_DIR)/$@/temp
endif
	@$(MAKE) -s build-lib LIB_NAME=$@

# Build individual library
build-lib:
	@echo "Checking library $(LIB_NAME)..."
ifeq ($(wildcard $(BUILD_LIBS_DIR)/$(LIB_NAME)/$(LIB_PREFIX)$(LIB_NAME)$(LIB_EXT)),)
	@echo "Building library $(LIB_NAME)..."
	@$(MAKE) -s compile-lib LIB_NAME=$(LIB_NAME)
else
	@echo "Library $(LIB_NAME) already built (skipping)"
endif

# Compile library
compile-lib:
ifneq ($(wildcard $(LIBS_DIR)/$(LIB_NAME)/Makefile),)
	@echo "Using $(LIB_NAME) library Makefile"
	@cd $(LIBS_DIR)/$(LIB_NAME) && $(MAKE) BUILD_DIR=$(BUILD_LIBS_DIR)/$(LIB_NAME)
else
	@echo "Compiling $(LIB_NAME) sources..."
	@$(MAKE) -s compile-lib-sources LIB_NAME=$(LIB_NAME)
	@echo "Creating static library $(LIB_PREFIX)$(LIB_NAME)$(LIB_EXT)..."
	@cd $(BUILD_LIBS_DIR)/$(LIB_NAME)/temp && $(AR) rcs ../$(LIB_PREFIX)$(LIB_NAME)$(LIB_EXT) *.o
	@echo "Library $(LIB_NAME) built successfully"
endif

# Compile library source files
compile-lib-sources:
	@$(eval LIB_SOURCES := $(wildcard $(LIBS_DIR)/$(LIB_NAME)/*.c))
	@$(foreach src,$(LIB_SOURCES),\
		echo "  Compiling $(notdir $(src))..." && \
		$(CC) $(CFLAGS) -I$(LIBS_DIR)/$(LIB_NAME) -c $(src) \
		-o $(BUILD_LIBS_DIR)/$(LIB_NAME)/temp/$(notdir $(basename $(src))).o $(if $(filter-out $(lastword $(LIB_SOURCES)),$(src)),&&,))

# Build main executable
$(EXECUTABLE): $(OBJ_FILES)
	@echo "=== Linking main project ==="
	@$(eval LIBS := $(wildcard $(BUILD_LIBS_DIR)/*/$(LIB_PREFIX)*$(LIB_EXT)))
	@$(eval INCLUDE_FLAGS := $(foreach libdir,$(LIB_DIRS),-I$(libdir)))
	$(CC) $(CFLAGS) $(INCLUDE_FLAGS) $(OBJ_FILES) -o $(EXECUTABLE) $(LIBS)
	@echo "Build completed: $(EXECUTABLE)"

# Compile project sources
$(BUILD_TEMP_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "Compiling: $<"
	@$(CC) $(CFLAGS) $(foreach libdir,$(LIB_DIRS),-I$(libdir)) -c $< -o $@

# Force rebuild of specific library
rebuild-%:
	@echo "=== Force rebuilding library: $* ==="
ifeq ($(OS),Windows_NT)
	@if exist "$(BUILD_LIBS_DIR)\\$*" $(RMDIR) "$(BUILD_LIBS_DIR)\\$*"
	@$(MKDIR) "$(BUILD_LIBS_DIR)\\$*"
	@$(MKDIR) "$(BUILD_LIBS_DIR)\\$*\\temp"
else
	@$(RMDIR) $(BUILD_LIBS_DIR)/$*
	@$(MKDIR) $(BUILD_LIBS_DIR)/$*
	@$(MKDIR) $(BUILD_LIBS_DIR)/$*/temp
endif
	@$(MAKE) -s compile-lib LIB_NAME=$*

# Clean
clean:
	@echo "=== Cleaning ==="
ifeq ($(OS),Windows_NT)
	@if exist "$(BUILD_BASE)" $(RMDIR) "$(BUILD_BASE)"
else
	@$(RMDIR) $(BUILD_BASE)
endif
	@echo "Cleaning completed"

# Help
help:
	@echo "Available commands:"
	@echo "  make                 - Build project (Debug by default)"
	@echo "  make BUILD_TYPE=Release - Build Release version"
	@echo "  make rebuild-<lib>   - Rebuild specific library"
	@echo "  make clean           - Clean build"
	@echo ""
	@echo "Example: make rebuild-lua"