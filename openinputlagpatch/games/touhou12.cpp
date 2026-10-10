// Touhou 12: Undefined Fantastic Object v1.00b

#include <stdio.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "config.h"
#include "overlay.h"
#include "tool_state.h"
#include "touhou12.h"

using namespace Touhou12;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004C22C8;

int __stdcall th12_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// th12 draws a game frame the way th13, th14 and th15 do. The block that does it sits in three
// frame functions, one per kind of scene the game can be in, and all three are the same:
//
//     0x450152  mov eax, [0x4CE8F0]       the device
//     0x450159  [ecx+0xA4]                BeginScene
//     0x450162  FUN_0045A380              reset the sprite queue
//     0x450167  mov edi, 0x4CE8E8         the render context, for the call below
//     0x45016C  [0x4CF278] = 0xFF
//     0x450176  FUN_00430300              prepare the render context (its object in EDI)
//     0x45017B  FUN_00462620              render every object (they fill the sprite queue)
//     0x450180  mov esi, [0x4CE8CC]       the sprite queue's owner, for the call below
//     0x450186  FUN_0045A3C0              draw the sprite queue (its object in ESI)
//     0x450192  [ecx+0x104]               SetTexture(0, NULL)
//     0x4501A6  [ecx+0xA8]                EndScene
//
// The other two blocks are at 0x450500 and 0x45069F, with the same instructions at different
// addresses. Each frame function then calls a helper of its own right after its EndScene, and that
// is where it presents: 0x450355 in FUN_004501E0, 0x450586 in FUN_00450580 and 0x450789 in
// FUN_00450720. Every one of them is a 5 byte load of the device followed by the arguments being
// pushed and the present called over the 16 bytes after it.
//
// Two of the helpers take their object in a register instead of on the stack, which is how the
// compiler that built this game left them, so they can't be reached through a function pointer -
// the trampolines below are what put the object where each one expects it. Both registers are
// callee-saved and are put back before returning: leaving them holding the object hands the caller
// something other than what it had, which is what broke the th13 version of this code (it crashed
// in the SetTexture right after the sprite queue was drawn).
//
// The extra presentations draw the state the game is already in, which is the "moved forward by
// zero frames" case; stage 1 is what puts anything of the game's own in motion inside them.

// The plain calls the render block makes into the game
static auto th12_reset_sprite_queue = (void(*)())0x0045A380;
static auto th12_render_objects = (int(*)())0x00462620;
static void* const th12_prepare_render_address = (void*)0x00430300;
static void* const th12_draw_sprite_queue_address = (void*)0x0045A3C0;

static void __declspec(naked) th12_prepare_render() {
	__asm {
		push edi
		mov edi, 0x004CE8E8
		call dword ptr [th12_prepare_render_address]
		pop edi
		ret
	}
}

static void __declspec(naked) th12_draw_sprite_queue() {
	__asm {
		push esi
		mov esi, dword ptr ds:[0x004CE8CC]
		call dword ptr [th12_draw_sprite_queue_address]
		pop esi
		ret
	}
}

// Where the user's tool builds its frame and where it draws it. thprac's th12 module starts its
// frame from a hook on the return of the scene update at 0x4625FB and draws the overlay it built
// from a hook on the return of the object render at 0x462722 (see tool_state.h); both are the ret
// of the walk the render block above calls into.
static unsigned char* const th12_tool_frame_address = (unsigned char*)0x004625FB;
static unsigned char* const th12_tool_draw_address = (unsigned char*)0x00462722;

// The viewport the scene is drawn with, taken from the frame the game's own render ran in
static D3DVIEWPORT9 th12_scene_viewport = {};

// The device the game draws with. The present sites load it from the game's own global, so that is
// the device that has to present the frame - not the one the patch's D3D9 hook captured, which is
// the same object while that hook is in place and nothing at all if the game got its device in a
// way the hook doesn't see. Reporting a failed present to the game ends it (its device-lost
// handling takes over), so the device the game itself would have presented with comes first.
static IDirect3DDevice9* th12_device() {
	IDirect3DDevice9* device = *(IDirect3DDevice9**)0x004CE8F0;
	if (device != nullptr)
		return device;
	return d3d9_hooked_device();
}

// Remembers the viewport the scene is about to be drawn with, then lets the game's own render run
// exactly as it always did (see th12_extra_presentation for what it's for)
static void th12_render_hook() {
	IDirect3DDevice9* device = th12_device();
	if (device != nullptr)
		device->GetViewport(&th12_scene_viewport);

	ToolState::WatchFrameReady(th12_tool_frame_address, th12_tool_draw_address);

	th12_render_objects();
}

// The number of presentations per second the config asks for. 0 means interpolation is off.
static double th12_present_rate() {
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
static const unsigned int th12_diag_frames_max = 300;
static const unsigned int th12_diag_presents_max = 8;
struct Th12DiagFrame {
	LARGE_INTEGER base;
	unsigned int presents;
	LARGE_INTEGER present_time[th12_diag_presents_max];
};
static Th12DiagFrame th12_diag_frames[th12_diag_frames_max];
static unsigned int th12_diag_count;
static bool th12_diag_key_down;

static LARGE_INTEGER th12_perf_freq;

static void th12_diag_dump() {
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

	unsigned int count = th12_diag_count < th12_diag_frames_max ? th12_diag_count : th12_diag_frames_max;
	unsigned int start = th12_diag_count < th12_diag_frames_max ? 0 : th12_diag_count % th12_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, th12_present_rate());
	fprintf(file, "frames recorded: %u\n", count);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th12DiagFrame& frame = th12_diag_frames[(start + i) % th12_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th12_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 / (double)th12_perf_freq.QuadPart);
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count, presents * (double)th12_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// One extra presentation of the frame: the same render block the game runs, once more, so the state
// the game is in is drawn and presented again. The tool's frame is readied again, so the object
// render below draws its overlay into this presentation the same way it does in the game's own one.
static void th12_extra_presentation(IDirect3DDevice9* device, float t) {
	device->BeginScene();
	if (th12_scene_viewport.Width != 0)
		device->SetViewport(&th12_scene_viewport);
	th12_reset_sprite_queue();
	*(DWORD*)0x004CF278 = 0xFF;
	th12_prepare_render();
	ToolState::ReadyToDraw();
	th12_render_objects();
	th12_draw_sprite_queue();
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
static HRESULT th12_present_hook() {
	IDirect3DDevice9* device = th12_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th12_perf_freq);
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	HRESULT result = device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();
	Limiter::MarkFrameWorkEnd();
	ToolState::WatchFrameDrawn();

	unsigned int presents = 1;
	LARGE_INTEGER present_time[th12_diag_presents_max];
	present_time[0] = base;

	double rate = th12_present_rate();
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

					if (presents < th12_diag_presents_max)
						QueryPerformanceCounter(&present_time[presents]);
					++presents;

					// Where this presentation sits between the game's own one and the next one:
					// the frames of one game frame are spread evenly over it, so the k-th of
					// extras + 1 of them draws the state k/(extras + 1) frames along
					th12_extra_presentation(device, (float)k / (float)(extras + 1));
				}
			}
		}
	}

	{
		Th12DiagFrame& frame = th12_diag_frames[th12_diag_count % th12_diag_frames_max];
		frame.base = base;
		frame.presents = presents;
		for (unsigned int p = 0; p < th12_diag_presents_max; ++p)
			frame.present_time[p] = p < presents ? present_time[p] : base;
		++th12_diag_count;
	}

	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th12_diag_key_down) {
			th12_diag_key_down = true;
			th12_diag_dump();
		}
	} else {
		th12_diag_key_down = false;
	}

	return result;
}

// Jumps over the present the game sets up after its present call site, so the code after it runs
// with the result the hook left in eax.
static void th12_skip_present_setup(DWORD address, size_t size) {
	BYTE patch[32];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + size) - (address + 5);
	for (size_t i = 5; i < size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, size);
}

void th12_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB, 0x43 };
		patch_bytes(0x00450424, patch, sizeof(patch));
	}
	{
		// Force fast input latency mode
		BYTE patch[] = { 0xEB };
		patch_bytes(0x0044F87A, patch, sizeof(patch)); // Skip over automatic
		patch_bytes(0x0044F88E, patch, sizeof(patch)); // Skip over normal
	}
	{
		// Hook window update
		patch_call(0x0044F8AA, th12_window_update_hook);
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x0044F6FC, Direct3DCreate9_hook);
	}
	if (Config::ReplaySpeedControl) {
		// Skip the original replay speed control stuff
		BYTE patch[] = { 0xEB };
		patch_bytes(0x0043C54F, patch, sizeof(patch));
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		// Take over the object render, only to remember the viewport the scene is drawn with (see
		// th12_render_hook); the render itself runs exactly the way it always did
		static const DWORD render_calls[] = { 0x0045017B, 0x00450529, 0x004506C8 };
		unsigned int render_patched = 0;
		for (DWORD address : render_calls) {
			BYTE* render_call = (BYTE*)address;
			if (render_call[0] == 0xE8) {
				patch_call(address, (void*)th12_render_hook);
				++render_patched;
			} else {
				printf("th12 render call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}

		// Take over the presents in the three frame functions' helpers. Each site starts by loading
		// the device (5 bytes, exactly what a hooked call needs) and then pushes its arguments and
		// calls Present over the 16 bytes below, which are skipped over so the code after them runs
		// with the result the hook left in eax.
		static const DWORD present_sites[] = { 0x00450355, 0x00450586, 0x00450789 };
		unsigned int present_patched = 0;
		for (DWORD address : present_sites) {
			BYTE* present_site = (BYTE*)address;
			if (present_site[0] == 0xA1 && *(DWORD*)(present_site + 1) == 0x004CE8F0) {
				patch_call(address, (void*)th12_present_hook);
				th12_skip_present_setup(address + 5, 16);
				++present_patched;
			} else {
				printf("th12 present call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}
		printf("Frame interpolation: %u render call(s) and %u present call(s) taken over (setting %d)\n",
			render_patched, present_patched,
			Config::InterpolationFPS ? (int)Config::InterpolationFPS : Config::Interpolation);

		// The frame rate the frames are actually presented at goes right after the line the patch
		// draws in the bottom left corner (see overlay.h), the same as in th13, th14 and th15
		overlay_show_present_rate(Config::ShowOverlay && (Config::Interpolation != 0 || Config::InterpolationFPS != 0));
		overlay_set_present_rate_follow_text(true);
	}
}

FPSTarget th12_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x004D48B8;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & (InputState::Skip | InputState::Shoot))
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
