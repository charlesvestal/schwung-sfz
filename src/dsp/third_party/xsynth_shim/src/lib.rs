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
use std::sync::Arc;

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

pub struct XSynthHandle {
    group: ChannelGroup,
}

#[no_mangle]
pub unsafe extern "C" fn xshim_create(sample_rate: u32, channels: u32) -> *mut XSynthHandle {
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
        Box::into_raw(Box::new(XSynthHandle { group }))
    }))
    .unwrap_or(ptr::null_mut())
}

#[no_mangle]
pub unsafe extern "C" fn xshim_destroy(handle: *mut XSynthHandle) {
    if handle.is_null() { return; }
    let _ = catch_unwind(AssertUnwindSafe(|| { drop(Box::from_raw(handle)); }));
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
        Err(_) => { eprintln!("[xshim] xshim_load_sfz panicked"); -1 }
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

#[no_mangle]
pub unsafe extern "C" fn xshim_voice_count(handle: *const XSynthHandle) -> u64 {
    if handle.is_null() { return 0; }
    let h = &*handle;
    h.group.voice_count()
}
