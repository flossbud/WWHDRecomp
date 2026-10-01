// The FSA service in place (D18; iosu_fsa.cpp): the FS client (../coreinit/coreinit_FS.cpp) calls it
// directly, on the calling guest thread, where Cemu sent each request through IOSU's kernel to a host
// thread. The client still delivers each asynchronous reply through the IPC driver, as IOSU did, so
// the guest sees the same steps.
#pragma once
#include "Cafe/IOSU/fsa/iosu_fsa.h"
#include "Cafe/IOSU/iosu_types_common.h"

namespace iosu::fsa
{
	IOS_ERROR OpenClient();                     // a new client's handle, or an IOS error
	void CloseClient(IOSDevHandle handle);
	// one request, as IOSU's ioctl (the shim buffer's request and response) or ioctlv (vec[0] the shim
	// buffer, vec[1] the data) would have carried it
	FSA_RESULT Ioctl(IOSDevHandle handle, FSA_CMD_OPERATION_TYPE operation, FSAShimBuffer* shimBuffer);
	FSA_RESULT Ioctlv(IOSDevHandle handle, FSA_CMD_OPERATION_TYPE operation, uint32 numIn, uint32 numOut, IPCIoctlVector* vec);
}
