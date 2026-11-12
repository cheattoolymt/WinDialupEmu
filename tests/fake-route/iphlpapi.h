#include "vmodem/vm_winsock.h"
typedef struct {
    unsigned Family;
    NET_LUID InterfaceLuid;
    uint32_t InterfaceIndex, SitePrefixLength, Metric, OtherPolicy;
    BOOLEAN DisableDefaultRoutes;
} MIB_IPINTERFACE_ROW;
void InitializeIpInterfaceEntry(MIB_IPINTERFACE_ROW *);
DWORD GetIpInterfaceEntry(MIB_IPINTERFACE_ROW *);
DWORD SetIpInterfaceEntry(MIB_IPINTERFACE_ROW *);
