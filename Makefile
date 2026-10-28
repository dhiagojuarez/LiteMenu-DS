#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
​ifeq ($(strip (DEVKITARM)),)(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif
​include $(DEVKITARM)/ds_rules
​#---------------------------------------------------------------------------------
​Project Directories & Target Name
​#---------------------------------------------------------------------------------
TARGET		:=	LiteMenu
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
​#---------------------------------------------------------------------------------
​Nintendo DS ROM Header Strings
​#---------------------------------------------------------------------------------
GAME_TITLE	:=	LiteMenu DS
GAME_SUBTITLE1	:=	Slot-1 Frontend
GAME_SUBTITLE2	:=	4MB NTR Mode
​#---------------------------------------------------------------------------------
​Compiler & Linker Flags (DS Lite ARM946E-S 67MHz, 4MB NTR Mode)
​#---------------------------------------------------------------------------------
ARCH	:=	-marm -mcpu=arm946e-s -mtune=arm946e-s
​CFLAGS	:=	-Wall -Wextra -O2 $(ARCH) -ffunction-sections -fdata-sections -DARM9
CXXFLAGS	:=	$(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS	:=	(ARCH)
LDFLAGS	=	-specs=ds_arm9.specs -g $(ARCH) -Wl,-Map,$(notdir*.map) -Wl,--gc-sections
​LIBS	:=	-lfat -lnds9
LIBDIRS	:=	$(LIBNDS)
​#---------------------------------------------------------------------------------
ifneq ((BUILD),(notdir$(CURDIR)))
#---------------------------------------------------------------------------------
​export OUTPUT	:=	(CURDIR)/(TARGET)
export VPATH	:=	(foreach dir,(SOURCES),(CURDIR)/(dir)) 
(foreach dir,(DATA),(CURDIR)/(dir))
export DEPSDIR	:=	(CURDIR)/(BUILD)
​CFILES		:=	(foreach dir,(SOURCES),(notdir $(wildcard$(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),(notdir (wildcard(dir)/.cpp)))
SFILES		:=	(foreach dir,(SOURCES),$(notdir (wildcard(dir)/.s)))
BINFILES	:=	(foreach dir,(DATA),$(notdir (wildcard(dir)/.)))
​ifeq ((strip(CPPFILES)),)
export LD	:=	$(CC)
else
export LD	:=	$(CXX)
endif
​export OFILES_BIN	:=	(addsuffix .o,(BINFILES))
export OFILES_SOURCES	:=	(CPPFILES:.cpp=.o) $(CFILES:.c=.o)$(SFILES:.s=.o)
export OFILES		:=	$(OFILES_BIN)$(OFILES_SOURCES)
export HFILES		:=	$(addsuffix .h,$(subst .,_,(BINFILES)))
​export INCLUDE	:=	(foreach dir,(INCLUDES),-I$(CURDIR)/(dir)) \$(foreach dir,(LIBDIRS),-I$(dir)/include) 
-I$(CURDIR)/$(BUILD)
​export LIBPATHS	:=	(foreach dir,(LIBDIRS),-L$(dir)/lib)
​export _GAME_TITLE	:=	$(GAME_TITLE)
export _GAME_SUBTITLE1	:=	$(GAME_SUBTITLE1)
export _GAME_SUBTITLE2	:=	$(GAME_SUBTITLE2)
​.PHONY: $(BUILD) clean all
​all: $(BUILD)
​(BUILD):
@[ -d $@ ] \vert{}\vert{} mkdir -p$@
@(MAKE) --no-print-directory -C (BUILD) -f(CURDIR)/Makefile
​clean:
@echo clean ...
@rm -fr (BUILD)(TARGET).elf $(TARGET).nds $(TARGET).arm9
​#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
​DEPENDS	:=	$(OFILES:.o=.d)
​(OUTPUT).nds	:(OUTPUT).elf
​(OUTPUT).elf	:(OFILES)
​(OFILES_SOURCES) :(HFILES)
​%.bin.o	%_bin.h :	%.bin
@echo (notdir<)
@$(bin2o)
​-include $(DEPENDS)
​#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------