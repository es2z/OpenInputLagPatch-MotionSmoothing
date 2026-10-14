#pragma once

#include <Windows.h>
#include "games.h"

// Frame limiter based on vpatch
class Limiter {
public:
	static void Initialize(ReplayCallback replay_callback);
	static void Tick();

	// Ticks the limiter, waiting for the given amount of time instead of the limiter's own frame
	// schedule. The games that keep a clock of their own (th19, th20) hand it the time their
	// clock says is left until the next frame, so the wait ends exactly when the game wants to
	// run it: the limiter only makes the game's own pacing precise instead of fighting it with a
	// schedule that can drift out of phase with the game's clock.
	static void TickUntil(double seconds);

	// Waits until the given point in time, in performance counter units. The frame interpolation
	// uses this to place the extra presentations inside a frame.
	static void WaitUntil(LARGE_INTEGER target);

	// The games with frame interpolation present more than once per frame. The extra presentations
	// are the patch's own work and shouldn't count towards the frame time the overlay reports, so
	// the interpolation code calls this where the game's own frame ended.
	static void MarkFrameWorkEnd();

	// Re-reads the frame rate an external tool (thprac) is asking for, which UpdateTargetFPS
	// picks up from external_game_fps.
	//
	// Tick() and TickUntil() do this on every frame they pace, but a game that paces itself
	// through TickUntil only reaches it while its own clock still says the next frame is ahead of
	// it: one that has been pushed to a frame rate it can't keep up with spends every frame behind
	// its clock, so the limiter is never ticked, the external frame rate is never read again, and
	// the game stays at the rate it could not keep up with even after the tool brings the value
	// back down. The interpolation calls this once per frame, next to the game's own present, so
	// the follow keeps working in that state.
	static void PollExternalGameFPS();

	// How long one of the limiter's frames is, and what the display is running at
	static LARGE_INTEGER FrameWait();
	static unsigned int DisplayRefresh();

	// Where the current frame started, i.e. where the limiter's wait for it ended. The frame
	// interpolation spreads the extra presentations over the frame starting from here, so they
	// can't push the frame past its own length (which would make the limiter fall behind).
	static LARGE_INTEGER FrameStart();

	// How long the game's own part of the last frame took. The extra presentations cost about as
	// much as the game's own pass does, so the interpolation uses this to tell whether there is
	// still room for another presentation before the frame is over.
	static LARGE_INTEGER FrameWork();

	static bool SetGameFPS(int fps);

	// Whether this frame's present should actually be submitted.
	// True unless the current target framerate is above the display refresh rate,
	// in which case we present every Nth frame so the present can't throttle the logic.
	static bool ShouldPresent();

	// Some games (th19) keep their own clock and compute how long a frame is from the frame
	// rate. Those games read this value directly, so it has to follow the limiter's target -
	// otherwise the game's clock keeps running at 60fps no matter what GameFPS is set to.
	// See touhou19.cpp for the games that use it.
	static double game_fps;

	// The number of presentations per second the frame interpolation should be putting on the
	// display, worked out from the two interpolation settings. 0 means there is nothing to
	// interpolate:
	//   Interpolation = -1     the largest multiple of the game's frame rate the display can show
	//                          (a 180hz display and a 60fps game give 3)
	//   Interpolation = N      present N times per game frame
	//   InterpolationFPS = R   present at R frames per second (e.g. *144)
	//
	// The games that had frame interpolation before this worked it out in their own file; the ones
	// that come after can call this instead.
	static double PresentRate();

	// How many extra presentations the frame that just presented should get, and how far apart they
	// should sit (in performance counter units, 0 when there is no room for any). A rate that isn't
	// a multiple of the game's frame rate has its remainder carried over from frame to frame, so
	// the presentations still average out ("*144" with 60fps logic presents 2 and 3 alternately).
	static unsigned int ExtraPresentations(double rate, LARGE_INTEGER& spacing);

	// Whether the patch is presenting every Nth frame instead of every one, because the target
	// frame rate is above what the display can show (see ShouldPresent). The frame interpolation
	// must not add presentations while it is: the thinning is what keeps the presents from
	// throttling the logic, and extra presentations would undo it.
	static bool PresentingEveryNthFrame();

	// Those same games are the ones external tools hook to change the frame rate: thprac, for
	// example, points the engine's clock at a variable of its own. The per game code reports
	// whatever frame rate is in use there through this, so the limiter follows it instead of
	// fighting over the engine's clock. Returns 0 when there's nothing to follow.
	static UINT (*external_game_fps)();

private:
	static bool UpdateTargetFPS();

	static bool initialized;
	static LARGE_INTEGER start_time;
	static unsigned int frame_num;
	static LARGE_INTEGER wait_amount;
	static LARGE_INTEGER last_wait_amount;
	static LARGE_INTEGER blt_prepare_time;
	static LARGE_INTEGER perf_freq;
	static ReplayCallback replay_callback;
	static LARGE_INTEGER frame_start;
	static LARGE_INTEGER frame_end;
	static LARGE_INTEGER frame_work;
	static unsigned int present_every; // 1 = present every frame
	static unsigned int display_refresh;
	static bool refresh_queried;
};
