// Touhou 10: Mountain of Faith v1.00a

#include <stdio.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "config.h"
#include "overlay.h"
#include "tool_state.h"
#include "touhou10.h"

using namespace Touhou10;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x00477838;

int __stdcall th10_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__Update(self);
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// th10 draws a game frame from a single place, unlike the later games, where the same block is
// repeated once per scene: it sits inside the window update itself, CWindowManager__Update at
// 0x439390, which the main loop calls once per frame with the window object at 0x4924F0 (0x438D2C
// pushes it and calls; that call is what the patch replaces with th10_window_update_hook). Every
// screen the game can be in - the title, the menus, a stage - is drawn by this one function, so
// this is also where the menus get their extra presentations.
//
// The frame function starts and ends the scene the way the later games do:
//
//     0x43948E  mov eax, [0x491C30]      the device
//     0x439496  call [ecx+0xA4]          BeginScene
//     0x43949C  ...                      reset the sprite queue, inlined here (three stores)
//     0x4394BD  mov edi, 0x491C28        the render context, for the call below
//     0x4394C2  [0x492378] = 0xFF
//     0x4394CC  FUN_00420CD0             prepare the render context (its object in EDI)
//     0x4394D8  FUN_00449D40             render every object (they fill the sprite queue)
//     0x4394DD  mov esi, [0x491C10]      the sprite queue's owner, for the call below
//     0x4394E3  FUN_00442F50             draw the sprite queue (its object in ESI)
//     0x4394F4  call [ecx+0x104]         SetTexture(0, NULL)
//     0x439502  call [edx+0xA8]          EndScene
//     0x439508  ...                      the frame counter goes back to 0
//     0x43950C  FUN_004391F0             present
//
// The block runs on every call of the function - the counter it is gated by, [0x491D66] plus one
// compared against the window object's +0x14, always passes, because [0x491D66] is a byte that is
// never written anywhere in the executable and starts at 0 (the three places that read it are
// 0x41366A, 0x4208DC and 0x43947B, all reads), the same "one update per rendered frame" the later
// games have. What does gate the block is the value the update walk returns: only 1 renders, so a
// screen that is being torn down or switched presents nothing that frame.
//
// The device is [0x491C30], the sprite queue's owner is [0x491C10], and the queue itself is a
// vertex buffer inside that owner: 0x3ADAC8 is how many sprites it holds, 0x3ADACC is where it
// starts, 0x72DACC is where the next sprite goes and 0x72DAD0 is the start of the batch that is
// drawn (FUN_00442F50 draws from it and then moves it up to the write cursor, which is what makes
// the report of "nothing to draw" a count of 0). The inlined reset is those three stores.
//
// Two of the calls take their object in a register instead of on the stack, which is how the
// compiler that built this game left them, so they can't be reached through a function pointer -
// the trampolines below are what put the object where each one expects it. Both registers are
// callee-saved and are put back before returning: leaving them holding the object hands the caller
// something other than what it had, which is what broke the th13 version of this code (it crashed
// in the SetTexture right after the sprite queue was drawn).
//
// The extra presentations draw the state the game is already in, which is the "moved forward by
// zero frames" case; stage 1 is what puts anything of the game's own in motion inside them.

static void* const th10_prepare_render_address = (void*)0x00420CD0;
static void* const th10_draw_sprite_queue_address = (void*)0x00442F50;

static void __declspec(naked) th10_prepare_render() {
	__asm {
		push edi
		mov edi, 0x00491C28
		call dword ptr [th10_prepare_render_address]
		pop edi
		ret
	}
}

static void __declspec(naked) th10_draw_sprite_queue() {
	__asm {
		push esi
		mov esi, dword ptr ds:[0x00491C10]
		call dword ptr [th10_draw_sprite_queue_address]
		pop esi
		ret
	}
}

// The call the render block makes into the game: the render walk, which runs every drawing task and
// so fills the sprite queue. It takes the scene object ([0x491BE4]) on the stack and cleans it up
// itself (its `ret 4` is at 0x449E2E).
static auto th10_render_objects = (int(__stdcall*)(void*))0x00449D40;
static void** const th10_scene = (void**)0x00491BE4;

// The update walk, which is what runs the game's own logic (the enemies, the bullets, the player,
// the timers). It takes the scene object and cleans it up itself as well (its `ret 4` is at
// 0x449D0E). Only counted here, so the diagnostics can show how much logic the game ran for a
// frame - the walk itself runs exactly as it always did.
static auto th10_update_objects = (int(__stdcall*)(void*))0x00449C00;
static unsigned int th10_frame_updates;

static int __stdcall th10_update_hook(void* scene) {
	++th10_frame_updates;
	return th10_update_objects(scene);
}

// The stage object and, at +0x38, the timer the stage counts its own frames in (the one thprac
// writes to when it starts stage 4 forward, see its th10 module): a second way to see how fast the
// game's own logic is going, straight out of the game's data.
static void** const th10_stage = (void**)0x004776E8;
static const size_t th10_stage_timer = 0x38;

// Where the user's tool builds its frame and where it draws it. thprac's th10 module starts its
// frame from a hook on the return of the update walk at 0x449D0E and draws the overlay it built
// from a hook on the return of the render walk at 0x449E2E; both are the `ret 4` of the walk the
// frame function calls (FUN_00449C00 and FUN_00449D40), which is why the tool state is told about
// them: 0xC2 is what those two bytes hold while no tool is loaded, and the code there takes the
// scene off the stack when it returns (see tool_state.h).
static unsigned char* const th10_tool_frame_address = (unsigned char*)0x00449D0E;
static unsigned char* const th10_tool_draw_address = (unsigned char*)0x00449E2E;
static const unsigned char th10_tool_plain_byte = 0xC2;

// What the scene is drawn with, taken from the frame the game's own render ran in. The frame
// function, right before it renders, makes its 2D context the current one (0x491E94 into
// [0x491FAC]), calls FUN_00421480 on it - which builds the view and projection matrices in the
// context (at +0x4C and +0x8C), hands them to SetTransform, and copies the two values at
// +0xE8/+0xEC of it into the sprite manager (at +0x5C/+0x60), which is what places the sprite
// queue on screen - and sets the viewport from it (at 0x439419, SetViewport with the context's
// +0xCC). So what an extra presentation has to put back is whatever the game's frame left: the
// user's tool draws its overlay with a viewport, a projection and a screen placement of its own,
// and an extra presentation runs after that.
//
// FUN_00421480 is not called again for the extra presentations for the same reason th11 doesn't
// call its own camera: it draws the sprite queue on its way in before it does anything else (the
// first thing it does is FUN_00442F50 when the sprite manager exists).
static D3DVIEWPORT9 th10_scene_viewport = {};
static D3DMATRIX th10_scene_view = {};
static D3DMATRIX th10_scene_projection = {};
static bool th10_scene_transforms = false;
static void* const th10_render_context = (void*)0x00491FAC;  // the current 2D context
static void* const th10_sprite_manager = (void*)0x00491C10;
static DWORD th10_scene_context;
static DWORD th10_scene_sprite_offset[2];
static const size_t th10_sprite_screen_offset = 0x5C;        // in the sprite manager

// The device the game draws with. The present and render sites load it from the game's own global,
// so that is the device that has to present the frame - not the one the patch's D3D9 hook
// captured, which is the same object while that hook is in place and nothing at all if the game
// got its device in a way the hook doesn't see. Reporting a failed present to the game ends it
// (its device-lost handling takes over), so the device the game itself would have presented with
// comes first.
static IDirect3DDevice9* th10_device() {
	IDirect3DDevice9* device = *(IDirect3DDevice9**)0x00491C30;
	if (device != nullptr)
		return device;
	return d3d9_hooked_device();
}

// The menus draw their text through two lists of records the game keeps in the text object it holds
// at [0x4776E0]: the game's own printf-for-text call appends a record to one of them (0x68 bytes
// each, the first at +0x76C with its count at +0x896C, the second at +0x6F6C with its count at
// +0x8970 - FUN_00401530 appends to the first and FUN_004015C0 to the second, both of them bumping
// the count of theirs), and the drawing then walks the records that count covers (FUN_00401760 for
// the first list, whose loop at 0x4019CB compares its index with [obj+0x896C], and FUN_00401A50 for
// the second, at 0x401C19 with [obj+0x8970]). Both counts are cleared once per frame by
// FUN_004014F0, which is one of the object's own update tasks and so part of what the update walk
// does - which an extra presentation does not run. So an extra frame that draws the same screen
// while the lists still hold the records the game's own frame appended draws that text a second
// time on top of itself, which the alpha blending these screens use turns into text that looks a
// little denser than the game's own frame shows (and, on the screens whose layout state moves
// between records, into a visible second copy).
//
// So the counts the game's own run of the walk started from are kept here and put back before every
// extra presentation: each one then draws the records the game's own frame drew.
static const size_t th10_text_records = 0x896C;
static const size_t th10_text_records_count = 2;
static void* const th10_text_object = (void*)0x004776E0;
static unsigned int th10_text_saved[th10_text_records_count];

static void th10_text_records_save() {
	void* text = *(void**)th10_text_object;
	for (size_t i = 0; i < th10_text_records_count; ++i)
		th10_text_saved[i] = text != nullptr
			? *(unsigned int*)((char*)text + th10_text_records + i * 4) : 0;
}

static void th10_text_records_restore() {
	void* text = *(void**)th10_text_object;
	if (text == nullptr)
		return;
	for (size_t i = 0; i < th10_text_records_count; ++i)
		*(unsigned int*)((char*)text + th10_text_records + i * 4) = th10_text_saved[i];
}

// The game keeps its own frame rate in an object of its own (fps.cpp: FUN_00413350 allocates it and
// puts it at [0x477708], and its task - the draw callback at 0x413690 - is on the render chain, so
// every run of the render walk runs it). The task counts the frames in a one second window (the
// counter at +0x20, advanced by [0x491D66] + 1 every time it runs) and works the rate out from that
// count and the window's own clock (FUN_004134B0), which is what it shows as "%2.1ffps". The rate
// is also acted on: when it goes above the threshold the game has at [0x470BB8] it treats the frame
// as having run too fast and, after a couple of those in a row, re-syncs its own clock from the
// performance counter (and clears that counter's base, which makes its next frame rate read as 0).
//
// An extra presentation is another run of the render walk, so without putting this state back the
// game counts three frames for every one it runs, shows 180, and ends up re-syncing its clock from
// it. The counters are put back the way the text records are: what the game's own frame left is
// kept, and written back around each extra presentation, so the extras are presentations and
// nothing else - they show the rate the game's own frame showed and never count as frames.
static void** const th10_fps = (void**)0x00477708;
static const size_t th10_fps_window = 0x14;       // where the current window started, a double
static const size_t th10_fps_state = 0x14;        // from here (the window start, a double) up to
static const size_t th10_fps_state_size = 0x24;   // +0x38: the window, the "too fast" count, the
                                                  // frame counter, two more doubles and the rate
static unsigned char th10_fps_saved[0x24];
static bool th10_fps_saved_valid;

static void th10_fps_save() {
	void* fps = *th10_fps;
	th10_fps_saved_valid = fps != nullptr;
	if (th10_fps_saved_valid)
		memcpy(th10_fps_saved, (char*)fps + th10_fps_state, th10_fps_state_size);
}

// as_presentation is set when this is being put back for an extra presentation rather than for the
// game's own accounting: the rate the game shows is worked out once a one second window, and the
// game's own pass works it out from the count as it is *before* the pass puts itself into it (the
// bump is the task's last step). A presentation a few milliseconds later would find that window
// over, work the rate out again from the count that now has that pass in it, and show a rate one
// count higher for its one presentation (60.0 turning into 60.7 for five milliseconds, once a
// second - the little flicker in the corner). Its window start is left reading as the future so
// the guard at the top of the rate takes it as "the window starts now", which leaves the elapsed
// at 0: the rate is not worked out again and the value the game's own pass showed stays in place.
static void th10_fps_restore(bool as_presentation) {
	void* fps = *th10_fps;
	if (!th10_fps_saved_valid || fps == nullptr)
		return;
	memcpy((char*)fps + th10_fps_state, th10_fps_saved, th10_fps_state_size);
	if (as_presentation) {
		const double future = 1.0e300;
		memcpy((char*)fps + th10_fps_window, &future, sizeof(future));
	}
}

// Puts the sprite queue back to empty, the way the frame function inlines it (0x43949C)
static void th10_reset_sprite_queue() {
	void* manager = *(void**)th10_sprite_manager;
	if (manager == nullptr)
		return;
	DWORD base = (DWORD)manager + 0x3ADACC;
	*(DWORD*)((char*)manager + 0x3ADAC8) = 0;
	*(DWORD*)((char*)manager + 0x72DACC) = base;
	*(DWORD*)((char*)manager + 0x72DAD0) = base;
}

// Remembers what the scene is drawn with, then lets the game's own render run exactly as it always
// did (see th10_extra_presentation for what it's for)
static int __stdcall th10_render_hook(void* scene) {
	IDirect3DDevice9* device = th10_device();
	if (device != nullptr) {
		device->GetViewport(&th10_scene_viewport);
		th10_scene_transforms = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th10_scene_view)) &&
			SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th10_scene_projection));
	}
	th10_scene_context = *(DWORD*)th10_render_context;
	void* manager = *(void**)th10_sprite_manager;
	if (manager != nullptr) {
		th10_scene_sprite_offset[0] = *(DWORD*)((char*)manager + th10_sprite_screen_offset);
		th10_scene_sprite_offset[1] = *(DWORD*)((char*)manager + th10_sprite_screen_offset + 4);
	}

	ToolState::WatchFrameReady(th10_tool_frame_address, th10_tool_draw_address,
		th10_tool_plain_byte, true);

	// What the game's own run of the walk finds the text lists at, for the extra presentations to
	// put back (see th10_text_records)
	th10_text_records_save();

	return th10_render_objects(scene);
}

// The number of presentations per second the config asks for. 0 means interpolation is off.
static double th10_present_rate() {
	if (Config::InterpolationFPS)
		return (double)Config::InterpolationFPS;

	int multiplier = Config::Interpolation;
	if (multiplier < 0) {
		UINT display = Limiter::DisplayRefresh();
		UINT logic = (UINT)(Limiter::game_fps + 0.5);
		if (display == 0 || logic == 0)
			return 0.0;
		multiplier = (int)(display / logic);
	}
	if (multiplier < 2)
		return 0.0;
	return Limiter::game_fps * (double)multiplier;
}

// Diagnostics: press U to write the presentation timing of the last frames to oilp_diag.txt
static const unsigned int th10_diag_frames_max = 300;
static const unsigned int th10_diag_presents_max = 8;
struct Th10DiagFrame {
	LARGE_INTEGER base;
	unsigned int presents;
	unsigned int updates;      // how many times the game's own update walk ran in this frame
	int stage_timer;           // the stage's own frame counter at the end of it
	float game_rate;           // the frame rate the game itself worked out (fps.cpp), and its count
	int game_frames;
	LARGE_INTEGER present_time[th10_diag_presents_max];
};
static Th10DiagFrame th10_diag_frames[th10_diag_frames_max];
static unsigned int th10_diag_count;
static bool th10_diag_key_down;

static LARGE_INTEGER th10_perf_freq;

static void th10_diag_dump() {
	wchar_t path[1024] = {};
	if (!GetModuleFileNameW(NULL, path, MAX_PATH))
		return;
	wchar_t* slash = wcsrchr(path, L'\\');
	if (slash)
		slash[1] = L'\0';
	wcscat_s(path, L"oilp_diag.txt");

	FILE* file = nullptr;
	if (_wfopen_s(&file, path, L"w") != 0 || file == nullptr)
		return;

	unsigned int count = th10_diag_count < th10_diag_frames_max ? th10_diag_count : th10_diag_frames_max;
	unsigned int start = th10_diag_count < th10_diag_frames_max ? 0 : th10_diag_count % th10_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, th10_present_rate());
	fprintf(file, "frames recorded: %u\n", count);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	unsigned int updates_min = 0, updates_max = 0;
	int stage_timer_first = 0, stage_timer_last = 0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th10DiagFrame& frame = th10_diag_frames[(start + i) % th10_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s) %u update(s)", i, frame.presents, frame.updates);
		for (unsigned int p = 0; p < frame.presents && p < th10_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 / (double)th10_perf_freq.QuadPart);
		fprintf(file, " stage timer %d", frame.stage_timer);
		fprintf(file, " game rate %.1f (%d counted)", frame.game_rate, frame.game_frames);
		fprintf(file, "\n");
		if (i == 0) {
			updates_min = updates_max = frame.updates;
			stage_timer_first = frame.stage_timer;
		}
		if (frame.updates < updates_min)
			updates_min = frame.updates;
		if (frame.updates > updates_max)
			updates_max = frame.updates;
		stage_timer_last = frame.stage_timer;
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	fprintf(file, "updates per frame: %u to %u\n", updates_min, updates_max);
	fprintf(file, "stage timer: %d to %d over %u frame(s)\n",
		stage_timer_first, stage_timer_last, count);
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count, presents * (double)th10_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// One extra presentation of the frame: the same render block the game runs, once more, so the state
// the game is in is drawn and presented again. The tool's frame is readied again, so the object
// render below draws its overlay into this presentation the same way it does in the game's own one.
static void th10_extra_presentation(IDirect3DDevice9* device, float t) {
	(void)t; // stage 1 is what moves anything; this draws the state the game is already in

	device->BeginScene();
	if (th10_scene_transforms) {
		device->SetTransform(D3DTS_VIEW, &th10_scene_view);
		device->SetTransform(D3DTS_PROJECTION, &th10_scene_projection);
	}
	if (th10_scene_viewport.Width != 0)
		device->SetViewport(&th10_scene_viewport);
	// The 2D context the game's own frame rendered with, and the screen placement it left in the
	// sprite queue (see th10_scene_viewport)
	*(DWORD*)th10_render_context = th10_scene_context;
	{
		void* manager = *(void**)th10_sprite_manager;
		if (manager != nullptr) {
			*(DWORD*)((char*)manager + th10_sprite_screen_offset) = th10_scene_sprite_offset[0];
			*(DWORD*)((char*)manager + th10_sprite_screen_offset + 4) = th10_scene_sprite_offset[1];
		}
	}
	th10_reset_sprite_queue();
	*(DWORD*)0x00492378 = 0xFF;
	th10_prepare_render();
	ToolState::ReadyToDraw();
	// The text lists go back to how the game's own run of the walk found them, so this presentation
	// draws the same text that one drew (see th10_text_records above)
	th10_text_records_restore();
	// Same for the game's own frame rate accounting: this presentation shows the rate the game's
	// own frame showed and isn't counted as a frame of its own (see th10_fps above)
	th10_fps_restore(true);
	// Nothing to render into an empty scene, and the walk itself would read the object it is handed
	void* scene = *th10_scene;
	if (scene != nullptr)
		th10_render_objects(scene);
	// The walk just ran the game's rate task, so what it made of that has to go back as well
	th10_fps_restore(false);
	th10_draw_sprite_queue();
	device->SetTexture(0, nullptr);
	device->EndScene();

	// The extras go through the device's own Present, without the hooks a tool put in front of it
	// (see d3d9_present_bypassing_hooks). If the patch's D3D9 hook isn't on this device there is no
	// saved original to use, and the game's own present call is what is left.
	if (d3d9_hooked_device() == device)
		d3d9_present_bypassing_hooks();
	else
		device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();
}

// Presents the frame the game just drew, and then the extra presentations the config asked for.
static HRESULT th10_present_hook() {
	IDirect3DDevice9* device = th10_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th10_perf_freq);
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	HRESULT result = device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();
	Limiter::MarkFrameWorkEnd();
	ToolState::WatchFrameDrawn();

	// What the game's own frame left its rate accounting at, for the extra presentations to put back
	// (see th10_fps); taken here because this is the point the game's own frame is over
	th10_fps_save();

	unsigned int presents = 1;
	LARGE_INTEGER present_time[th10_diag_presents_max];
	present_time[0] = base;

	double rate = th10_present_rate();
	if (SUCCEEDED(result) && rate > 0.0 && Limiter::game_fps > 0.0) {
		static bool rate_printed = false;
		if (!rate_printed) {
			rate_printed = true;
			printf("Frame interpolation: %.1f presentations per second (%.2f per game frame)\n",
				rate, rate / Limiter::game_fps);
		}

		static double presentations_owed = 0.0;
		presentations_owed += rate / Limiter::game_fps - 1.0;
		unsigned int extras = 0;
		while (presentations_owed >= 1.0 - 1e-9 && extras < 32) {
			presentations_owed -= 1.0;
			++extras;
		}

		LARGE_INTEGER frame_wait = Limiter::FrameWait();
		if (extras > 0 && frame_wait.QuadPart > 0) {
			double spacing = (double)frame_wait.QuadPart / (double)(extras + 1);
			if (spacing >= 1.0) {
				LARGE_INTEGER frame_start = Limiter::FrameStart();
				LARGE_INTEGER frame_work = Limiter::FrameWork();
				bool has_deadline = frame_start.QuadPart > 0 && frame_work.QuadPart > 0;
				LARGE_INTEGER deadline;
				deadline.QuadPart = frame_start.QuadPart + frame_wait.QuadPart;

				for (unsigned int k = 1; k <= extras; ++k) {
					LARGE_INTEGER target;
					target.QuadPart = base.QuadPart + (LONGLONG)(spacing * (double)k + 0.5);
					if (has_deadline && target.QuadPart + frame_work.QuadPart > deadline.QuadPart)
						continue;

					Limiter::WaitUntil(target);

					if (presents < th10_diag_presents_max)
						QueryPerformanceCounter(&present_time[presents]);
					++presents;

					// Where this presentation sits between the game's own one and the next one:
					// the frames of one game frame are spread evenly over it, so the k-th of
					// extras + 1 of them draws the state k/(extras + 1) frames along
					th10_extra_presentation(device, (float)k / (float)(extras + 1));
				}
			}
		}
	}

	{
		Th10DiagFrame& frame = th10_diag_frames[th10_diag_count % th10_diag_frames_max];
		frame.base = base;
		frame.presents = presents;
		frame.updates = th10_frame_updates;
		th10_frame_updates = 0;
		void* stage = *th10_stage;
		frame.stage_timer = stage != nullptr
			? *(int*)((char*)stage + th10_stage_timer) : 0;
		void* fps = *th10_fps;
		frame.game_rate = fps != nullptr ? *(float*)((char*)fps + 0x34) : 0.0f;
		frame.game_frames = fps != nullptr ? *(int*)((char*)fps + 0x20) : 0;
		for (unsigned int p = 0; p < th10_diag_presents_max; ++p)
			frame.present_time[p] = p < presents ? present_time[p] : base;
		++th10_diag_count;
	}

	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th10_diag_key_down) {
			th10_diag_key_down = true;
			th10_diag_dump();
		}
	} else {
		th10_diag_key_down = false;
	}

	return result;
}

// The present the patch takes over is split in the middle of its own argument setup: the game
// pushes three of the four arguments (0x4391FD, 0x4391FF, 0x439201), then loads the device
// (0x43920A, where the patch puts its call), then pushes the last argument and the `this` pointer
// and calls Present (0x439211 to 0x439216), which is __stdcall and takes all five dwords off the
// stack itself (its `ret 0x14`). The patch jumps over everything from 0x43920F on, so those first
// three pushes are still on the stack when the hook is done; the `ret 12` here is what takes them
// off in place of the present the game would have called, so the site's landing point (0x439217)
// and the stack cookie check at the end of the function see the stack they expect. Leaving them on
// there costs the game its own cookie and it reports that as a buffer overrun.
//
// (The three present sites of th11 and the later games don't need this: their whole argument setup
// is inside the skipped part, so it cancels itself out.)
static void __declspec(naked) th10_present_hook_keeping_arguments() {
	__asm {
		call th10_present_hook
		ret 0xC
	}
}

// Jumps over the present the game sets up after its present call site, so the code after it runs
// with the result the hook left in eax.
static void th10_skip_present_setup(DWORD address, size_t size) {
	BYTE patch[32];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + size) - (address + 5);
	for (size_t i = 5; i < size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, size);
}

void th10_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB };
		patch_bytes(0x004393C5, patch, sizeof(patch));
	}
	{
		// Don't sleep in the main loop
		BYTE patch[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		patch_bytes(0x004393B7, patch, sizeof(patch));
	}
	{
		// Hook window update
		patch_call(0x00438D31, th10_window_update_hook);
	}
	{
		// th10 keeps a frame rate of its own and acts on it: once the rate it works out goes above
		// the threshold it has at [0x470BB8] it treats the frame as having run too fast, and after
		// two of those in a row it re-syncs its own clock from the performance counter - after four
		// it clears that counter's base as well, which is what makes the rate it reports read as 0
		// for a while (its own source says so: see fps.cpp, the task FUN_004134B0 runs).
		//
		// Running the game at its own 60 frames a second never reaches that threshold, but raising
		// the frame rate does, right away - the rate is that much higher than 60 by design, and the
		// limiter is what is really pacing the game then. That leaves this protection nothing to act
		// on and only the re-syncing it does on the way, so it goes: the jump here is the same one
		// thprac's own th10 module makes (0x413516, `EB 50`) when it is the one running the game.
		BYTE patch[] = { 0xEB, 0x50 };
		patch_bytes(0x00413516, patch, sizeof(patch));
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x00438BC3, Direct3DCreate9_hook);
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		// Take over the call into the game's own update walk as well, only to count how many times
		// it ran for a frame (see th10_update_hook); the walk itself runs exactly as it always did
		BYTE* update_call = (BYTE*)0x0043942F;
		if (update_call[0] == 0xE8) {
			patch_call(0x0043942F, (void*)th10_update_hook);
		} else {
			printf("th10 update call at 0x43942F doesn't look like the known one, skipping!\n");
		}

		// Take over the object render, only to remember what the scene is drawn with (see
		// th10_render_hook); the render itself runs exactly the way it always did
		BYTE* render_call = (BYTE*)0x004394D8;
		unsigned int render_patched = 0;
		if (render_call[0] == 0xE8) {
			patch_call(0x004394D8, (void*)th10_render_hook);
			++render_patched;
		} else {
			printf("th10 render call at 0x4394D8 doesn't look like the known one, skipping!\n");
		}

		// Take over the present inside the frame function's own present helper (FUN_004391F0).
		// The site starts by loading the device (5 bytes, exactly what a hooked call needs) and then
		// pushes its arguments and calls Present over the 8 bytes below (0x43920F is `mov ecx,[eax]`,
		// 0x439211 `push 0`, 0x439213 `push eax` and 0x439214 `call [ecx+44h]`), which are skipped
		// over so 0x439217 (`test eax,eax`) runs with the result the hook left in eax. Three of the
		// four arguments were pushed before the site, so the hook returns through the trampoline that
		// takes them back off (see th10_present_hook_keeping_arguments).
		const size_t present_setup = 8;
		BYTE* present_site = (BYTE*)0x0043920A;
		unsigned int present_patched = 0;
		if (present_site[0] == 0xA1 && *(DWORD*)(present_site + 1) == 0x00491C30) {
			// The setup has to end exactly on the instruction after the call, or the code after it
			// runs from the middle of one and walks the stack off by whatever it decodes to
			if (present_site[5 + present_setup] == 0x85) {
				patch_call(0x0043920A, (void*)th10_present_hook_keeping_arguments);
				th10_skip_present_setup(0x0043920A + 5, present_setup);
				++present_patched;
			} else {
				printf("th10 present call at 0x43920A isn't followed by the code expected after it (0x%02x), skipping!\n",
					present_site[5 + present_setup]);
			}
		} else {
			printf("th10 present call at 0x43920A doesn't look like the known one, skipping!\n");
		}
		printf("Frame interpolation: %u render call(s) and %u present call(s) taken over (setting %d)\n",
			render_patched, present_patched,
			Config::InterpolationFPS ? (int)Config::InterpolationFPS : Config::Interpolation);

		// The frame rate the frames are actually presented at goes right after the line the patch
		// draws in the bottom left corner (see overlay.h), the same as in th11 to th15
		overlay_show_present_rate(Config::ShowOverlay && (Config::Interpolation != 0 || Config::InterpolationFPS != 0));
		overlay_set_present_rate_follow_text(true);
	}
}

FPSTarget th10_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x00474E30;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & (InputState::Skip | InputState::Shoot))
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
