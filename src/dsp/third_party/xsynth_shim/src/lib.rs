// Minimal C ABI shim around xsynth-core for the Move SFZ plugin.
//
// We only need block-rendering through ChannelGroup. The upstream xsynth-clib
// crate pulls cpal + alsa via xsynth-realtime, which we don't use, so we wrap
// xsynth-core directly here and avoid the audio-backend dep chain.

use std::cell::RefCell;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;

thread_local! {
    static LAST_ERROR: RefCell<String> = const { RefCell::new(String::new()) };
}

fn set_last_error<S: Into<String>>(msg: S) {
    LAST_ERROR.with(|cell| *cell.borrow_mut() = msg.into());
}

use xsynth_core::{
    AudioPipe, AudioStreamParams, ChannelCount,
    channel::{ChannelAudioEvent, ChannelEvent, ChannelConfigEvent, ChannelInitOptions, ControlEvent},
    channel_group::{
        ChannelGroup, ChannelGroupConfig, ParallelismOptions, SynthEvent, SynthFormat,
    },
    soundfont::{Interpolator, SampleSoundfont, SoundfontBase, SoundfontInitOptions},
};

/// Load worker status. Mirrors `XSHIM_LOAD_*` constants exposed to C.
/// 0 idle / 1 loading / 2 ready / 3 error / 4 cancelled
const STATUS_IDLE: u32 = 0;
const STATUS_LOADING: u32 = 1;
const STATUS_READY: u32 = 2;
const STATUS_ERROR: u32 = 3;
const STATUS_CANCELLED: u32 = 4;

struct LoadWorker {
    handle: Option<thread::JoinHandle<()>>,
    status: Arc<AtomicU32>,
    cancel: Arc<AtomicBool>,
    /// Worker writes the loaded soundfont here on success; main thread takes
    /// it out via `xshim_load_apply`. Mutex<Option> so we can `take()`.
    result: Arc<Mutex<Option<SampleSoundfont>>>,
}

pub struct XSynthHandle {
    group: ChannelGroup,
    worker: Option<LoadWorker>,
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
        let group = ChannelGroup::new(cfg);
        Box::into_raw(Box::new(XSynthHandle { group, worker: None }))
    }))
    .unwrap_or(ptr::null_mut())
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
        h.group.send_event(SynthEvent::AllChannels(ChannelEvent::Config(
            ChannelConfigEvent::SetSoundfonts(Vec::new()),
        )));

        let sf = match SampleSoundfont::new(pb, stream_params, opts) {
            Ok(sf) => sf,
            Err(e) => {
                set_last_error(format!("SampleSoundfont::new: {e:?}"));
                return -1;
            }
        };
        let arc: Arc<dyn SoundfontBase> = Arc::new(sf);
        h.group.send_event(SynthEvent::AllChannels(ChannelEvent::Config(
            ChannelConfigEvent::SetSoundfonts(vec![arc]),
        )));
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
        h.group.send_event(SynthEvent::Channel(
            ch as u32,
            ChannelEvent::Audio(ChannelAudioEvent::NoteOn { key, vel }),
        ));
    }));
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
    LAST_ERROR.with(|cell| {
        let msg = cell.borrow();
        let bytes = msg.as_bytes();
        let n = bytes.len().min(out_len - 1);
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), out_buf as *mut u8, n);
        *out_buf.add(n) = 0;
        n
    })
}

/// Cancel the in-flight load (if any) and join its thread. Drops the old
/// soundfont from the channel group before any new load kicks off so peak
/// memory stays bounded on a memory-tight device.
unsafe fn cancel_inflight(h: &mut XSynthHandle) {
    if let Some(mut w) = h.worker.take() {
        w.cancel.store(true, Ordering::Release);
        if let Some(jh) = w.handle.take() {
            let _ = jh.join();
        }
    }
}

/// Drop the currently-loaded soundfont. Used before kicking off a load so
/// new + old don't both live in RAM simultaneously.
unsafe fn drop_current_soundfont(h: &mut XSynthHandle) {
    h.group.send_event(SynthEvent::AllChannels(
        ChannelEvent::Audio(ChannelAudioEvent::AllNotesKilled),
    ));
    h.group.send_event(SynthEvent::AllChannels(ChannelEvent::Config(
        ChannelConfigEvent::SetSoundfonts(Vec::new()),
    )));
}

#[no_mangle]
pub unsafe extern "C" fn xshim_load_sfz_async(handle: *mut XSynthHandle, path: *const c_char) -> c_int {
    if handle.is_null() || path.is_null() { return -1; }
    let h = &mut *handle;

    let cstr = match CStr::from_ptr(path).to_str() {
        Ok(s) => s.to_string(),
        Err(_) => { set_last_error("path is not valid UTF-8"); return -1; }
    };

    // Stop any in-flight load + drop the current soundfont so peak memory
    // stays within budget while the new one loads.
    cancel_inflight(h);
    drop_current_soundfont(h);

    let status = Arc::new(AtomicU32::new(STATUS_LOADING));
    let cancel = Arc::new(AtomicBool::new(false));
    let result: Arc<Mutex<Option<SampleSoundfont>>> = Arc::new(Mutex::new(None));

    let status_t = status.clone();
    let cancel_t = cancel.clone();
    let result_t = result.clone();

    let join_handle = thread::Builder::new()
        .name("xshim-load".into())
        .spawn(move || {
            let stream_params = AudioStreamParams::new(44100, ChannelCount::Stereo);
            let opts = SoundfontInitOptions {
                bank: None, preset: None,
                vol_envelope_options: Default::default(),
                use_effects: true,
                interpolator: Interpolator::Linear,
            };
            let pb = PathBuf::from(&cstr);
            let load_res = catch_unwind(AssertUnwindSafe(|| {
                SampleSoundfont::new_sfz_cancellable(pb, stream_params, opts, Some(cancel_t))
            }));
            match load_res {
                Ok(Ok(sf)) => {
                    *result_t.lock().unwrap() = Some(sf);
                    status_t.store(STATUS_READY, Ordering::Release);
                }
                Ok(Err(e)) => {
                    let msg = format!("{e:?}");
                    set_last_error(format!("SampleSoundfont::new: {msg}"));
                    let is_cancel = matches!(e, xsynth_core::soundfont::LoadSfzError::Cancelled);
                    status_t.store(
                        if is_cancel { STATUS_CANCELLED } else { STATUS_ERROR },
                        Ordering::Release,
                    );
                }
                Err(_) => {
                    set_last_error("panic during load");
                    status_t.store(STATUS_ERROR, Ordering::Release);
                }
            }
        });

    match join_handle {
        Ok(jh) => {
            h.worker = Some(LoadWorker {
                handle: Some(jh),
                status,
                cancel,
                result,
            });
            0
        }
        Err(e) => {
            set_last_error(format!("thread spawn failed: {e}"));
            -1
        }
    }
}

/// 0 idle / 1 loading / 2 ready / 3 error / 4 cancelled. Always returns a
/// snapshot — caller can poll repeatedly.
#[no_mangle]
pub unsafe extern "C" fn xshim_load_status(handle: *const XSynthHandle) -> c_int {
    if handle.is_null() { return STATUS_IDLE as c_int; }
    let h = &*handle;
    match &h.worker {
        Some(w) => w.status.load(Ordering::Acquire) as c_int,
        None => STATUS_IDLE as c_int,
    }
}

/// Signal cancellation. Worker will return on its next sample boundary.
#[no_mangle]
pub unsafe extern "C" fn xshim_load_cancel(handle: *mut XSynthHandle) {
    if handle.is_null() { return; }
    let h = &mut *handle;
    if let Some(w) = &h.worker {
        w.cancel.store(true, Ordering::Release);
    }
}

/// Apply a ready soundfont into the channel group. Returns 0 on success,
/// -1 if no soundfont is ready. Resets worker state to Idle.
#[no_mangle]
pub unsafe extern "C" fn xshim_load_apply(handle: *mut XSynthHandle) -> c_int {
    if handle.is_null() { return -1; }
    let h = &mut *handle;
    let Some(w) = h.worker.as_mut() else { return -1; };
    if w.status.load(Ordering::Acquire) != STATUS_READY { return -1; }

    let sf_opt = w.result.lock().unwrap().take();
    let Some(sf) = sf_opt else { return -1; };

    let arc: Arc<dyn SoundfontBase> = Arc::new(sf);
    h.group.send_event(SynthEvent::AllChannels(ChannelEvent::Config(
        ChannelConfigEvent::SetSoundfonts(vec![arc]),
    )));

    // Reap the worker thread now that we've consumed its output.
    if let Some(jh) = w.handle.take() { let _ = jh.join(); }
    h.worker = None;
    0
}

/// Clear error / cancelled status so subsequent loads can fire cleanly.
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
