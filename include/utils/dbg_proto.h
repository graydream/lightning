#ifndef __LLIB_DBG_PROTO_H__
#define __LLIB_DBG_PROTO_H__

/* debug subsystems (32 bits, non-overlapping) */
#define S_UNDEF                 0x00000000
#define S_LTG_UTILS             0x00000001
#define S_LTG_MEM               0x00000002
#define S_LTG_NET               0x00000004
#define S_LTG_RPC               0x00000008
#define S_LTG_CORE              0x00000010

#define LTG_DEBUG
#define D_MSG_ON

#define __D_BUG           0x00000001
#define __D_INFO          0x00000002
#define __D_WARNING       0x00000004
#define __D_ERROR         0x00000008
#define __D_FATAL         0x00000010

#ifndef DBG_SUBSYS
#define DBG_SUBSYS S_UNDEF
#endif
#endif
