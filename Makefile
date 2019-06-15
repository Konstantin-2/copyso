EXECUTABLE = copyso
prefix ?= /usr/local
exec_prefix ?= $(prefix)
bindir ?= $(exec_prefix)/bin
datarootdir ?= $(prefix)/share
srcdir ?= .

LDFLAGS += -Wall -Wl,-pie -pipe -lstdc++fs
CXXFLAGS += -std=c++17 -pipe -I$(srcdir) -Os -D NDEBUG -DDATAROOTDIR=\"$(datarootdir)\" -fpie
CXXFLAGSD = $(CXXFLAGS) -O0 -Wall -ggdb -U NDEBUG
OBJECTS = $(EXECUTABLE).o

.PHONY : all clean i18n install uninstall

all : $(EXECUTABLE) ru/LC_MESSAGES/$(EXECUTABLE).mo

$(EXECUTABLE) : $(OBJECTS)
	@echo 'LINK $@'
	@$(CXX) -o $@ $(OBJECTS) $(LDFLAGS)

%.o : $(srcdir)/%.cpp
	@echo 'CPP  $@'
	@$(CXX) $(CXXFLAGS) -c $<

$(EXECUTABLE).pot : $(EXECUTABLE).cpp
	@echo 'POT  $@'
	@xgettext -k_ --c++ -s --no-wrap --omit-header --no-location -o $(EXECUTABLE).pot $(EXECUTABLE).cpp

ru/$(EXECUTABLE).po : $(EXECUTABLE).pot
	@echo 'PO   $@'
	@msgmerge --update ru/$(EXECUTABLE).po $(EXECUTABLE).pot

ru/LC_MESSAGES/$(EXECUTABLE).mo : ru/$(EXECUTABLE).po
	@echo 'MO   $@'
	@mkdir -p ru/LC_MESSAGES
	@msgfmt --output $@ $<

i18n : ru/LC_MESSAGES/$(EXECUTABLE).mo

clean :
	$(RM) *.o $(EXECUTABLE) ru/$(EXECUTABLE).po~ ru/LC_MESSAGES/$(EXECUTABLE).mo

install ::
	install -s $(EXECUTABLE) $(bindir)
	mkdir -p $(datarootdir)/locale/ru
	cp -r ru/LC_MESSAGES $(datarootdir)/locale/ru
	mkdir -p $(datarootdir)/man/man1
	cp $(EXECUTABLE).1 $(datarootdir)/man/man1
	gzip -f $(datarootdir)/man/man1/$(EXECUTABLE).1
	mkdir -p $(datarootdir)/man/ru/man1
	cp ru/$(EXECUTABLE).1 $(datarootdir)/man/ru/man1
	gzip -f $(datarootdir)/man/ru/man1/$(EXECUTABLE).1

uninstall :
	$(RM) $(bindir)/$(EXECUTABLE) $(datarootdir)/locale/ru/LC_MESSAGES/$(EXECUTABLE).mo $(datarootdir)/man/man1/$(EXECUTABLE).1.gz $(datarootdir)/man/ru/man1/$(EXECUTABLE).1.gz
