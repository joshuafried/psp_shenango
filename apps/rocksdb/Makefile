# Makefile for rocksdb
ROOT_PATH=../..
include $(ROOT_PATH)/build/shared.mk

librt_libs = $(ROOT_PATH)/bindings/cc/librt++.a
INC += -I$(ROOT_PATH)/bindings/cc

rocksdb_server_src = rocksdb_server.cc
rocksdb_server_obj = $(rocksdb_server_src:.cc=.o)

create_db_src = create_db.cc
create_db_obj = $(create_db_src:.cc=.o)

INC += -Irocksdb/include/rocksdb/
LIBS += rocksdb/librocksdb.a -lgflags -lsnappy -lz -llz4 -lpthread -ljemalloc -ldl

src = $(rocksdb_server_src) $(create_db_src)
obj = $(rocksdb_server_obj) $(create_db_obj)
dep = $(obj:.o=.d)

# must be first
all: rocksdb_server create_db

rocksdb_server: $(rocksdb_server_obj) $(librt_libs) $(RUNTIME_DEPS)
	$(LDXX) -o $@ $(LDFLAGS) -ldl $(rocksdb_server_obj) $(librt_libs) $(RUNTIME_LIBS) $(LIBS)

create_db: $(create_db_obj)
	$(LDXX) -o $@ $(LDFLAGS) -ldl $(create_db_obj) $(LIBS)


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
