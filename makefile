# ----------------------------
# Makefile Options
# ----------------------------

NAME = BAKROOMS
ICON = icon.png
DESCRIPTION = "Backrooms Escape"
COMPRESSED = NO
ARCHIVED = NO

CFLAGS = -Wall -Wextra -O2
CXXFLAGS = -Wall -Wextra -O2

# ----------------------------

include $(shell cedev-config --makefile)
