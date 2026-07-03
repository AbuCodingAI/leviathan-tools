# Java Applet Plugin Build

BROWSER_SRC = ../../src
INSTALL_DIR = $(HOME)/.infinitas/plugins

CFLAGS = -Wall -Wextra -O2 -std=c99 -fPIC -shared \
         -I$(BROWSER_SRC) \
         $(shell pkg-config --cflags glib-2.0 gtk4)

all: java_plugin.so AppletRunner.jar

java_plugin.so: java_plugin.c
	$(CC) $(CFLAGS) -o $@ $<

AppletRunner.jar: AppletRunner.java
	javac --add-modules java.desktop -Xlint:-deprecation AppletRunner.java
	jar cf AppletRunner.jar AppletRunner*.class
	rm -f AppletRunner*.class

install: java_plugin.so AppletRunner.jar
	mkdir -p $(INSTALL_DIR)
	cp java_plugin.so $(INSTALL_DIR)/
	cp AppletRunner.jar $(INSTALL_DIR)/
	@echo "Java Applet plugin installed to $(INSTALL_DIR)"
	@echo "Restart Infinitas to activate."

clean:
	rm -f java_plugin.so AppletRunner.jar AppletRunner*.class

.PHONY: all install clean
