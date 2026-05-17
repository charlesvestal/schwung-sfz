// Dump DS AU full juce state as XML to stdout.
import Foundation
import AVFoundation

let desc = AudioComponentDescription(
    componentType: kAudioUnitType_MusicDevice,
    componentSubType: 0x44736D70, componentManufacturer: 0x446C6479,
    componentFlags: 0, componentFlagsMask: 0)
let sem = DispatchSemaphore(value: 0)
var u: AVAudioUnit?
AVAudioUnit.instantiate(with: desc, options: []) { unit, _ in u = unit; sem.signal() }
sem.wait()
guard let ds = u else { exit(1) }
guard let st = ds.auAudioUnit.fullState else { exit(2) }
if let data = st["jucePluginState"] as? Data {
    // JUCE state header: 4-byte magic ('VC2!') + 4-byte length + XML
    let start = 8
    if data.count > start {
        let xml = data.subdata(in: start..<data.count)
        FileHandle.standardOutput.write(xml)
    }
}
