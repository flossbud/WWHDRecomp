// The debug menu (debug_menu.h): its pages, the controller's navigation and what each choice asks of
// the test aids (src/overrides/sixty.cpp). It filters the pad the game reads (KPADReadEx, input.cpp),
// so an input script drives it as a player does.
#include "debug_menu.h"
#include <mutex>

namespace
{
	using wwhd::os::input::Pad;
	namespace B = wwhd::os::input;

	// Where the menu warps: a stage, a spawn point, a room and a layer, as WWHD_DEBUG_STAGE takes them
	// (docs/handoff.md lists how each was found); boss rooms turn the refights on first.
	struct Dest { const char* label; const char* stage; int point, room, layer; bool boss; };
	constexpr Dest kIslands[] = {
		{ "Outset Island", "sea", 0, 44, -1, false },
		{ "Windfall Island", "sea", 0, 11, -1, false },
		{ "Dragon Roost Island", "Adanmae", 0, 0, -1, false },
		{ "Rito Aerie", "Atorizk", 0, 0, -1, false },
		{ "Forest Haven", "Omori", 0, 0, -1, false },
	};
	constexpr Dest kDungeons[] = {
		{ "Dragon Roost Cavern", "M_NewD2", 0, 0, -1, false },
		{ "Forbidden Woods", "kindan", 0, 0, -1, false },
		{ "Tower of the Gods", "Siren", 0, 0, -1, false },
		{ "Forsaken Fortress", "MajyuE", 0, 0, -1, false },
		{ "Earth Temple", "M_Dai", 0, 0, -1, false },
		{ "Wind Temple", "kaze", 0, 0, -1, false },
		{ "Hyrule Castle", "Hyrule", 0, 0, -1, false },
		{ "Ganon's Tower", "GanonA", 0, 0, -1, false },
	};
	constexpr Dest kBosses[] = {
		{ "Gohma (Dragon Roost Cavern)", "M_DragB", 0, 0, -1, true },
		{ "Kalle Demos (Forbidden Woods)", "kinBOSS", 0, 0, -1, true },
		{ "Gohdan (Tower of the Gods)", "SirenB", 0, 0, -1, true },
		{ "Helmaroc King (Forsaken Fortress)", "M2tower", 22, 0, 3, true },
		{ "Jalhalla (Earth Temple)", "M_DaiB", 0, 0, -1, true },
		{ "Molgera (Wind Temple)", "kazeB", 0, 0, -1, true },
		{ "Puppet Ganon (Ganon's Tower)", "GanonK", 0, 0, -1, true },
		{ "Ganondorf (Ganon's Tower)", "GTower", 0, 0, -1, true },
		// Ganon's Tower's rooms where four bosses are fought again (each boss placed there and present
		// after the warp: stage_actors.py, a tracked run)
		{ "Gohma again (Ganon's Tower)", "Xboss0", 0, 0, -1, true },
		{ "Kalle Demos again (Ganon's Tower)", "Xboss1", 0, 0, -1, true },
		{ "Jalhalla again (Ganon's Tower)", "Xboss2", 0, 0, -1, true },
		{ "Molgera again (Ganon's Tower)", "Xboss3", 0, 0, -1, true },
	};

	// enemies to spawn ahead of Link (each spawned so in a test: docs/handoff.md); anglex is more
	// parameters for some (a Darknut's 0x80: a shield and a cape)
	struct Foe { const char* label; int process; uint32 param, anglex; };
	constexpr Foe kFoes[] = {
		{ "Bokoblin", 189, 0, 0 }, { "Moblin", 188, 0, 0 }, { "Darknut", 191, 0, 0x80 }, { "Chuchu", 206, 0, 0 },
		{ "Keese", 215, 0, 0 }, { "ReDead", 224, 0, 0 }, { "Kargaroc", 181, 0, 0 },
	};

	enum Page { kTop, kIslandPage, kDungeonPage, kBossPage, kFoePage };
	struct List { const Dest* dests; int count; const char* title; };
	List ListOf(Page p)
	{
		switch (p)
		{
		case kIslandPage: return { kIslands, (int)std::size(kIslands), "Warp: islands" };
		case kDungeonPage: return { kDungeons, (int)std::size(kDungeons), "Warp: dungeons" };
		case kBossPage: return { kBosses, (int)std::size(kBosses), "Warp: bosses (refights on)" };
		case kFoePage: return { nullptr, (int)std::size(kFoes), "Spawn an enemy ahead of Link" };
		default: return { nullptr, 0, "Debug menu" };
		}
	}

	std::mutex s_lock;
	bool s_open = false, s_swallow = false;
	Page s_page = kTop;
	int s_cursor = 0;
	uint32 s_version = 1, s_prev = 0;

	// the boss rush (the owner's idea: the bosses again from a finished save, one after another): kBosses'
	// first eight in order (not Ganon's Tower's refight rooms, where a beaten boss sends Link on by itself:
	// Gohma's death there changes the stage instead of setting the bit, d_a_btd.cpp), refights on; when the boss being fought is beaten (the game sets its dungeon's "boss beaten"
	// bit, wwhd::debug::RushBossBeaten) the game's next stage change (its warp out) goes to the next one
	constexpr int kRushBosses = 8;
	static_assert(kRushBosses <= (int)std::size(kBosses));
	std::mutex s_rushLock;
	int s_rush = -1;                                   // the boss being fought, or -1
	bool s_rushBeaten = false;
	void StartRush()
	{
		{
			std::lock_guard lock(s_rushLock);
			s_rush = 0;
			s_rushBeaten = false;
		}
		wwhd::debug::SetBossRefight(true);
		const Dest& d = kBosses[0];
		wwhd::debug::RequestStage(d.stage, d.point, d.room, d.layer);
	}
	void StopRush()
	{
		std::lock_guard lock(s_rushLock);
		s_rush = -1;
	}

	std::vector<std::string> Items()
	{
		if (s_page == kTop)
			return { "Islands", "Dungeons", "Bosses (refights on)", "Boss rush (all, in order)", "Spawn an enemy",
				std::string("Boss refights: ") + (wwhd::debug::BossRefight() ? "ON" : "OFF"), "Close" };
		std::vector<std::string> items;
		if (s_page == kFoePage)
		{
			for (const Foe& f : kFoes)
				items.push_back(f.label);
			items.push_back("Back");
			return items;
		}
		List l = ListOf(s_page);
		for (int i = 0; i < l.count; i++)
			items.push_back(l.dests[i].label);
		items.push_back("Back");
		return items;
	}

	// a page's item on the top page (its cursor on the way back)
	int TopIndex(Page p)
	{
		return p == kFoePage ? 4 : (int)p - 1;
	}

	// the stick as a D-pad, so that either moves the cursor
	uint32 Directions(const Pad& p)
	{
		uint32 d = p.buttons & (B::UP | B::DOWN | B::LEFT | B::RIGHT);
		if (p.ly > 0.6f) d |= B::UP;
		if (p.ly < -0.6f) d |= B::DOWN;
		return d;
	}

	void Choose()
	{
		const int count = (int)Items().size();
		if (s_page == kTop)
		{
			switch (s_cursor)
			{
			case 0: s_page = kIslandPage; s_cursor = 0; break;
			case 1: s_page = kDungeonPage; s_cursor = 0; break;
			case 2: s_page = kBossPage; s_cursor = 0; break;
			case 3: StartRush(); s_open = false; break;
			case 4: s_page = kFoePage; s_cursor = 0; break;
			case 5: wwhd::debug::SetBossRefight(!wwhd::debug::BossRefight()); break;
			default: s_open = false; break;
			}
			return;
		}
		if (s_cursor == count - 1)
		{
			s_cursor = TopIndex(s_page);
			s_page = kTop;
			return;
		}
		if (s_page == kFoePage)
		{
			const Foe& f = kFoes[s_cursor];
			wwhd::debug::RequestSpawn(f.process, f.param, f.anglex);
			s_open = false;
			return;
		}
		const Dest& d = ListOf(s_page).dests[s_cursor];
		if (d.boss)
			wwhd::debug::SetBossRefight(true);
		StopRush();                                     // a warp of its own ends a rush
		wwhd::debug::RequestStage(d.stage, d.point, d.room, d.layer);
		s_open = false;
	}
}

namespace wwhd::debug
{
	void RushBossBeaten()
	{
		std::lock_guard lock(s_rushLock);
		if (s_rush >= 0)
			s_rushBeaten = true;
	}

	bool RushNextStage(const char*& name, int& point, int& room, int& layer)
	{
		std::lock_guard lock(s_rushLock);
		if (s_rush < 0 || !s_rushBeaten)
			return false;
		s_rushBeaten = false;
		if (++s_rush >= kRushBosses)
		{
			s_rush = -1;                                // the last one beaten: the game's own warp
			return false;
		}
		const Dest& d = kBosses[s_rush];
		name = d.stage;
		point = d.point;
		room = d.room;
		layer = d.layer;
		return true;
	}
}

namespace wwhd::os::debug_menu
{
	void Toggle()
	{
		std::lock_guard lock(s_lock);
		s_open = !s_open;
		s_page = kTop;
		s_cursor = 0;
		s_swallow = true;                        // the game sees nothing until the keys are let go
		s_version++;
	}

	bool IsOpen()
	{
		std::lock_guard lock(s_lock);
		return s_open;
	}

	void Hover(int item)
	{
		std::lock_guard lock(s_lock);
		if (!s_open || item < 0 || item >= (int)Items().size() || item == s_cursor)
			return;
		s_cursor = item;
		s_version++;
	}

	void Click(int item)
	{
		std::lock_guard lock(s_lock);
		if (!s_open || item < 0 || item >= (int)Items().size())
			return;
		s_cursor = item;
		Choose();
		if (!s_open)
			s_swallow = true;
		s_version++;
	}

	void Back()
	{
		std::lock_guard lock(s_lock);
		if (!s_open)
			return;
		if (s_page == kTop)
		{
			s_open = false;
			s_swallow = true;
		}
		else
		{
			s_cursor = TopIndex(s_page);
			s_page = kTop;
		}
		s_version++;
	}

	void Scroll(int steps)
	{
		std::lock_guard lock(s_lock);
		const int count = (int)Items().size();
		if (!s_open || !count || !steps)
			return;
		s_cursor = ((s_cursor + steps) % count + count) % count;
		s_version++;
	}

	input::Pad Filter(const input::Pad& pad)
	{
		std::lock_guard lock(s_lock);
		const uint32 now = pad.buttons | Directions(pad);
		const uint32 pressed = now & ~s_prev;
		s_prev = now;
		const uint32 both = B::LCLICK | B::RCLICK;
		if ((now & both) == both && (pressed & both))
		{
			s_open = !s_open;
			s_page = kTop;
			s_cursor = 0;
			s_swallow = true;
			s_version++;
		}
		else if (s_open)
		{
			const int count = (int)Items().size();
			const int before = s_cursor;
			const Page page = s_page;
			const bool open = s_open;
			if (pressed & B::UP)
				s_cursor = (s_cursor + count - 1) % count;
			if (pressed & B::DOWN)
				s_cursor = (s_cursor + 1) % count;
			if (pressed & (B::A | B::PLUS))            // + is Enter on the keyboard
				Choose();
			else if (pressed & B::B)
			{
				if (s_page == kTop)
					s_open = false;
				else
				{
					s_cursor = TopIndex(s_page);
					s_page = kTop;
				}
			}
			if (s_cursor != before || s_page != page || s_open != open || (pressed & B::A))
				s_version++;
			if (!s_open)
				s_swallow = true;
		}
		// while the menu is open, and until the buttons that closed it are let go, the game sees nothing
		if (s_open || s_swallow)
		{
			if (!s_open && now == 0)
				s_swallow = false;
			return input::Pad{};
		}
		return pad;
	}

	View Current()
	{
		std::lock_guard lock(s_lock);
		View v;
		v.open = s_open;
		v.version = s_version;
		if (!s_open)
			return v;
		v.title = ListOf(s_page).title;
		v.items = Items();
		v.cursor = s_cursor;
		v.hint = s_page == kTop ? "Move: D-pad/mouse   Choose: A/click   Close: B/F1"
		                        : "Move: D-pad/mouse   Pick: A/click   Back: B/right click";
		return v;
	}
}
