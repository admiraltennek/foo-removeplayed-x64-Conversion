#define VERSION ("1.4.3-test")

#include "../_sdk/foobar2000/SDK/foobar2000.h"

#include "../_sdk/foobar2000/helpers/helpers.h"

// CHANGED: diagnostic messages in foobar2000's console (View > Console).
// Set RP_DEBUG to 0 once everything works to silence them.
#define RP_DEBUG 1
#if RP_DEBUG
#define RP_LOG(...) console::printf(__VA_ARGS__)
#else
#define RP_LOG(...) ((void)0)
#endif
// CHANGED (1.4.3): milliseconds shown in every log line, so the order and spacing of events can be seen.
#define RP_NOW ((unsigned)(GetTickCount() % 100000))

DECLARE_COMPONENT_VERSION( "Remove played Files", VERSION,
"Removes played files from playlist\n"
"By Chronial\n"
__DATE__ " - " __TIME__ )

metadb_handle_ptr g_current_track;

// CHANGED (1.4.2): true while this component has switched on foobar2000's
// "Stop after current" for the last remaining track (see on_playback_time).
// Only ever touched from the main thread.
static bool g_stop_armed = false;

// {D8882A57-C32C-47b1-AE3B-016CCAA05B7C}
static const GUID guid_removeplayed_enabled = { 0xd8882a57, 0xc32c, 0x47b1, { 0xae, 0x3b, 0x1, 0x6c, 0xca, 0xa0, 0x5b, 0x7c } };
static cfg_bool cfg_removeplayed_enabled(guid_removeplayed_enabled, true);

// {2C4FDA9F-6F29-488b-A9C4-7E341A9FC81F}
static const GUID guid_removeskipped_enabled = { 0x2c4fda9f, 0x6f29, 0x488b, { 0xa9, 0xc4, 0x7e, 0x34, 0x1a, 0x9f, 0xc8, 0x1f } };
static cfg_bool cfg_removeskipped_enabled(guid_removeskipped_enabled, true);

class my_initquit : public initquit
{
	void on_init()
	{
	}
	void on_quit()
	{
		g_current_track = 0;
	}
};

static service_factory_single_t<my_initquit> g_my_initquit;

class remove_callback : public main_thread_callback {
public:
	// CHANGED: added a constructor so the new flag always starts out false.
	remove_callback() : playlist(0), index(0), stopAfterRemoval(false) {}

	t_size playlist;
	t_size index;
	// Safety net from 1.4.1: true when playback has wrapped around onto the
	// last remaining track anyway (see removePlayedSong). Playback is then
	// stopped right after the track is removed, so it is not played twice.
	bool stopAfterRemoval;

	void callback_run(){
		static_api_ptr_t<playlist_manager> pm;
		const bit_array_one del(index);
		pm->playlist_remove_items(playlist,del);
		RP_LOG("foo_removeplayed [%u]: removed item %u from playlist %u (stop afterwards: %d)", RP_NOW, (unsigned)index, (unsigned)playlist, stopAfterRemoval ? 1 : 0);
		if (stopAfterRemoval){
			static_api_ptr_t<playback_control> pc;
			pc->stop();
		}
	}
};

// CHANGED (1.4.2): switches foobar2000's "Stop after current" on or off.
// playback_control must not be used from inside play callbacks, so this
// runs as a main thread callback.
class stop_flag_callback : public main_thread_callback {
public:
	stop_flag_callback() : arm(false) {}
	bool arm;

	void callback_run(){
		static_api_ptr_t<playback_control> pc;
		if (arm){
			// Leave it alone if the user has already switched it on themselves.
			if (!pc->get_stop_after_current()){
				pc->set_stop_after_current(true);
				g_stop_armed = true;
			}
		} else if (g_stop_armed){
			// Only ever switch off what this component switched on.
			pc->set_stop_after_current(false);
			g_stop_armed = false;
		}
		RP_LOG("foo_removeplayed [%u]: stop-after-current is now %d (set by this component: %d)", RP_NOW, pc->get_stop_after_current() ? 1 : 0, g_stop_armed ? 1 : 0);
	}
};

class my_play_callback : public play_callback_static {
public:
	my_play_callback(){
		trackSkipped = false;
		lastTime = 0;
		stopRequested = false;
	}
private:
	double lastTime;
	bool trackSkipped;
	// CHANGED (1.4.2): true while we have asked for "Stop after current"
	// because the playing track is the only one left in its playlist.
	bool stopRequested;

	// CHANGED (1.4.2): asks the main thread to switch our "Stop after current" request on or off.
	void requestStopAfterCurrent(bool arm){
		static_api_ptr_t<main_thread_callback_manager> cm;
		service_ptr_t<stop_flag_callback> callback = new service_impl_t<stop_flag_callback>;
		callback->arm = arm;
		RP_LOG("foo_removeplayed [%u]: %s stop-after-current", RP_NOW, arm ? "requesting" : "releasing");
		cm->add_callback(callback);
	}

	// CHANGED (1.4.2): true if the current track was within its last 2 seconds.
	bool playedToTheEnd(){
		if (!(g_current_track != 0)) return false;
		const double length = g_current_track->get_length();
		return length > 0 && (lastTime + 2.0) >= length;
	}

	// newTrackStarting is true when called from on_playback_new_track().
	// CHANGED (1.4.3): returns true if a removal was queued; itemCountOut (optional)
	// then receives the number of items the playlist held at that moment.
	bool removePlayedSong(bool newTrackStarting = false, t_size * itemCountOut = 0){
		bool queued = false;
		if ((!trackSkipped && cfg_removeplayed_enabled) || (trackSkipped && cfg_removeskipped_enabled)){
			t_size playlist;
			t_size songIndex;
			static_api_ptr_t<playlist_manager> pm;

			// this works in a strange way: This actually gets the info of
			// the previous track and not the new one (when called from on_playback_new_track).
			if (pm->get_playing_item_location(&playlist,&songIndex)){
				//if ((lastTime + 1) > g_current_track->get_length()){
				if (g_current_track != 0){
					static_api_ptr_t<main_thread_callback_manager> cm;
					service_ptr_t<remove_callback> callback = new service_impl_t<remove_callback>;
					callback->index = songIndex;
					callback->playlist = playlist;

					// Safety net from 1.4.1 for the replay of the last track.
					// When a new track starts, the previous track is still in the playlist.
					// If the playlist holds only that one track, the "new" track is that same
					// track coming round again (shuffle and repeat modes never run out of
					// tracks). It is about to be removed, so stop playback after removing it.
					const t_size itemCount = pm->playlist_get_item_count(playlist);
					callback->stopAfterRemoval = newTrackStarting && (itemCount <= 1);
					if (itemCountOut) *itemCountOut = itemCount;

					RP_LOG("foo_removeplayed [%u]: queued removal of item %u in playlist %u (items in playlist: %u, skipped: %d, stop afterwards: %d)",
						RP_NOW, (unsigned)songIndex, (unsigned)playlist, (unsigned)itemCount, trackSkipped ? 1 : 0, callback->stopAfterRemoval ? 1 : 0);

					cm->add_callback(callback);
					queued = true;
				}
			}
		}
		trackSkipped = false;
		return queued;
	}

public:
	//! Controls which methods your callback wants called; returned value should not change in run time, you should expect it to be queried only once (on startup). See play_callback::flag_* constants.
	virtual unsigned get_flags(){
		return play_callback::flag_on_playback_new_track
			| play_callback::flag_on_playback_time // CHANGED (1.4.2): needed to watch for the last remaining track
			| play_callback::flag_on_playback_stop;
	};

	//! Playback advanced to new track.
	virtual void on_playback_new_track(metadb_handle_ptr p_track){
		RP_LOG("foo_removeplayed [%u]: on_playback_new_track", RP_NOW);
		t_size itemCount = 0;
		const bool removalQueued = removePlayedSong(true, &itemCount); // tell it a new track is starting
		g_current_track = p_track;
		lastTime = 0;
		// CHANGED (1.4.2): a request made for the previous track must not carry over.
		if (stopRequested){
			stopRequested = false;
			requestStopAfterCurrent(false);
		}
		// CHANGED (1.4.3): the finished track is being removed. If the playlist held just
		// that track and this one, this is now the last track, so ask for "Stop after
		// current" immediately instead of waiting for the next one-second tick. Short
		// tracks can otherwise be too late: foobar2000 may already have decided what
		// plays next. (The request is queued after the removal, so it runs after it.)
		if (removalQueued && itemCount == 2){
			stopRequested = true;
			requestStopAfterCurrent(true);
		}
	};

	//! Playback stopped.
	virtual void on_playback_stop(play_control::t_stop_reason p_reason){
		RP_LOG("foo_removeplayed [%u]: on_playback_stop, reason %d", RP_NOW, (int)p_reason);

		// CHANGED (1.4.2): never let our "Stop after current" outlive this playback session.
		const bool wasRequested = stopRequested;
		if (stopRequested || g_stop_armed){
			requestStopAfterCurrent(false);
		}
		stopRequested = false;

		// was last song in playlist
		if (p_reason == play_control::stop_reason_eof){
			trackSkipped = false;
			removePlayedSong();
		} else if (p_reason == play_control::stop_reason_starting_another){
			trackSkipped = true;
			removePlayedSong();
		} else if (wasRequested && playedToTheEnd()){
			// CHANGED (1.4.2): "Stop after current" may be reported with a different
			// reason than end-of-file. If it was our request and the track was
			// essentially finished, treat it as played.
			RP_LOG("foo_removeplayed [%u]: treating this stop as the end of the last track", RP_NOW);
			trackSkipped = false;
			removePlayedSong();
		} else {
			g_current_track = 0;
		}
	};

	//! Called every second, for time display
	virtual void on_playback_time(double p_time){
		lastTime = p_time;

		// CHANGED (1.4.2): if the playing track is the only one left in its playlist,
		// ask for "Stop after current". Otherwise foobar2000 (in shuffle and repeat
		// modes) would start that same track again once it ends. This is checked every
		// second, so the request is withdrawn as soon as more tracks are added.
		bool wantStop = false;
		if (cfg_removeplayed_enabled){
			t_size playlist;
			t_size songIndex;
			static_api_ptr_t<playlist_manager> pm;
			if (pm->get_playing_item_location(&playlist,&songIndex)){
				wantStop = (pm->playlist_get_item_count(playlist) == 1);
			}
		}
		if (wantStop != stopRequested){
			stopRequested = wantStop;
			requestStopAfterCurrent(wantStop);
		}
	};

	virtual void on_playback_starting(play_control::t_track_command p_command,bool p_paused){};
	//! User has seeked to specific time.
	virtual void on_playback_seek(double p_time){};
	//! Called on pause/unpause.
	virtual void FB2KAPI on_playback_pause(bool p_state){};
	//! Called when currently played file gets edited.
	virtual void on_playback_edited(metadb_handle_ptr p_track){};
	//! Dynamic info (VBR bitrate etc) change.
	virtual void on_playback_dynamic_info(const file_info & p_info){};
	//! Per-track dynamic info (stream track titles etc) change. Happens less often than on_playback_dynamic_info().
	virtual void on_playback_dynamic_info_track(const file_info & p_info){};
	//! User changed volume settings. Possibly called when not playing.
	//! @param p_new_val new volume level in dB; 0 for full volume.
	virtual void FB2KAPI on_volume_change(float p_new_val){};
};

static service_factory_single_t<my_play_callback> g_play_callback;

class my_mainmenu_commands : public mainmenu_commands {
	// Return the number of commands we provide.
	virtual t_uint32 get_command_count() {
		return 2;
	}

	// All commands are identified by a GUID.
	virtual GUID get_command(t_uint32 p_index) {
		// {85C4F7DD-9652-4c3f-BBA7-51D9FDEE1E5D}
		static const GUID guid_enable_removecurrent = { 0x85c4f7dd, 0x9652, 0x4c3f, { 0xbb, 0xa7, 0x51, 0xd9, 0xfd, 0xee, 0x1e, 0x5d } };
		// {57829D63-8E55-485a-9E5B-E56437FCBCB3}
		static const GUID guid_enable_removeskipped = { 0x57829d63, 0x8e55, 0x485a, { 0x9e, 0x5b, 0xe5, 0x64, 0x37, 0xfc, 0xbc, 0xb3 } };

		if (p_index == 0)
			return guid_enable_removecurrent;
		else if (p_index == 1)
			return guid_enable_removeskipped;
		return pfc::guid_null;
	}

	// Set p_out to the name of the n-th command.
	// This name is used to identify the command and determines
	// the default position of the command in the menu.
	virtual void get_name(t_uint32 p_index, pfc::string_base & p_out) {
		if (p_index == 0)
			p_out = "Remove played tracks";
		else if (p_index == 1)
			p_out = "Remove skipped tracks";
	}

	// Set p_out to the description for the n-th command.
	virtual bool get_description(t_uint32 p_index, pfc::string_base & p_out) {
		if (p_index == 0)
			p_out = "Removes tracks from the Playlist once they've been played";
		else if (p_index == 1)
			p_out = "Removes tracks from the Playlist if you skip them";
		else
			return false;
		return true;
	}

	// Every set of commands needs to declare which group it belongs to.
	virtual GUID get_parent()
	{
		return mainmenu_groups::playback_etc;
	}

	// Execute n-th command.
	// p_callback is reserved for future use.
	virtual void execute(t_uint32 p_index, service_ptr_t<service_base> p_callback) {
		if (p_index == 0) {
			cfg_removeplayed_enabled = !cfg_removeplayed_enabled;
		} else if (p_index == 1) {
			cfg_removeskipped_enabled = !cfg_removeskipped_enabled;
		}
	}

	// The standard version of this command does not support checked or disabled
	// commands, so we use our own version.
	virtual bool get_display(t_uint32 p_index, pfc::string_base & p_text, t_uint32 & p_flags)
	{
		p_flags = 0;
		if (is_checked(p_index))
			p_flags |= flag_checked;
		get_name(p_index,p_text);
		return true;
	}

	// Return whether the n-th command is checked.
	bool is_checked(t_uint32 p_index) {
		if (p_index == 0)
			return cfg_removeplayed_enabled;
		else if (p_index == 1)
			return cfg_removeskipped_enabled;
		return false;
	}
};

// We need to create a service factory for our menu item class,
// otherwise the menu commands won't be known to the system.
static mainmenu_commands_factory_t<my_mainmenu_commands> foo_menu;
