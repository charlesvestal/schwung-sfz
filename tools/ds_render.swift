// ds_render — drive DecentSampler AU directly via AudioUnit C APIs.
// AVAudioEngine.manualRendering doesn't advance JUCE's async preset
// loader; using the C API + CFRunLoop pumping + an "OfflineRender"
// hint does.
//
//   swift ds_render.swift <preset.dspreset> <out.wav> [note=60] [vel=100] \
//                          [duration_s=2.0] [tail_s=2.0] [rate=44100]

import Foundation
import AudioToolbox
import AVFoundation

let args = CommandLine.arguments
guard args.count >= 3 else {
    FileHandle.standardError.write("usage: ds_render <preset.dspreset> <out.wav>...\n".data(using: .utf8)!)
    exit(1)
}
let presetPath = (args[1] as NSString).expandingTildeInPath
let outPath    = (args[2] as NSString).expandingTildeInPath
let note: UInt8 = UInt8(args.count > 3 ? Int(args[3]) ?? 60 : 60)
let vel:  UInt8 = UInt8(args.count > 4 ? Int(args[4]) ?? 100 : 100)
let durS:  Double = args.count > 5 ? Double(args[5]) ?? 2.0 : 2.0
let tailS: Double = args.count > 6 ? Double(args[6]) ?? 2.0 : 2.0
let sampleRate: Double = args.count > 7 ? Double(args[7]) ?? 44100 : 44100

let presetDir = (presetPath as NSString).deletingLastPathComponent
FileManager.default.changeCurrentDirectoryPath(presetDir)

// ----- 1) instantiate DS AU directly via the C API -----
var desc = AudioComponentDescription(
    componentType: kAudioUnitType_MusicDevice,
    componentSubType: 0x44736D70,
    componentManufacturer: 0x446C6479,
    componentFlags: 0, componentFlagsMask: 0)
guard let comp = AudioComponentFindNext(nil, &desc) else {
    FileHandle.standardError.write("DS AU not found\n".data(using: .utf8)!)
    exit(2)
}
var unitOpt: AudioUnit? = nil
var st = AudioComponentInstanceNew(comp, &unitOpt)
guard st == noErr, let unit = unitOpt else {
    FileHandle.standardError.write("instance new: \(st)\n".data(using: .utf8)!)
    exit(3)
}

// Hint offline render (some AUs disable the audio-thread requirement here).
var off: UInt32 = 1
_ = AudioUnitSetProperty(unit, kAudioUnitProperty_OfflineRender,
                          kAudioUnitScope_Global, 0,
                          &off, UInt32(MemoryLayout<UInt32>.size))

// ----- 2) format: NON-interleaved stereo (CoreAudio's canonical AU layout) -----
var fmt = AudioStreamBasicDescription(
    mSampleRate: sampleRate,
    mFormatID: kAudioFormatLinearPCM,
    mFormatFlags: kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
                | kAudioFormatFlagIsNonInterleaved,
    mBytesPerPacket: 4, mFramesPerPacket: 1,
    mBytesPerFrame: 4, mChannelsPerFrame: 2,
    mBitsPerChannel: 32, mReserved: 0)
st = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                           kAudioUnitScope_Output, 0,
                           &fmt, UInt32(MemoryLayout<AudioStreamBasicDescription>.size))
guard st == noErr else {
    FileHandle.standardError.write("set format: \(st)\n".data(using: .utf8)!)
    exit(4)
}

var maxFrames: UInt32 = 4096
_ = AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice,
                          kAudioUnitScope_Global, 0,
                          &maxFrames, UInt32(MemoryLayout<UInt32>.size))

st = AudioUnitInitialize(unit)
guard st == noErr else {
    FileHandle.standardError.write("AudioUnitInitialize: \(st)\n".data(using: .utf8)!)
    exit(5)
}

// ----- 3) set state pointing at our preset -----
let fileURL = URL(fileURLWithPath: presetPath)
let urlString = fileURL.absoluteString
var bookmarkB64 = ""
if let bd = try? fileURL.bookmarkData(options: [.withSecurityScope],
                                     includingResourceValuesForKeys: nil,
                                     relativeTo: nil) {
    bookmarkB64 = bd.base64EncodedString()
}
let xml = """
<?xml version="1.0" encoding="UTF-8"?> <DecentSampler _presetName="\(fileURL.lastPathComponent)" _libraryUrl="\(urlString)" _libraryCanonicalUrl="\(urlString)" _samplePath="\(presetDir)" _sampleLibraryId="0" _libraryBookmark="\(bookmarkB64)" _tuningA69Frequency="440.0" _velocityPreprocessorOutLow="0" _velocityPreprocessorOutHigh="127" _velocityPreprocessorDrive="0.0" _velocityPreprocessorCompression="0.0" _velocityPreprocessorRandom="0" _mpeTimbreSensitivity="0.0" _mpeTimbreMin="0.0" _mpeTimbreMax="1.0" _mpePressureSensitivity="0.0" _mpePressureMin="0.0" _mpePressureMax="1.0"><ui/><effects/><groups/><tags/><buses/><midi/><modulators/><noteSequences/></DecentSampler>
"""
let xmlBytes = Array(xml.utf8)
var juceData = Data([0x56, 0x43, 0x32, 0x21])
var xmlLen = UInt32(xmlBytes.count).littleEndian
withUnsafeBytes(of: &xmlLen) { juceData.append(contentsOf: $0) }
juceData.append(contentsOf: xmlBytes)

let dict: NSDictionary = [
    "type":            Int(desc.componentType),
    "subtype":         Int(desc.componentSubType),
    "manufacturer":    Int(desc.componentManufacturer),
    "version":         0,
    "data":            NSData(),
    "jucePluginState": juceData as NSData,
    "name":            "Untitled",
]
var dictRef: CFPropertyList? = dict as CFPropertyList
let setSt = AudioUnitSetProperty(unit, kAudioUnitProperty_ClassInfo,
                                  kAudioUnitScope_Global, 0,
                                  &dictRef,
                                  UInt32(MemoryLayout<CFPropertyList?>.size))
FileHandle.standardError.write("[ds_render] ClassInfo set: \(setSt)\n".data(using: .utf8)!)

// ----- 4) pump the run loop so JUCE's async loader fires -----
let loadDeadline = Date().addingTimeInterval(8.0)
while Date() < loadDeadline {
    RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.05))
}

// Diagnostic dump.
var classOut: CFPropertyList? = nil
var classSize = UInt32(MemoryLayout<CFPropertyList?>.size)
let getSt = AudioUnitGetProperty(unit, kAudioUnitProperty_ClassInfo,
                                  kAudioUnitScope_Global, 0,
                                  &classOut, &classSize)
if getSt == noErr, let d = classOut as? [String: Any],
   let jd = d["jucePluginState"] as? Data {
    let bx = jd.subdata(in: 8..<jd.count)
    try? bx.write(to: URL(fileURLWithPath: "/tmp/ds_state_after.xml"))
    let s = String(data: bx, encoding: .utf8) ?? ""
    FileHandle.standardError.write("[ds_render] state-after (\(bx.count)B): \(String(s.prefix(200)))\n".data(using: .utf8)!)
}

// ----- 5) output WAV -----
let stereoFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                               sampleRate: sampleRate, channels: 2,
                               interleaved: false)!
let outSettings: [String: Any] = [
    AVFormatIDKey: kAudioFormatLinearPCM,
    AVLinearPCMBitDepthKey: 16,
    AVLinearPCMIsBigEndianKey: false,
    AVLinearPCMIsFloatKey: false,
    AVLinearPCMIsNonInterleaved: false,
    AVSampleRateKey: sampleRate,
    AVNumberOfChannelsKey: 2,
]
var outFile: AVAudioFile? = try AVAudioFile(forWriting: URL(fileURLWithPath: outPath),
                                             settings: outSettings,
                                             commonFormat: .pcmFormatFloat32,
                                             interleaved: false)

// ----- 6) render via AudioUnitRender. Non-interleaved → 2 buffers. -----
let block: UInt32 = 256
let chBytes = Int(block) * 4  // 1 ch × 4 bytes
let rawL = UnsafeMutableRawPointer.allocate(byteCount: chBytes, alignment: 4)
let rawR = UnsafeMutableRawPointer.allocate(byteCount: chBytes, alignment: 4)
defer { rawL.deallocate(); rawR.deallocate() }
let abl = AudioBufferList.allocate(maximumBuffers: 2)
defer { abl.unsafeMutablePointer.deallocate() }
abl[0] = AudioBuffer(mNumberChannels: 1, mDataByteSize: UInt32(chBytes), mData: rawL)
abl[1] = AudioBuffer(mNumberChannels: 1, mDataByteSize: UInt32(chBytes), mData: rawR)

var ts = AudioTimeStamp()
ts.mFlags = .sampleTimeValid
ts.mSampleTime = 0
var flags: AudioUnitRenderActionFlags = []

func sendMIDI(_ status: UInt8, _ d1: UInt8, _ d2: UInt8) {
    _ = MusicDeviceMIDIEvent(unit, UInt32(status), UInt32(d1), UInt32(d2), 0)
}

let preFrames = Int64(sampleRate * 1.0)
let noteFrames = Int64(durS * sampleRate)
let tailFrames = Int64(tailS * sampleRate)

let pcmBuf = AVAudioPCMBuffer(pcmFormat: stereoFmt, frameCapacity: block)!

var pre: Int64 = 0
while pre < preFrames {
    let want = UInt32(min(Int64(block), preFrames - pre))
    _ = AudioUnitRender(unit, &flags, &ts, 0, want, abl.unsafeMutablePointer)
    ts.mSampleTime += Double(want)
    pre += Int64(want)
    RunLoop.current.run(mode: .default, before: Date())
}

sendMIDI(0x90, note, vel)

var rendered: Int64 = 0
var released = false
let total = noteFrames + tailFrames
while rendered < total {
    if !released && rendered >= noteFrames {
        sendMIDI(0x80, note, 0)
        released = true
    }
    let want = UInt32(min(Int64(block), total - rendered))
    let rs = AudioUnitRender(unit, &flags, &ts, 0, want, abl.unsafeMutablePointer)
    if rs != noErr {
        FileHandle.standardError.write("render: \(rs)\n".data(using: .utf8)!)
        break
    }
    ts.mSampleTime += Double(want)
    pcmBuf.frameLength = want
    let dstL = pcmBuf.floatChannelData![0]
    let dstR = pcmBuf.floatChannelData![1]
    let srcL = rawL.bindMemory(to: Float.self, capacity: Int(want))
    let srcR = rawR.bindMemory(to: Float.self, capacity: Int(want))
    for i in 0..<Int(want) { dstL[i] = srcL[i]; dstR[i] = srcR[i] }
    try? outFile?.write(from: pcmBuf)
    rendered += Int64(want)
}

AudioUnitUninitialize(unit)
AudioComponentInstanceDispose(unit)
outFile = nil
FileHandle.standardError.write("[ds_render] wrote \(rendered) frames to \(outPath)\n".data(using: .utf8)!)
