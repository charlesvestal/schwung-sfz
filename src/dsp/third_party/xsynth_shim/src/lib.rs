// Minimal C ABI shim around xsynth-core for the Move SFZ plugin.
//
// We only need block-rendering through ChannelGroup. The upstream xsynth-clib
// crate pulls cpal + alsa via xsynth-realtime, which we don't use, so we wrap
// xsynth-core directly here and avoid the audio-backend dep chain.

use std::ffi::CStr;
use std::os::raw::{c_char, c_int};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::thread;

/// MOVE: process-global load lock. Move runs multiple SFZ tracks in the
/// same shim process, each with its own XSynthHandle. If two of them
/// concurrently load heavy DS libraries (WörliTzer 950 MB + Clean Fender
/// 600 MB) they collectively exceed the device's free RAM and Rust's
/// alloc handler aborts the whole process. Workers acquire this lock for
/// the duration of SampleSoundfont::new so allocations are sequential
/// across instances.
fn process_load_lock() -> &'static Mutex<()> {
    static L: OnceLock<Mutex<()>> = OnceLock::new();
    L.get_or_init(|| Mutex::new(()))
}

/// MOVE: read `MemAvailable` from /proc/meminfo (KB units). Returns 0 on
/// any error so callers can choose to skip the check rather than fail.
fn available_memory_mb() -> u64 {
    if let Ok(content) = std::fs::read_to_string("/proc/meminfo") {
        for line in content.lines() {
            if let Some(rest) = line.strip_prefix("MemAvailable:") {
                if let Some(kb) = rest.split_whitespace().next().and_then(|s| s.parse::<u64>().ok()) {
                    return kb / 1024;
                }
            }
        }
    }
    0
}

/// MOVE: refuse a load if free RAM is below this threshold. The earlier
/// 350 MB threshold was too aggressive: once a single heavy patch is
/// resident, MemAvailable drops below 350 MB and EVERY subsequent load
/// (including switching from heavy to lighter) gets refused — even though
/// the drop_sink + worker drain would free enough memory in practice.
/// 80 MB is just enough margin for the alloc path itself to make progress
/// past the worst-case fragmentation; below this we'd genuinely OOM-abort.
const MIN_FREE_MB_FOR_LOAD: u64 = 80;

/// MOVE: process-global last-error. Was thread_local — that prevented the
/// audio thread (caller of xshim_last_error) from ever seeing errors that
/// the load WORKER thread had set. Use a shared Mutex<String> instead so
/// errors propagate across the worker→audio boundary.
fn last_error_slot() -> &'static Mutex<String> {
    static L: OnceLock<Mutex<String>> = OnceLock::new();
    L.get_or_init(|| Mutex::new(String::new()))
}

fn set_last_error<S: Into<String>>(msg: S) {
    if let Ok(mut s) = last_error_slot().lock() {
        *s = msg.into();
    }
}

use xsynth_core::{
    AudioPipe, AudioStreamParams, ChannelCount,
    channel::{ChannelAudioEvent, ChannelEvent, ChannelConfigEvent, ChannelInitOptions, ControlEvent, SoundfontDropSink},
    channel_group::{
        ChannelGroup, ChannelGroupConfig, ParallelismOptions, SynthEvent, SynthFormat,
    },
    soundfont::{Interpolator, SampleSoundfont, SoundfontBase, SoundfontInitOptions},
};

const STATUS_IDLE:      u32 = 0;
const STATUS_LOADING:   u32 = 1;
const STATUS_READY:     u32 = 2;
const STATUS_ERROR:     u32 = 3;
const STATUS_CANCELLED: u32 = 4;

struct LoadWorker {
    handle: Option<thread::JoinHandle<()>>,
    status: Arc<AtomicU32>,
    cancel: Arc<AtomicBool>,
    result: Arc<Mutex<Option<SampleSoundfont>>>,
}

pub struct XSynthHandle {
    group: ChannelGroup,
    worker: Option<LoadWorker>,
    drop_sink: SoundfontDropSink,
    /// MOVE: NoteOns since the last `xshim_take_noteon_count` call. Used by
    /// the plugin's render-perf log to separate spawn-heavy blocks (chord
    /// burst from a sequence) from sustain-heavy blocks (lots of voices
    /// already ringing). Incremented in xshim_note_on, snapshot+reset by
    /// xshim_take_noteon_count just before each render block.
    noteon_count: AtomicU32,
}

/* Install once: panic hook that writes the panic message + a short backtrace
 * to /tmp/xshim_panic.log before the process aborts. catch_unwind can miss
 * panics in foreign threads or alloc-failure aborts; this hook fires earlier
 * in the panic path so we get diagnostics either way. */
use std::sync::Once;
static PANIC_HOOK_INIT: Once = Once::new();

fn install_panic_hook() {
    PANIC_HOOK_INIT.call_once(|| {
        std::panic::set_hook(Box::new(|info| {
            let payload = if let Some(s) = info.payload().downcast_ref::<&str>() { s.to_string() }
                else if let Some(s) = info.payload().downcast_ref::<String>() { s.clone() }
                else { "<no message>".to_string() };
            let loc = info.location()
                .map(|l| format!("{}:{}:{}", l.file(), l.line(), l.column()))
                .unwrap_or_else(|| "<no location>".to_string());
            let entry = format!("[xshim panic] {loc}: {payload}\n");
            set_last_error(format!("panic at {loc}: {payload}"));
            // Best-effort: write to a known file so we can inspect even when
            // catch_unwind doesn't catch (e.g., aborts on thread panics).
            let _ = std::fs::OpenOptions::new()
                .create(true).append(true)
                .open("/data/UserData/schwung/tmp/xshim_panic.log")
                .and_then(|mut f| std::io::Write::write_all(&mut f, entry.as_bytes()));
        }));
    });
}

#[no_mangle]
pub unsafe extern "C" fn xshim_create(sample_rate: u32, channels: u32) -> *mut XSynthHandle {
    install_panic_hook();
    catch_unwind(AssertUnwindSafe(|| {
        let cc = match channels {
            1 => ChannelCount::Mono,
            _ => ChannelCount::Stereo,
        };
        let cfg = ChannelGroupConfig {
            channel_init_options: ChannelInitOptions { fade_out_killing: true },
            format: SynthFormat::Midi,
            audio_params: AudioStreamParams::new(sample_rate, cc),
            parallelism: ParallelismOptions::AUTO_PER_CHANNEL,
        };
        let mut group = ChannelGroup::new(cfg);
        let drop_sink: SoundfontDropSink = Arc::new(Mutex::new(Vec::new()));
        group.set_soundfont_drop_sink(drop_sink.clone());
        Box::into_raw(Box::new(XSynthHandle {
            group,
            worker: None,
            drop_sink,
            noteon_count: AtomicU32::new(0),
        }))
    }))
    .unwrap_or(ptr::null_mut())
}

unsafe fn cancel_inflight(h: &mut XSynthHandle) {
    if let Some(mut w) = h.worker.take() {
        w.cancel.store(true, Ordering::Release);
        if let Some(jh) = w.handle.take() { let _ = jh.join(); }
    }
}

#[no_mangle]
pub unsafe extern "C" fn xshim_destroy(handle: *mut XSynthHandle) {
    if handle.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let mut boxed = Box::from_raw(handle);
        cancel_inflight(&mut boxed);
        drop(boxed);
    }));
}

#[no_mangle]
pub unsafe extern "C" fn xshim_load_sfz(handle: *mut XSynthHandle, path: *const c_char) -> c_int {
    if handle.is_null() || path.is_null() { return -1; }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let h = &mut *handle;
        let cstr = match CStr::from_ptr(path).to_str() {
            Ok(s) => s,
            Err(_) => { eprintln!("[xshim] path is not valid UTF-8"); return -1; }
        };
        let pb = PathBuf::from(cstr);
        let stream_params = AudioStreamParams::new(44100, ChannelCount::Stereo);
        let opts = SoundfontInitOptions {
            bank: None,
            preset: None,
            vol_envelope_options: Default::default(),
            use_effects: true,
            interpolator: Interpolator::Linear,
        };

        /* MOVE: Kill voices + drop the existing soundfont BEFORE constructing
         * the new one. Otherwise old samples (e.g. WörliTzer's ~800 MB) and
         * new samples (e.g. WobbliTzer's ~800 MB) both live in RAM at peak,
         * exceeding the device's free memory and triggering an abort. */
        h.group.send_event(SynthEvent::AllChannels(
            ChannelEvent::Audio(ChannelAudioEvent::AllNotesKilled),
        ));
        // MOVE FORK: target channel 0 (see comment in xshim_load_apply
        // below — broadcast to all 16 channels caused 94 ms audio
        // stalls on preset switch).
        h.group.send_event(SynthEvent::Channel(
            0,
            ChannelEvent::Config(ChannelConfigEvent::SetSoundfonts(Vec::new())),
        ));

        let sf = match SampleSoundfont::new(pb, stream_params, opts) {
            Ok(sf) => sf,
            Err(e) => {
                set_last_error(format!("SampleSoundfont::new: {e:?}"));
                return -1;
            }
        };
        let arc: Arc<dyn SoundfontBase> = Arc::new(sf);
        // MOVE FORK: target channel 0 only. The plugin's MIDI routing
        // sends everything to channel 0 (see xshim_note_on / xshim_cc
        // hardcoding `ch=0`), so the other 15 channels never carry
        // voices. Broadcasting SetSoundfonts via AllChannels still
        // forces a rebuild_matrix on each idle channel — 16 × 128 × 128
        // spawner-list builds = enough audio-thread work (~94 ms peaks
        // in production) to drop Move's audio chain entirely.
        h.group.send_event(SynthEvent::Channel(
            0,
            ChannelEvent::Config(ChannelConfigEvent::SetSoundfonts(vec![arc])),
        ));
        0
    }));
    match result {
        Ok(rc) => rc,
        Err(e) => {
            let msg = if let Some(s) = e.downcast_ref::<&str>() { s.to_string() }
                else if let Some(s) = e.downcast_ref::<String>() { s.clone() }
                else { "unknown panic payload".to_string() };
            set_last_error(format!("panic during load: {msg}"));
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn xshim_note_on(handle: *mut XSynthHandle, ch: u8, key: u8, vel: u8) {
    if handle.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let h = &mut *handle;
        h.noteon_count.fetch_add(1, Ordering::Relaxed);
        h.group.send_event(SynthEvent::Channel(
            ch as u32,
            ChannelEvent::Audio(ChannelAudioEvent::NoteOn { key, vel }),
        ));
    }));
}

/// MOVE: snapshot+reset NoteOn count since the prior call. Plugin calls
/// this right before xshim_render so the rendered block can be classified
/// as spawn-heavy vs sustain-heavy in the perf log.
#[no_mangle]
pub unsafe extern "C" fn xshim_take_noteon_count(handle: *mut XSynthHandle) -> u32 {
    if handle.is_null() { return 0; }
    let h = &mut *handle;
    h.noteon_count.swap(0, Ordering::Relaxed)
}

/// MOVE: read the most recent block's per-section render breakdown.
/// `out` must point to 4 u32 slots: [parallel_us, sum_us, fx_us, total_us].
/// `parallel_us` covers event drain + per-key render inside the rayon
/// pool. `sum_us` is the serial mix of per-key audio_cache into out.
/// `fx_us` is apply_channel_effects (volume/pan/cutoff sweep). `total_us`
/// is wall time inside push_key_events_and_render. Globally-static —
/// reflects the last channel-render that completed (one active SFZ
/// channel per instance, so unambiguous in practice).
#[no_mangle]
pub unsafe extern "C" fn xshim_take_render_breakdown(_handle: *mut XSynthHandle, out: *mut u32) {
    if out.is_null() { return; }
    let slot = std::slice::from_raw_parts_mut(out, 4);
    slot[0] = xsynth_core::channel::LAST_PARALLEL_US.load(Ordering::Relaxed);
    slot[1] = xsynth_core::channel::LAST_SUM_US.load(Ordering::Relaxed);
    slot[2] = xsynth_core::channel::LAST_FX_US.load(Ordering::Relaxed);
    slot[3] = xsynth_core::channel::LAST_TOTAL_US.load(Ordering::Relaxed);
}

#[no_mangle]
pub unsafe extern "C" fn xshim_note_off(handle: *mut XSynthHandle, ch: u8, key: u8) {
    if handle.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let h = &mut *handle;
        h.group.send_event(SynthEvent::Channel(
            ch as u32,
            ChannelEvent::Audio(ChannelAudioEvent::NoteOff { key }),
        ));
    }));
}

#[no_mangle]
pub unsafe extern "C" fn xshim_cc(handle: *mut XSynthHandle, ch: u8, cc: u8, val: u8) {
    if handle.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let h = &mut *handle;
        h.group.send_event(SynthEvent::Channel(
            ch as u32,
            ChannelEvent::Audio(ChannelAudioEvent::Control(ControlEvent::Raw(cc, val))),
        ));
    }));
}

#[no_mangle]
pub unsafe extern "C" fn xshim_pitch_bend(handle: *mut XSynthHandle, ch: u8, value: f32) {
    if handle.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let h = &mut *handle;
        h.group.send_event(SynthEvent::Channel(
            ch as u32,
            ChannelEvent::Audio(ChannelAudioEvent::Control(ControlEvent::PitchBend(value))),
        ));
    }));
}

#[no_mangle]
pub unsafe extern "C" fn xshim_all_notes_off(handle: *mut XSynthHandle) {
    if handle.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let h = &mut *handle;
        h.group.send_event(SynthEvent::AllChannels(
            ChannelEvent::Audio(ChannelAudioEvent::AllNotesOff),
        ));
    }));
}

#[no_mangle]
pub unsafe extern "C" fn xshim_render(handle: *mut XSynthHandle, out: *mut f32, num_samples: usize) {
    if handle.is_null() || out.is_null() || num_samples == 0 { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let h = &mut *handle;
        let slc = std::slice::from_raw_parts_mut(out, num_samples);
        h.group.read_samples(slc);
    }));
}

#[no_mangle]
pub unsafe extern "C" fn xshim_last_error(out_buf: *mut c_char, out_len: usize) -> usize {
    if out_buf.is_null() || out_len == 0 { return 0; }
    let msg = match last_error_slot().lock() {
        Ok(s) => s.clone(),
        Err(_) => String::new(),
    };
    let bytes = msg.as_bytes();
    let n = bytes.len().min(out_len - 1);
    std::ptr::copy_nonoverlapping(bytes.as_ptr(), out_buf as *mut u8, n);
    *out_buf.add(n) = 0;
    n
}

/// Send SetSoundfonts(empty) so the OLD soundfont gets pushed to drop_sink.
/// The worker thread will then drain & drop it before allocating the new
/// soundfont, keeping peak memory bounded.
unsafe fn dispatch_drop_old(h: &mut XSynthHandle) {
    h.group.send_event(SynthEvent::AllChannels(
        ChannelEvent::Audio(ChannelAudioEvent::AllNotesKilled),
    ));
    // MOVE FORK: target channel 0 only (see xshim_load_sfz comment).
    h.group.send_event(SynthEvent::Channel(
        0,
        ChannelEvent::Config(ChannelConfigEvent::SetSoundfonts(Vec::new())),
    ));
}

#[no_mangle]
pub unsafe extern "C" fn xshim_load_sfz_async(handle: *mut XSynthHandle, path: *const c_char) -> c_int {
    if handle.is_null() || path.is_null() { return -1; }
    let h = &mut *handle;

    let cstr = match CStr::from_ptr(path).to_str() {
        Ok(s) => s.to_string(),
        Err(_) => { set_last_error("path is not valid UTF-8"); return -1; }
    };

    cancel_inflight(h);
    dispatch_drop_old(h);

    let status = Arc::new(AtomicU32::new(STATUS_LOADING));
    let cancel = Arc::new(AtomicBool::new(false));
    let result: Arc<Mutex<Option<SampleSoundfont>>> = Arc::new(Mutex::new(None));

    let status_t = status.clone();
    let cancel_t = cancel.clone();
    let result_t = result.clone();
    let drop_sink_t = h.drop_sink.clone();

    let join_handle = thread::Builder::new()
        .name("xshim-load".into())
        .spawn(move || {
            // Drop any pending old soundfont(s) FIRST so we don't have OLD +
            // NEW resident at the same time.
            let t_drain = std::time::Instant::now();
            {
                let owned = {
                    if let Ok(mut q) = drop_sink_t.lock() {
                        std::mem::take(&mut *q)
                    } else { Vec::new() }
                };
                let drain_count = owned.len();
                let t_locked = t_drain.elapsed().as_micros();
                drop(owned);
                let t_dropped = t_drain.elapsed().as_micros();
                use std::io::Write;
                if let Ok(mut f) = std::fs::OpenOptions::new()
                    .create(true)
                    .append(true)
                    .open("/data/UserData/schwung/tmp/xsynth_debug.log")
                {
                    let _ = writeln!(
                        f,
                        "[xshim] worker drain: count={} lock={}us drop={}us",
                        drain_count, t_locked, t_dropped - t_locked,
                    );
                }
            }
            let stream_params = AudioStreamParams::new(44100, ChannelCount::Stereo);
            let opts = SoundfontInitOptions {
                bank: None, preset: None,
                vol_envelope_options: Default::default(),
                use_effects: true,
                interpolator: Interpolator::Linear,
            };
            let pb = PathBuf::from(&cstr);
            // Acquire the process-global load lock so peak memory stays
            // bounded to ONE soundfont's allocation at a time.
            let _guard = process_load_lock().lock();
            // Cancel check after acquiring lock — caller may have signaled
            // cancel while we were queued.
            if cancel_t.load(Ordering::Relaxed) {
                set_last_error("Load cancelled before start");
                status_t.store(STATUS_CANCELLED, Ordering::Release);
                return;
            }
            // OOM guard: refuse if RAM is too tight rather than abort the
            // whole shim. Without this, two heavy DS patches concurrently
            // resident push past Move's free RAM and Rust's alloc handler
            // takes down the process.
            let avail = available_memory_mb();
            if avail > 0 && avail < MIN_FREE_MB_FOR_LOAD {
                set_last_error(format!(
                    "Insufficient memory to load ({} MB free, need ≥{} MB). \
                     Switch other tracks to lighter patches.",
                    avail, MIN_FREE_MB_FOR_LOAD
                ));
                status_t.store(STATUS_ERROR, Ordering::Release);
                return;
            }
            let load_res = catch_unwind(AssertUnwindSafe(|| {
                SampleSoundfont::new_sfz_cancellable(pb, stream_params, opts, Some(cancel_t))
            }));
            match load_res {
                Ok(Ok(sf)) => {
                    *result_t.lock().unwrap() = Some(sf);
                    status_t.store(STATUS_READY, Ordering::Release);
                }
                Ok(Err(e)) => {
                    let is_cancel = matches!(e, xsynth_core::soundfont::LoadSfzError::Cancelled);
                    set_last_error(format!("SampleSoundfont::new_sfz: {e:?}"));
                    status_t.store(
                        if is_cancel { STATUS_CANCELLED } else { STATUS_ERROR },
                        Ordering::Release,
                    );
                }
                Err(_) => {
                    set_last_error("panic during async load");
                    status_t.store(STATUS_ERROR, Ordering::Release);
                }
            }
        });

    match join_handle {
        Ok(jh) => {
            h.worker = Some(LoadWorker {
                handle: Some(jh),
                status, cancel, result,
            });
            0
        }
        Err(e) => {
            set_last_error(format!("thread spawn failed: {e}"));
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn xshim_load_status(handle: *const XSynthHandle) -> c_int {
    if handle.is_null() { return STATUS_IDLE as c_int; }
    let h = &*handle;
    match &h.worker {
        Some(w) => w.status.load(Ordering::Acquire) as c_int,
        None => STATUS_IDLE as c_int,
    }
}

#[no_mangle]
pub unsafe extern "C" fn xshim_load_cancel(handle: *mut XSynthHandle) {
    if handle.is_null() { return; }
    let h = &mut *handle;
    if let Some(w) = &h.worker {
        w.cancel.store(true, Ordering::Release);
    }
}

#[no_mangle]
pub unsafe extern "C" fn xshim_load_apply(handle: *mut XSynthHandle) -> c_int {
    if handle.is_null() { return -1; }
    let h = &mut *handle;
    let Some(w) = h.worker.as_mut() else { return -1; };
    if w.status.load(Ordering::Acquire) != STATUS_READY { return -1; }
    let sf_opt = w.result.lock().unwrap().take();
    let Some(sf) = sf_opt else { return -1; };
    let arc: Arc<dyn SoundfontBase> = Arc::new(sf);
    // MOVE FORK: channel 0 only — broadcast forces rebuild_matrix on
    // all 16 idle channels which spikes audio thread to ~94 ms.
    h.group.send_event(SynthEvent::Channel(
        0,
        ChannelEvent::Config(ChannelConfigEvent::SetSoundfonts(vec![arc])),
    ));
    if let Some(jh) = w.handle.take() { let _ = jh.join(); }
    h.worker = None;
    0
}

#[no_mangle]
pub unsafe extern "C" fn xshim_load_clear_status(handle: *mut XSynthHandle) {
    if handle.is_null() { return; }
    let h = &mut *handle;
    if let Some(w) = h.worker.as_mut() {
        if let Some(jh) = w.handle.take() { let _ = jh.join(); }
    }
    h.worker = None;
}

#[no_mangle]
pub unsafe extern "C" fn xshim_voice_count(handle: *const XSynthHandle) -> u64 {
    if handle.is_null() { return 0; }
    let h = &*handle;
    h.group.voice_count()
}

/// MOVE: per-key polyphony cap. xsynth doesn't have a global voice cap —
/// `layers` is the per-key voice count limit. A DS preset with multiple
/// velocity/mic/round-robin layers can spawn many voices per note-on; this
/// caps how many stack on a single keypress. Pass 0 for "unlimited".
#[no_mangle]
pub unsafe extern "C" fn xshim_set_layer_count(handle: *mut XSynthHandle, layers: u32) {
    if handle.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let h = &mut *handle;
        let opt = if layers == 0 { None } else { Some(layers as usize) };
        h.group.send_event(SynthEvent::AllChannels(ChannelEvent::Config(
            ChannelConfigEvent::SetLayerCount(opt),
        )));
    }));
}

/// MOVE: polyphony cap. One unit = one note-on event, regardless of how
/// many voices the preset spawns per note (WörliTzer = 5 voices/note;
/// Bass = 1 voice/note). When the cap is exceeded, xsynth drops the
/// oldest releasing note-group whole — release tails were fading anyway,
/// so audible impact is "longer tails clip a beat early" rather than
/// dropped audio frames. Pass 0 for "unlimited".
#[no_mangle]
pub unsafe extern "C" fn xshim_set_polyphony_cap(handle: *mut XSynthHandle, cap: u32) {
    if handle.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let h = &mut *handle;
        let opt = if cap == 0 { None } else { Some(cap as usize) };
        h.group.send_event(SynthEvent::AllChannels(ChannelEvent::Config(
            ChannelConfigEvent::SetPolyphonyCap(opt),
        )));
    }));
}

/// MOVE / Phase 9 prototype: install / remove the channel reverb.
/// Pass enable=0 to remove. enable=1 installs a fundsp `reverb_stereo`
/// with the given (room, time, damp) — typical room = 10..20,
/// time = 1..5, damp = 0..1.
#[no_mangle]
pub unsafe extern "C" fn xshim_set_reverb(
    handle: *mut XSynthHandle,
    enable: u32,
    room: f32,
    time: f32,
    damp: f32,
) {
    if handle.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let h = &mut *handle;
        let params = if enable != 0 { Some((room, time, damp)) } else { None };
        h.group.send_event(SynthEvent::AllChannels(ChannelEvent::Config(
            ChannelConfigEvent::SetReverb(params),
        )));
    }));
}

/// MOVE / Phase 9 prototype: dry/wet mix for the channel reverb. 0
/// disables processing entirely (no CPU cost); 1 is fully wet.
#[no_mangle]
pub unsafe extern "C" fn xshim_set_reverb_wet(handle: *mut XSynthHandle, wet: f32) {
    if handle.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let h = &mut *handle;
        h.group.send_event(SynthEvent::AllChannels(ChannelEvent::Config(
            ChannelConfigEvent::SetReverbWet(wet),
        )));
    }));
}

/// MOVE: spawn burst limit. Max NoteOn events drained per render block;
/// surplus events defer to subsequent blocks. Quantized chord bursts
/// (e.g. sequence step lands 10 notes at once) overwhelm a single
/// render block with spawn cost. Spreading them across 2-3 blocks adds
/// 3-9 ms of intra-chord latency, below human perception. Pass 0 for
/// "unlimited" (legacy behavior).
#[no_mangle]
pub unsafe extern "C" fn xshim_set_spawn_burst_limit(handle: *mut XSynthHandle, limit: u32) {
    if handle.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let h = &mut *handle;
        let opt = if limit == 0 { None } else { Some(limit as usize) };
        h.group.send_event(SynthEvent::AllChannels(ChannelEvent::Config(
            ChannelConfigEvent::SetSpawnBurstLimit(opt),
        )));
    }));
}
