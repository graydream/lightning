project (lightning)

cmake_minimum_required(VERSION 2.8)

set(CMAKE_VERBOSE_MAKEFILE OFF)

set(CMAKE_C_FLAGS "-std=gnu99 -W -Wall -fPIC -Werror -Wno-array-bounds -Wno-implicit-fallthrough -Werror=return-type -Wno-stringop-truncation -Wno-format-truncation -Wno-format-overflow -Wno-stringop-overflow -Wno-misleading-indentation -Wno-deprecated-declarations -Wno-cast-function-type -Wno-int-in-bool-context -Wno-pointer-compare -D_GNU_SOURCE -D_REENTRANT -D_FILE_OFFSET_BITS=64 -fms-extensions")

SET(CMAKE_C_FLAGS_DEBUG          "$ENV{CFLAGS} -DDEBUG -O0 -g -ggdb -gsplit-dwarf")
SET(CMAKE_C_FLAGS_RELEASE        "$ENV{CFLAGS} -DDEBUG -O3 -g -ggdb -gsplit-dwarf")
SET(CMAKE_C_FLAGS_RELWITHDEBINFO "$ENV{CFLAGS} -DDEBUG -O3 -g -ggdb -gsplit-dwarf")
SET(CMAKE_C_FLAGS_MINSIZEREL     "$ENV{CFLAGS} -DDEBUG -O3 -g -ggdb -gsplit-dwarf")

message("***************Build for ${CMAKE_BUILD_TYPE}***************")

#set(CMAKE_INSTALL_PREFIX "/usr/local/lightning")

execute_process(COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/get_version.sh ${CMAKE_CURRENT_SOURCE_DIR})

set(CMAKE_C_LIBS lightning pthread crypt crypto uuid ssl isal z m curl yajl rdmacm yaml numa ibverbs)

message("lightning: install directory ${CMAKE_INSTALL_PREFIX}, use -D CMAKE_INSTALL_PREFIX=<dir> replace it")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(lightning STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/3part/crc32.c
    ${CMAKE_CURRENT_SOURCE_DIR}/3part/hash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/3part/libringbuf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/3part/cJSON.c
    ${CMAKE_CURRENT_SOURCE_DIR}/3part/skiplist.c
    ${CMAKE_CURRENT_SOURCE_DIR}/3part/etcd-api.c
    ${CMAKE_CURRENT_SOURCE_DIR}/3part/base64_urlsafe.c

    ${CMAKE_CURRENT_SOURCE_DIR}/utils/dbg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/htab.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/lock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/misc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/timer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/pspin.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/analysis.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/etcd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/gettime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/fnotify.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/ltg_errno.c

    ${CMAKE_CURRENT_SOURCE_DIR}/mem/malloc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mem/mem_ring.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mem/hugepage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mem/ltgbuf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mem/buf_seg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mem/slab.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mem/slab_stream.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mem/slab_static.c

    #${CMAKE_CURRENT_SOURCE_DIR}/net/lib/network.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/lib/sock_passive.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/lib/sock_tcp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/lib/sock_xmit.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/lib/net_crc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/lib/net_passive.c
        
    ${CMAKE_CURRENT_SOURCE_DIR}/net/stdnet/net_rpc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/stdnet/conn.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/stdnet/sock_xnect.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/stdnet/sock_buffer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/stdnet/sdevent.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/stdnet/net_events.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/stdnet/net_lib.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/stdnet/net_table.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/stdnet/heartbeat.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/stdnet/heartbeat1.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/stdnet/xnect.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/stdnet/main_loop.c

    ${CMAKE_CURRENT_SOURCE_DIR}/net/corenet/corenet_rdma.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/corenet/corenet_connect_rdma.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/corenet/corenet_tcp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/corenet/corenet_connect_tcp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/corenet/corenet_maping.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/corenet/corenet_hb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/corenet/rdma_event.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/corenet/corenet.c

    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/stdrpc/rpc_lib.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/stdrpc/rpc_proto.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/stdrpc/rpc_passive.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/stdrpc/rpc_reply.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/stdrpc/rpc_request.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/corerpc/corerpc_lib.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/corerpc/corerpc_proto.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/corerpc/corerpc_reply.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/corerpc/corerpc_request.c

    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/lib/rpc_table.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/lib/request_proto.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/lib/reply_proto.c

    ${CMAKE_CURRENT_SOURCE_DIR}/core/core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/core_ring.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/core_latency.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/cpuset.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/sche.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/sche_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/sche_thread.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/ltg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/core_event.c
)

install(TARGETS lightning ARCHIVE DESTINATION lib)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION include FILE_PERMISSIONS OWNER_WRITE OWNER_READ GROUP_READ WORLD_READ)

add_executable(init ${CMAKE_CURRENT_SOURCE_DIR}/example/init.c)
target_link_libraries(init ${CMAKE_C_LIBS})
