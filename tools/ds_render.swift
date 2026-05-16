// ds_render — host the DecentSampler AU offline, send MIDI, write WAV.
//
//   swift ds_render.swift <preset.dspreset> <output.wav> [note=60] [vel=100] \
//                         [duration_s=2.0] [tail_s=2.0] [rate=44100]
//
// Loads the DS AU (aumu Dsmp Dldy), tries to load the given .dspreset via
// the AU's fullState (key "DS_PRESET_PATH" — DS happens to honor this on
// load; if a future DS build changes the key, dump the state from a Logic
// session and inspect). Renders offline via AVAudioEngine.manualRendering.

import Foundation
import AVFoundation

// MARK: - args
let args = CommandLine.arguments
guard args.count >= 3 else {
    FileHandle.standardError.write("usage: ds_render <preset.dspreset> <out.wav> [note] [vel] [dur_s] [tail_s] [rate]\n".data(using: .utf8)!)
    exit(1)
}
let presetPath = (args[1] as NSString).expandingTildeInPath
let outPath    = (args[2] as NSString).expandingTildeInPath
// chdir to the preset's directory so DS's relative sample-path lookup
// finds the .wav files (DS stores `_samplePath` as a working-directory
// hint; it falls back to cwd otherwise).
let presetDir = (presetPath as NSString).deletingLastPathComponent
FileManager.default.changeCurrentDirectoryPath(presetDir)
let note: UInt8 = UInt8(args.count > 3 ? Int(args[3]) ?? 60 : 60)
let vel:  UInt8 = UInt8(args.count > 4 ? Int(args[4]) ?? 100 : 100)
let durS:    Double = args.count > 5 ? Double(args[5]) ?? 2.0 : 2.0
let tailS:   Double = args.count > 6 ? Double(args[6]) ?? 2.0 : 2.0
let sampleRate: Double = args.count > 7 ? Double(args[7]) ?? 44100 : 44100

// MARK: - load DS AU
let desc = AudioComponentDescription(
    componentType: kAudioUnitType_MusicDevice,
    componentSubType: 0x44736D70,   // 'Dsmp'
    componentManufacturer: 0x446C6479, // 'Dldy'
    componentFlags: 0, componentFlagsMask: 0)

let engine = AVAudioEngine()
let semaphore = DispatchSemaphore(value: 0)
var dsUnit: AVAudioUnit?
var loadError: Error?
AVAudioUnit.instantiate(with: desc, options: []) { unit, err in
    dsUnit = unit
    loadError = err
    semaphore.signal()
}
semaphore.wait()
guard let ds = dsUnit, loadError == nil else {
    FileHandle.standardError.write("instantiate failed: \(String(describing: loadError))\n".data(using: .utf8)!)
    exit(2)
}

engine.attach(ds)
let stereoFormat = AVAudioFormat(standardFormatWithSampleRate: sampleRate, channels: 2)!
engine.connect(ds, to: engine.mainMixerNode, format: stereoFormat)

// MARK: - load the dspreset via JUCE plugin state
// DS is a JUCE plugin; its state lives under fullState key
// "jucePluginState" as a 'VC2!'-magic blob containing XML where
// `_libraryUrl` holds the file URL of the .dspreset. Build the XML,
// wrap it in the JUCE blob format, drop it on the AU.
let fileURL = URL(fileURLWithPath: presetPath)
let urlString = fileURL.absoluteString
// Generate a security-scoped bookmark — DS requires one to actually
// follow the URL (an empty `_libraryBookmark` makes it reset URL=""
// silently). Bookmarks are file URL → opaque data; base64-encoded
// into the XML attribute.
var bookmarkB64 = ""
do {
    let bd = try fileURL.bookmarkData(options: [.withSecurityScope],
                                      includingResourceValuesForKeys: nil,
                                      relativeTo: nil)
    bookmarkB64 = bd.base64EncodedString()
} catch {
    FileHandle.standardError.write("bookmark create failed: \(error)\n".data(using: .utf8)!)
}
let xml = """
<?xml version="1.0" encoding="UTF-8"?> <DecentSampler _presetName="\(fileURL.lastPathComponent)" _libraryUrl="\(urlString)" _libraryCanonicalUrl="\(urlString)" _samplePath="" _sampleLibraryId="0" _libraryBookmark="\(bookmarkB64)" _tuningA69Frequency="440.0" _velocityPreprocessorOutLow="0" _velocityPreprocessorOutHigh="127" _velocityPreprocessorDrive="0.0" _velocityPreprocessorCompression="0.0" _velocityPreprocessorRandom="0" _mpeTimbreSensitivity="0.0" _mpeTimbreMin="0.0" _mpeTimbreMax="1.0" _mpePressureSensitivity="0.0" _mpePressureMin="0.0" _mpePressureMax="1.0"><ui/><effects/><groups/><tags/><buses/><midi/><modulators/><noteSequences/></DecentSampler>
"""
let xmlBytes = Array(xml.utf8)
var jucePluginState = Data()
jucePluginState.append(contentsOf: [0x56, 0x43, 0x32, 0x21])  // 'VC2!'
var xmlLen = UInt32(xmlBytes.count).littleEndian
withUnsafeBytes(of: &xmlLen) { jucePluginState.append(contentsOf: $0) }
jucePluginState.append(contentsOf: xmlBytes)

let cls: [String: Any] = [
    "type":            Int(desc.componentType),
    "subtype":         Int(desc.componentSubType),
    "manufacturer":    Int(desc.componentManufacturer),
    "version":         0,
    "data":            Data(),
    "jucePluginState": jucePluginState,
    "name":            "Untitled",
]
// Defer setting state until AFTER engine.start() — starting the engine
// re-initializes the AU and would otherwise wipe whatever we set.
// (Marker — actual set happens below.)

// MARK: - prepare offline rendering
let outURL = URL(fileURLWithPath: outPath)
let outSettings: [String: Any] = [
    AVFormatIDKey: kAudioFormatLinearPCM,
    AVLinearPCMBitDepthKey: 16,
    AVLinearPCMIsBigEndianKey: false,
    AVLinearPCMIsFloatKey: false,
    AVLinearPCMIsNonInterleaved: false,
    AVSampleRateKey: sampleRate,
    AVNumberOfChannelsKey: 2,
]
// Use an optional so we can release it before exit to flush the WAV
// header (AVAudioFile only finalizes RIFF size on deinit).
var outFile: AVAudioFile? = nil
do {
    outFile = try AVAudioFile(forWriting: outURL, settings: outSettings,
                              commonFormat: .pcmFormatFloat32, interleaved: false)
} catch {
    FileHandle.standardError.write("open out: \(error)\n".data(using: .utf8)!)
    exit(3)
}

let blockFrames: AVAudioFrameCount = 128
do {
    try engine.enableManualRenderingMode(.offline,
        format: stereoFormat,
        maximumFrameCount: blockFrames)
} catch {
    FileHandle.standardError.write("manualRendering: \(error)\n".data(using: .utf8)!)
    exit(4)
}
try? engine.start()

// NOW apply the JUCE state, so engine.start()'s AU init doesn't wipe it.
ds.auAudioUnit.fullState = cls
Thread.sleep(forTimeInterval: 5.0)
if let back = ds.auAudioUnit.fullState,
   let bjuce = back["jucePluginState"] as? Data {
    let bxml = bjuce.subdata(in: 8..<bjuce.count)
    try? bxml.write(to: URL(fileURLWithPath: "/tmp/ds_state_after.xml"))
    FileHandle.standardError.write("[ds_render] dumped state (\(bxml.count) bytes)\n".data(using: .utf8)!)
}

// MARK: - MIDI scheduling helper
// Prefer the AUv3 scheduleMIDIEventListBlock (matches how a modern host
// like Logic delivers MIDI), fall back to MusicDeviceMIDIEvent.
let mdUnit = ds.audioUnit
typealias MDMIDIEventFn = @convention(c) (AudioUnit, UInt32, UInt32, UInt32, UInt32) -> OSStatus
let musicDeviceMIDIEvent: MDMIDIEventFn = MusicDeviceMIDIEvent
let midiListBlock = ds.auAudioUnit.scheduleMIDIEventListBlock

func sendMIDI(_ status: UInt8, _ d1: UInt8, _ d2: UInt8) {
    if let block = midiListBlock {
        var words: [UInt32] = [UInt32(status) << 16 | UInt32(d1) << 8 | UInt32(d2)]
        words.withUnsafeMutableBufferPointer { buf in
            let raw = UnsafeMutableRawPointer(buf.baseAddress!)
            // MIDIEventList header: numPackets, then packets (timestamp + wordCount + words[])
            var list = MIDIEventList()
            list.protocol = ._1_0
            list.numPackets = 1
            list.packet.timeStamp = 0
            list.packet.wordCount = 1
            withUnsafeMutablePointer(to: &list.packet.words) { wp in
                wp.withMemoryRebound(to: UInt32.self, capacity: 64) { p in
                    p[0] = UInt32(status) << 16 | UInt32(d1) << 8 | UInt32(d2)
                }
            }
            _ = raw  // suppress unused warning
            _ = block(AUEventSampleTimeImmediate, 0, &list)
        }
    } else {
        _ = musicDeviceMIDIEvent(mdUnit, UInt32(status), UInt32(d1), UInt32(d2), 0)
    }
}

// MARK: - render
let renderBuf = AVAudioPCMBuffer(pcmFormat: stereoFormat, frameCapacity: blockFrames)!
let noteFrames: Int64 = Int64(durS  * sampleRate)
let tailFrames: Int64 = Int64(tailS * sampleRate)
let totalFrames: Int64 = noteFrames + tailFrames

// Pre-roll: render ~1s of blocks WITHOUT MIDI so DS can finish its
// async preset / sample load before we send the NoteOn. The block
// rendering drains JUCE's audio-thread callbacks which is what
// actually advances the loader. Output discarded.
let preRollFrames = Int64(sampleRate * 1.0)
var preRolled: Int64 = 0
while preRolled < preRollFrames {
    let want = min(AVAudioFrameCount(preRollFrames - preRolled), blockFrames)
    _ = try engine.renderOffline(want, to: renderBuf)
    preRolled += Int64(want)
}

// NoteOn now (BEFORE the first scored render block).
sendMIDI(0x90, note, vel)

var rendered: Int64 = 0
var noteReleased = false
while rendered < totalFrames {
    if !noteReleased && rendered >= noteFrames {
        sendMIDI(0x80, note, 0)
        noteReleased = true
    }
    let want = min(AVAudioFrameCount(totalFrames - rendered), blockFrames)
    do {
        let st = try engine.renderOffline(want, to: renderBuf)
        if st != .success { break }
        try outFile!.write(from: renderBuf)
        rendered += Int64(want)
    } catch {
        FileHandle.standardError.write("render: \(error)\n".data(using: .utf8)!)
        exit(5)
    }
}

engine.stop()
// Release AVAudioFile so its deinit flushes the RIFF size into the
// WAV header. Without this, the file's RIFF size stays at 0 (whatever
// it was when first opened) and `wave` / `afinfo` see 0 frames even
// though all the audio bytes are on disk.
outFile = nil
FileHandle.standardError.write("[ds_render] wrote \(rendered) frames to \(outPath)\n".data(using: .utf8)!)
