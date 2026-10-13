// Save states (q-savestate, session qa; the owner's ask, 2026-10-11: reproduce one-off bugs such as B16 on another
// machine, and test from any moment). A state is the game's memory, every guest thread's registers and the host-side
// records the OS layer, GX2, the sound and the 60 fps layer keep outside it, taken at one save point: the top of a
// whole tick's frame (fw_procFrame, f_0274C264), the main thread parked there (suspended) and every other guest thread
// at a point it can resume from what guest memory holds (savestate.cpp: "at rest"). A load boots the same build as
// usual, parks at the same point, puts the memory and the records back and builds the game's threads anew: each
// resumes from its saved context (the dispatcher's interpreter fallback runs a function's remainder;
// docs/recompiler-design.md D5), or runs the OS call it was waiting in again. The renderer's own images are not
// kept: the picture rebuilds within a frame or two.
//
// WWHD_STATE_SAVE=SWAP:PATH saves at the first whole tick from that swap (the game goes on); WWHD_STATE_LOAD=PATH
// loads at the first save point where the game's threads are the state's. savestate.cpp.
#pragma once
#include "Common/precompiled.h"
#include <string>
#include <algorithm>
#include <type_traits>
#include <unordered_map>
#include <vector>

struct PPCInterpreter_t;
struct OSThread_t;

namespace wwhd::state
{
	// fw_procFrame's entry on a whole tick (overrides/sixty.cpp): parks the main thread when a save, a load or a census
	// is due (cheap otherwise)
	void AtFrameTop(PPCInterpreter_t* ctx);
	// the main core's idle loop (coreinit_Thread.cpp), before the virtual clock moves on: true when it took the parked
	// main thread's save point, with the scheduler's lock not held
	bool Idle();
	// a guest thread's fiber starts (coreinit_Thread.cpp, its context loaded): a thread a load built anew that waited
	// in an OS call runs the call again, without the cycles its first run was charged
	void FiberStart(PPCInterpreter_t* hCPU);
	// every switch-out (coreinit_Thread.cpp, scheduler lock held): whether the thread stopped at an exact point (a
	// ready thread is resumable from its saved IP only then: in the OS layer or an override's host code it's stale)
	void NoteSwitchOut(OSThread_t* thread, bool exact);
	// a save, load or census is waiting for the game to come to rest (task_loop.cpp skips its idle waits meanwhile)
	bool Pending();

	// The debug menu's slots (debug_menu.cpp; the owner's choice: four, in the game folder's portable/states, never in
	// git: a state is game memory). SetDir at start (cemu_boot.cpp); a request is taken at the next whole tick's top
	// (the game goes on meanwhile), its outcome in Status
	constexpr int kSlots = 4;
	void SetDir(const std::string& portableDir);
	void RequestSave(int slot);                    // 1..kSlots
	void RequestLoad(int slot);
	std::string SlotInfo(int slot);                // "empty", or when it was saved
	std::string SlotFile(int slot);                // its file (portable/states/slotN.state; "" before SetDir)
	// The pause menu (t-pausemenu): RequestHold(true) parks the game at the next whole tick's top, as a save does, and
	// keeps it there (Held) until RequestHold(false); a slot's save or load asked for meanwhile is made there and the
	// game stays held after it. While held the sound is silent (ax_out.cpp) and the game reads no input
	void RequestHold(bool on);
	bool Held();
	// The start screen (pause_menu.cpp): the game's sound stopped where it is, held or not: no AX frames, so its sound
	// threads (driven by AX's frame callbacks) and the song they play wait, and the device plays nothing (ax_out.cpp)
	void PauseSound(bool on);
	bool SoundPaused();
	// the pause menu's File 1 save: while held, the main thread is woken to run fn (the game's own save: SetGameSave,
	// overrides/sixty.cpp) at the tick's top, then parked again; the save's own thread writes the file meanwhile
	void SetGameSave(void (*fn)(PPCInterpreter_t*), uint32 managerAt);
	void RequestGameSave();
	int GameSaveStatus();                          // 0 none, 1 under way, then once: 2 saved, -1 failed
	std::string Status();                          // the last request's outcome ("" none yet)

	// One module's host-side state, written and read by the same function (a record): on a save Pod/Bytes append what
	// they're given, on a load they overwrite it from the state. A record checks what must match (Expect), and Fail
	// refuses the state with a reason (the load stops before anything is touched when it can).
	class Archive
	{
	public:
		explicit Archive(bool loading) : m_loading(loading) {}
		bool Loading() const { return m_loading; }
		bool Ok() const { return m_error.empty(); }
		const std::string& Error() const { return m_error; }
		void Fail(std::string why) { if (m_error.empty()) m_error = std::move(why); }

		void Bytes(void* p, size_t n)
		{
			if (!m_loading)
			{
				const uint8* b = (const uint8*)p;
				m_data.insert(m_data.end(), b, b + n);
				return;
			}
			if (!Ok() || m_pos + n > m_data.size())
			{
				Fail("the state ends early");
				memset(p, 0, n);
				return;
			}
			memcpy(p, m_data.data() + m_pos, n);
			m_pos += n;
		}
		template <typename T> void Pod(T& v)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			Bytes(&v, sizeof(T));
		}
		template <typename T> void Vec(std::vector<T>& v)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			uint32 n = (uint32)v.size();
			Pod(n);
			if (m_loading)
				v.resize(Ok() ? n : 0);
			if (!v.empty())
				Bytes(v.data(), v.size() * sizeof(T));
		}
		// a map of plain keys and values, sorted by key (the same bytes for the same contents)
		template <typename K, typename V> void Map(std::unordered_map<K, V>& m)
		{
			static_assert(std::is_trivially_copyable_v<K> && std::is_trivially_copyable_v<V>);
			struct Entry { K k; V v; };
			std::vector<Entry> e;
			for (auto& [k, v] : m)
				e.push_back({ k, v });
			std::sort(e.begin(), e.end(), [](const Entry& a, const Entry& b) { return a.k < b.k; });
			Vec(e);
			if (m_loading && Ok())
			{
				m.clear();
				for (auto& x : e)
					m.emplace(x.k, x.v);
			}
		}
		void Str(std::string& s)
		{
			uint32 n = (uint32)s.size();
			Pod(n);
			if (m_loading)
				s.resize(Ok() ? n : 0);
			if (!s.empty())
				Bytes(s.data(), s.size());
		}
		// a value the loading process must already have (the same build, the same boot): saved, then compared
		template <typename T> void Expect(const T& now, const char* what)
		{
			T v = now;
			Pod(v);
			if (m_loading && Ok() && memcmp(&v, &now, sizeof(T)) != 0)
				Fail(std::string(what) + " differs from this process's");
		}

		std::vector<uint8>& Data() { return m_data; }
		bool AtEnd() const { return m_pos == m_data.size(); }

	private:
		bool m_loading;
		std::vector<uint8> m_data;
		size_t m_pos = 0;
		std::string m_error;
	};

	// A module's record, registered at static initialization (WWHD_STATE_RECORD below); the records run in the order of
	// their tags, the same on a save and a load. A change to what a record carries (a field more or less, a carried
	// type's layout) bumps kVersion (savestate.cpp): a state of the old format is then refused before anything is
	// applied. Without it the load fails in that record, guest memory already the state's, and the process stops
	using RecordFn = void (*)(Archive& ar);
	struct Record
	{
		Record(const char* tag, RecordFn fn);
	};
}

#define WWHD_STATE_RECORD_CAT2(a, b) a##b
#define WWHD_STATE_RECORD_CAT(a, b) WWHD_STATE_RECORD_CAT2(a, b)
// WWHD_STATE_RECORD("tag", fn): fn(Archive&) carries this module's host state in save states
#define WWHD_STATE_RECORD(tag, fn) static wwhd::state::Record WWHD_STATE_RECORD_CAT(s_stateRecord_, __LINE__)(tag, fn)
