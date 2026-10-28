#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip (DEVKITARM)),)(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif
include $(DEVKITARM)/ds_rules
#---------------------------------------------------------------------------------
TARGET is the name of the output .nds file
BUILD is the directory where object files & intermediate targets will be placed
SOURCES is a list of directories containing source code
INCLUDES is a list of directories containing extra header files
DATA is a list of directories containing binary data
GRAPHICS is a list of directories containing graphics files
#---------------------------------------------------------------------------------
TARGET		:=	LiteMenu
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
#---------------------------------------------------------------------------------
Contributions to the Nintendo DS Header (ndstool parameters)
#---------------------------------------------------------------------------------
GAME_TITLE	:=	LiteMenu DS
GAME_SUBTITLE1	:=	Slot-1 Frontend
GAME_SUBTITLE2	:=	4MB NTR Mode
GAME_ICON	:=	$(CURDIR)/icon.bmp
#---------------------------------------------------------------------------------
Options for code generation
Explicitly targeting Nintendo DS Lite ARM946E-S (67 MHz) in 32-bit ARM mode
with strict 4MB NTR RAM footprint optimizations.
#---------------------------------------------------------------------------------
ARCH	:=	-marm -mcpu=arm946e-s -mtune=arm946e-s
CFLAGS	:=	-Wall -Wextra -Wshadow -O2 
$(ARCH) -ffunction-sections -fdata-sections 
-DARM9
CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS	:=	(ARCH)
LDFLAGS	=	-specs=ds_arm9.specs -g $(ARCH) -Wl,-Map,$(notdir*.map) -Wl,--gc-sections
#---------------------------------------------------------------------------------
any extra libraries we wish to link with the project
Note: -lfat must precede -lnds9 so libfat symbols resolve against libnds DLDI/disc_io
#---------------------------------------------------------------------------------
LIBS	:= -lfat -lnds9
#---------------------------------------------------------------------------------
list of directories containing libraries, this must be the top level containing
include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:=	$(LIBNDS)
#---------------------------------------------------------------------------------
no real need to edit anything past this point unless you need to add additional
rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ((BUILD),(notdir$(CURDIR)))
#---------------------------------------------------------------------------------
export OUTPUT	:=	(CURDIR)/(TARGET)
export VPATH	:=	(foreach dir,(SOURCES),(CURDIR)/(dir)) 
(foreach dir,(DATA),(CURDIR)/(dir))
export DEPSDIR	:=	(CURDIR)/(BUILD)
CFILES		:=	(foreach dir,(SOURCES),(notdir $(wildcard$(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),(notdir (wildcard(dir)/.cpp)))
SFILES		:=	(foreach dir,(SOURCES),$(notdir (wildcard(dir)/.s)))
BINFILES	:=	(foreach dir,(DATA),$(notdir (wildcard(dir)/.)))
#---------------------------------------------------------------------------------
use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ((strip(CPPFILES)),)
export LD	:=	$(CC)
else
export LD	:=	$(CXX)
endif
export OFILES_BIN := (addsuffix .o,(BINFILES))
export OFILES_SOURCES := $(CPPFILES:.cpp=.o) (CFILES:.c=.o)(SFILES:.s=.o)
export OFILES := (OFILES_BIN)(OFILES_SOURCES)
export HFILES := (addsuffix .h,(subst .,_,$(BINFILES)))
export INCLUDE	:=	(foreach dir,(INCLUDES),-I$(CURDIR)/(dir)) \$(foreach dir,(LIBDIRS),-I$(dir)/include) 
-I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	(foreach dir,(LIBDIRS),-L$(dir)/lib)
export _GAME_TITLE := $(GAME_TITLE)
export _GAME_SUBTITLE1 := $(GAME_SUBTITLE1)
export _GAME_SUBTITLE2 := $(GAME_SUBTITLE2)
.PHONY: $(BUILD) clean all
#---------------------------------------------------------------------------------
all: $(BUILD)
(BUILD):
@[ -d $@ ] \vert{}\vert{} mkdir -p$@
@(MAKE) --no-print-directory -C (BUILD) -f(CURDIR)/Makefile
#---------------------------------------------------------------------------------
clean:
@echo clean ...
@rm -fr (BUILD)(TARGET).elf $(TARGET).nds $(TARGET).arm9
#---------------------------------------------------------------------------------
else
DEPENDS	:=	$(OFILES:.o=.d)
#---------------------------------------------------------------------------------
main targets: build .nds ROM via ndstool from compiled ARM9 ELF + default ARM7
#---------------------------------------------------------------------------------
(OUTPUT).nds	:(OUTPUT).elf
@ndstool -c (OUTPUT).nds \ 	-9(OUTPUT).elf 
-b (ICON) "(_GAME_TITLE);(_GAME_SUBTITLE1);(_GAME_SUBTITLE2)"
@echo "Built (notdir@) successfully (4MB NTR Slot-1 DLDI Ready)."
(OUTPUT).elf	:(OFILES)
(OFILES_SOURCES) :(HFILES)
#---------------------------------------------------------------------------------
rule for binary data files using bin2s
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
@echo (notdir<)
@$(bin2o)
-include $(DEPENDS)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------