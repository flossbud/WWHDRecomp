// Network and account services with no state to share (os.h, D18). The game runs offline with one
// user in slot 1; these only report that. The rest of nn_ac, nn_act, nn_boss, nn_olv, nlibcurl and
// nsysnet stays Cemu's until each library moves whole: their functions share state (accounts
// loaded by nn_act.Initialize, curl and socket setup, boss and olv objects in Cemu's memory).
#include "os.h"

namespace
{
	using namespace wwhd::os;

	constexpr uint32 kModuleAc = 13;
	constexpr uint32 kAcSuccess = kModuleAc << 20;   // nn::Result: level success, module nn_ac
}

// nn::Result nn::ac::Initialize(void)
WWHD_OS_FUNCTION_NAMED(nn_ac, "Initialize__Q2_2nn2acFv", nn_ac_Initialize) { Return(ctx, kAcSuccess); }

// nn::Result nn::ac::Connect(void): a plain 0, as games expect (Cemu's note: Terraria, Splatoon)
WWHD_OS_FUNCTION_NAMED(nn_ac, "Connect__Q2_2nn2acFv", nn_ac_Connect) { Return(ctx, 0); }

// uint8 nn::act::GetSlotNo(void): the user, slot 1 (slots count from 1)
WWHD_OS_FUNCTION_NAMED(nn_act, "GetSlotNo__Q2_2nn3actFv", nn_act_GetSlotNo) { Return(ctx, 1); }

// nn::Result nn::act::GetParentalControlSlotNoEx(uint8* slot, uint8 user): slot 1
WWHD_OS_FUNCTION_NAMED(nn_act, "GetParentalControlSlotNoEx__Q2_2nn3actFPUcUc", nn_act_GetParentalControlSlotNoEx)
{
	Write8(Arg(ctx, 0), 1);
	Return(ctx, 0);
}
