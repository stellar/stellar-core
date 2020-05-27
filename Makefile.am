## Process this file with automake to produce Makefile.in

SUBDIRS = lib src
AM_EXTRA_RECURSIVE_TARGETS = check-valgrind
ACLOCAL_AMFLAGS = -I m4

dist_doc_DATA = docs/stellar-core_example.cfg			\
	docs/stellar-core_standalone.cfg			\
	docs/stellar-core_testnet.cfg docs/software/admin.md	\
	docs/software/commands.md

include $(top_srcdir)/common.mk

.PHONY: always
always:
	@:

if USE_CLANG_FORMAT
format: always
	cd src && $(MAKE) $(AM_MAKEFLAGS) $@
endif # USE_CLANG_FORMAT

if USE_AFL_FUZZ
fuzz-testcases fuzz fuzz-clean: all
	cd src && $(MAKE) $(AM_MAKEFLAGS) $@
endif # USE_AFL_FUZZ

if USE_TRACY_GUI
TRACY_GUI_DIR=$(top_srcdir)/lib/tracy/profiler
TRACY_GUI_SOURCE=$(TRACY_GUI_DIR)/src
TRACY_GUI_BUILD=$(TRACY_GUI_DIR)/build/unix

$(TRACY_GUI_BUILD)/Tracy-release: $(wildcard $(TRACY_GUI_SOURCE)/*.*)
	$(MAKE) -C $(TRACY_GUI_BUILD) release CC="$(CC)" CXX="$(CXX)"

tracy-gui: $(TRACY_GUI_BUILD)/Tracy-release
	cp -v $< $@
endif # USE_TRACY_GUI

if USE_TRACY_CAPTURE
TRACY_CAPTURE_DIR=$(top_srcdir)/lib/tracy/capture
TRACY_CAPTURE_SOURCE=$(TRACY_CAPTURE_DIR)/src
TRACY_CAPTURE_BUILD=$(TRACY_CAPTURE_DIR)/build/unix

$(TRACY_CAPTURE_BUILD)/capture-release: $(wildcard $(TRACY_CAPTURE_SOURCE)/*.*)
	$(MAKE) -C $(TRACY_CAPTURE_BUILD) release CC="$(CC)" CXX="$(CXX)"

tracy-capture: $(TRACY_CAPTURE_BUILD)/capture-release
	cp -v $< $@
endif # USE_TRACY_CAPTURE

EXTRA_DIST = stellar-core.supp test/testnet/multitail.conf	\
	test/testnet/run-test.sh README.md make-mks

@VALGRIND_CHECK_RULES@
VALGRIND_FLAGS = --num-callers=30 --trace-children=yes \
          --trace-children-skip=hostname,aws,gcutil,gcloud,azure,node,python
VALGRIND_SUPPRESSIONS_FILES = stellar-core.supp

maintainer-clean-local:
	cd $(srcdir) && rm -rf `sed -ne 's!^/!!p' .gitignore` *~ .gitignore~

CLEANFILES = test-suite.log .gitignore~ *~
