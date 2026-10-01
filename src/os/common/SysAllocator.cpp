// SysAllocator.cpp (docs/recompiler-design.md D18): SysAllocators: Cemu's OS objects in guest
// memory, laid out as in the reference. Cemu's src/Common/SysAllocator.cpp (Mozilla Public License
// 2.0), forked: wwhd-null links this in place of Cemu's object, under Cemu's names (so Cemu's code
// that calls it reaches ours). Route traces, GPU commands and sound must stay as Cemu's
// (tools/reference/stream_check.sh).
#include "SysAllocator.h"
#include "sysalloc_layout.h"
#include <map>
#include <tuple>

extern MPTR sysAreaAllocatorOffset; // coreinit_MEM.cpp: the next free byte of the Cemu area

namespace
{
	// wwhd: what a slot is, as sysalloc_layout.h knows it: the file that declared it (with the line,
	// for a header), its size and alignment
	using SlotKey = std::tuple<std::string, uint32, uint32, uint32>;

	SlotKey KeyOf(const SysAllocatorBase* slot)
	{
		std::string_view path = slot->DeclaredFile();
		std::string name{path.substr(path.find_last_of("/\\") + 1)};
		bool header = name.ends_with(".h") || name.ends_with(".hpp");
		return {name, header ? slot->DeclaredLine() : 0, slot->SlotSize(), slot->SlotAlignment()};
	}
}

// wwhd: each slot goes where the reference has it (sysalloc_layout.h), found by what it is, not by
// when its static constructor registered it (the link's order, which used to have to be the
// reference's); a slot the layout doesn't know goes after the reference's, and the Cemu area's bump
// allocator then carries on from there. WWHD_SYSALLOC_ORDER=link lays them out in registration order,
// as Cemu does. CEMU_SYSALLOC_LOG=path lists the slots as laid out (cemu-patches/0016's format).
void SysAllocatorContainer::Initialize()
{
	const char* order = getenv("WWHD_SYSALLOC_ORDER");
	const bool byLink = order && std::string_view(order) == "link";
	std::map<std::tuple<std::string, uint32, uint32, uint32, uint32>, uint32> layout;
	for (const SysAllocatorSlot& slot : kSysAllocatorLayout)
		layout[{slot.file, slot.line, slot.size, slot.alignment, slot.ordinal}] = slot.offset;
	std::map<SlotKey, uint32> ordinals;
	uint32 end = kSysAllocatorLayoutEnd;
	FILE* log = getenv("CEMU_SYSALLOC_LOG") ? fopen(getenv("CEMU_SYSALLOC_LOG"), "w") : nullptr;
	for (SysAllocatorBase* sysAlloc : m_sysAllocList)
	{
		uint32 align = std::max<uint32>(sysAlloc->SlotAlignment(), 1);
		uint32 offset = (sysAreaAllocatorOffset + align - 1) / align * align;
		if (!byLink)
		{
			SlotKey key = KeyOf(sysAlloc);
			uint32 ordinal = ordinals[key]++;
			auto it = layout.find(std::tuple_cat(key, std::make_tuple(ordinal)));
			if (it != layout.end())
				offset = it->second;
			else
			{
				offset = (end + align - 1) / align * align;
				end = offset + ((sysAlloc->SlotSize() + 3) & ~3);
				cemuLog_log(LogType::Force, "SysAllocator: {}:{} ({} bytes) isn't in sysalloc_layout.h; it goes after the reference's slots, at offset {:#x}",
					sysAlloc->DeclaredFile(), sysAlloc->DeclaredLine(), sysAlloc->SlotSize(), offset);
			}
			sysAreaAllocatorOffset = offset;   // the slot's own allocation takes exactly this
		}
		sysAlloc->Initialize();
		if (log)
			fprintf(log, "%s\t%u\t%u\t%u\t%08x\n", sysAlloc->DeclaredFile(), sysAlloc->DeclaredLine(), sysAlloc->SlotSize(), sysAlloc->SlotAlignment(), offset);
	}
	if (!byLink)
		sysAreaAllocatorOffset = end;
	if (log)
	{
		fprintf(log, "end\t0\t0\t0\t%08x\n", (uint32)sysAreaAllocatorOffset);
		fclose(log);
	}
}

void SysAllocatorContainer::PushSysAllocator(SysAllocatorBase* base)
{
	m_sysAllocList.push_back(base);
}

SysAllocatorContainer& SysAllocatorContainer::GetInstance()
{
	static SysAllocatorContainer s_instance;
	return s_instance;
}

SysAllocatorBase::SysAllocatorBase(const std::source_location& where, uint32 slotSize, uint32 slotAlignment)
	: m_declaredFile(where.file_name()), m_declaredLine(where.line()), m_slotSize(slotSize), m_slotAlignment(slotAlignment)
{
	SysAllocatorContainer::GetInstance().PushSysAllocator(this);
}