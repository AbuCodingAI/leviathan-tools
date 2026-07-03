CXX ?= gcc
CFLAGS ?= -O2 -pipe -Wall -Wextra $(shell pkg-config --cflags gdk-pixbuf-2.0)
LIBS ?= -lX11 -lXext -lXrandr $(shell pkg-config --libs gdk-pixbuf-2.0)

BIN_DIR := ../../bin
SESSION_DIR := /usr/share/xsessions

all: $(BIN_DIR)/churro-time-widget $(BIN_DIR)/churro-tray $(BIN_DIR)/churro-dock $(BIN_DIR)/churro-settings $(BIN_DIR)/churro-session $(BIN_DIR)/churro-launcher $(BIN_DIR)/churro-desktops $(BIN_DIR)/churro-keybind

$(BIN_DIR)/churro-time-widget: time-widget.c
	mkdir -p $(BIN_DIR)
	$(CXX) $(CFLAGS) $< -o $@ $(LIBS)

$(BIN_DIR)/churro-tray: tray-widget.c
	mkdir -p $(BIN_DIR)
	$(CXX) $(CFLAGS) $< -o $@ $(LIBS)

$(BIN_DIR)/churro-dock: dock.c
	mkdir -p $(BIN_DIR)
	$(CXX) $(CFLAGS) $< -o $@ $(LIBS)

$(BIN_DIR)/churro-settings: settings.c
	mkdir -p $(BIN_DIR)
	$(CXX) $(CFLAGS) $< -o $@ $(LIBS)

$(BIN_DIR)/churro-session: churro-session.sh
	mkdir -p $(BIN_DIR)
	cp churro-session.sh $(BIN_DIR)/churro-session
	chmod 0755 $(BIN_DIR)/churro-session

$(BIN_DIR)/churro-launcher: launcher.c
	mkdir -p $(BIN_DIR)
	$(CXX) $(CFLAGS) $< -o $@ $(LIBS)

$(BIN_DIR)/churro-desktops: desktops.c
	mkdir -p $(BIN_DIR)
	$(CXX) $(CFLAGS) $< -o $@ $(LIBS)

$(BIN_DIR)/churro-keybind: keybind.c
	mkdir -p $(BIN_DIR)
	$(CXX) $(CFLAGS) $< -o $@ $(LIBS)

install-session: churro.desktop
	sudo mkdir -p $(SESSION_DIR)
	sudo cp churro.desktop $(SESSION_DIR)/churro.desktop
	@echo "Churro session installed. Select 'Churro' at login screen."

clean:
	rm -f $(BIN_DIR)/churro-time-widget $(BIN_DIR)/churro-tray $(BIN_DIR)/churro-dock $(BIN_DIR)/churro-settings $(BIN_DIR)/churro-session $(BIN_DIR)/churro-launcher $(BIN_DIR)/churro-desktops $(BIN_DIR)/churro-keybind

.PHONY: all clean
