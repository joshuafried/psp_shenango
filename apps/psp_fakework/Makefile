# Makefile for rocksdb
ROOT_PATH=../..
include $(ROOT_PATH)/build/shared.mk

librt_libs = $(ROOT_PATH)/bindings/cc/librt++.a
INC += -I$(ROOT_PATH)/bindings/cc


psp_fakework_src = psp_fakework.cc
psp_fakework_obj = $(psp_fakework_src:.cc=.o)

src = $(psp_fakework_src)
obj = $(psp_fakework_obj)
dep = $(obj:.o=.d)

# must be first
all: psp_fakework

psp_fakework: $(psp_fakework_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) -ldl $(psp_fakework_obj) $(librt_libs) $(RUNTIME_LIBS) $(LIBS)

ifneq ($(MAKECMDGOALS),clean)
-include $(dep)   # include all dep files in the makefile
endif

# rule to generate a dep file by using the C preprocessor
# (see man cpp for details on the -MM and -MT options)
%.d: %.cc
	@$(CXX) $(CXXFLAGS) $< -MM -MT $(@:.d=.o) >$@
%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(obj) $(dep) rocksdb_server create_db
