include NickelHook/NickelHook.mk

override LIBRARY  := libnfolders.so
override SOURCES  += nfolders.cc
override CFLAGS   += -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden
override CXXFLAGS += -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden -fvisibility-inlines-hidden

include NickelHook/NickelHook.mk
